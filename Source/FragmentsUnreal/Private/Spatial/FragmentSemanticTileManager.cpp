#include "Spatial/FragmentSemanticTileManager.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "DrawDebugHelpers.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

DEFINE_LOG_CATEGORY_STATIC(LogSemanticTiles, Log, All);

UFragmentSemanticTileManager::UFragmentSemanticTileManager()
{
}

void UFragmentSemanticTileManager::Initialize(const FString& InModelGuid, UFragmentsImporter* InImporter)
{
	if (!InImporter)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("Initialize: Null importer"));
		return;
	}

	ModelGuid = InModelGuid;
	Importer = InImporter;
	RootActor = InImporter->GetOwnerRef(); // Store root actor for component attachment

	UE_LOG(LogSemanticTiles, Log, TEXT("Initialized Semantic Tile Manager for model: %s"), *ModelGuid);
}

void UFragmentSemanticTileManager::BuildSemanticTiles()
{
	if (!Importer)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("BuildSemanticTiles: No importer"));
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Get model wrapper
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("BuildSemanticTiles: Model wrapper not found for GUID %s"), *ModelGuid);
		return;
	}

	// Get root item
	const FFragmentItem& RootItem = Wrapper->GetModelItemRef();

	// Clear existing tiles
	SemanticTileMap.Empty();
	AllSemanticTiles.Empty();
	for (int32 i = 0; i < 4; i++)
	{
		TilesByPriority[i].Empty();
	}

	// Recursively collect all fragments and group by IFC class
	TArray<const FFragmentItem*> AllFragments;
	TFunction<void(const FFragmentItem&)> CollectFragments = [&](const FFragmentItem& Item)
	{
		AllFragments.Add(&Item);
		for (const FFragmentItem* Child : Item.FragmentChildren)
		{
			if (Child)
			{
				CollectFragments(*Child);
			}
		}
	};
	CollectFragments(RootItem);

	if (Config.bEnableDebugLogging)
	{
		UE_LOG(LogSemanticTiles, Log, TEXT("Collected %d total fragments"), AllFragments.Num());
	}

	// Group fragments by IFC class
	for (const FFragmentItem* Item : AllFragments)
	{
		FString IFCClass = ExtractIFCClass(Item->LocalId);

		// Get or create semantic tile for this IFC class
		USemanticTile** TilePtr = SemanticTileMap.Find(IFCClass);
		USemanticTile* Tile = nullptr;

		if (!TilePtr)
		{
			// Create new semantic tile
			Tile = NewObject<USemanticTile>(this);
			Tile->IFCClassName = IFCClass;
			Tile->Priority = DeterminePriority(IFCClass);
			Tile->RepresentativeColor = GetRepresentativeColor(IFCClass);
			Tile->CombinedBounds = FBox(ForceInit);

			SemanticTileMap.Add(IFCClass, Tile);
			AllSemanticTiles.Add(Tile);
			TilesByPriority[static_cast<int32>(Tile->Priority)].Add(Tile);

			if (Config.bEnableDebugLogging)
			{
				UE_LOG(LogSemanticTiles, Verbose, TEXT("Created semantic tile for IFC class: %s (Priority: %d)"),
				       *IFCClass, static_cast<int32>(Tile->Priority));
			}
		}
		else
		{
			Tile = *TilePtr;
		}

		// Add fragment to tile
		Tile->FragmentIDs.Add(Item->LocalId);
		Tile->Count++;
	}

	// Calculate combined bounds for each tile
	for (USemanticTile* Tile : AllSemanticTiles)
	{
		CalculateCombinedBounds(Tile);
	}

	// Phase 4: Build spatial subdivision for each tile
	if (Config.bEnableSpatialSubdivision)
	{
		for (USemanticTile* Tile : AllSemanticTiles)
		{
			BuildSpatialSubdivision(Tile);
		}
	}

	const double ElapsedTime = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	UE_LOG(LogSemanticTiles, Log, TEXT("Built %d semantic tiles from %d fragments in %.2f ms"),
	       AllSemanticTiles.Num(), AllFragments.Num(), ElapsedTime);

	// Log summary by priority
	for (int32 i = 0; i < 4; i++)
	{
		int32 TileCount = TilesByPriority[i].Num();
		int32 FragmentCount = 0;
		for (USemanticTile* Tile : TilesByPriority[i])
		{
			FragmentCount += Tile->Count;
		}

		const TCHAR* PriorityNames[] = { TEXT("Structural"), TEXT("Openings"), TEXT("Furnishings"), TEXT("Details") };
		UE_LOG(LogSemanticTiles, Log, TEXT("  Priority %d (%s): %d tiles, %d fragments"),
		       i, PriorityNames[i], TileCount, FragmentCount);
	}
}

void UFragmentSemanticTileManager::Tick(float DeltaTime, const FVector& CameraLocation,
                                         const FRotator& CameraRotation, float FOV, float ViewportHeight)
{
	// Phase 2: LOD system implementation
	// Calculate screen coverage for each semantic tile and transition LODs

	if (!Importer)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();

	// Debug: Log first tick and periodically log LOD states
	static bool bFirstTick = true;
	static double LastDebugTime = 0.0;
	const double DebugInterval = 2.0; // Log every 2 seconds

	if (bFirstTick)
	{
		UE_LOG(LogSemanticTiles, Log, TEXT("First Tick called - %d semantic tiles, %d priority levels"),
		       AllSemanticTiles.Num(), 4);
		UE_LOG(LogSemanticTiles, Log, TEXT("Camera: Loc=%s, FOV=%.1f, ViewportH=%.1f"),
		       *CameraLocation.ToString(), FOV, ViewportHeight);
		bFirstTick = false;
		LastDebugTime = CurrentTime;
	}

	// Periodic debug logging
	bool bDoPeriodicDebug = (CurrentTime - LastDebugTime) >= DebugInterval;
	if (bDoPeriodicDebug)
	{
		UE_LOG(LogSemanticTiles, Log, TEXT("=== Semantic Tile LOD Status (Camera: %s, Rotation: %s) ==="),
		       *CameraLocation.ToString(), *CameraRotation.ToString());
		LastDebugTime = CurrentTime;
	}

	// Process tiles by priority (structural first, details last)
	for (int32 PriorityIdx = 0; PriorityIdx < 4; PriorityIdx++)
	{
		for (USemanticTile* Tile : TilesByPriority[PriorityIdx])
		{
			if (!Tile)
			{
				continue;
			}

			// Phase 4: Process sub-tiles recursively if spatial subdivision is enabled
			if (Config.bEnableSpatialSubdivision && Tile->SpatialSubTiles.Num() > 0)
			{
				// Recursively process sub-tiles starting from root
				TFunction<void(int32)> ProcessSubTileRecursive = [&](int32 SubTileIndex)
				{
					if (SubTileIndex < 0 || SubTileIndex >= Tile->SpatialSubTiles.Num())
					{
						return;
					}

					FSemanticSubTile& SubTile = Tile->SpatialSubTiles[SubTileIndex];

					// Update LOD for this sub-tile
					UpdateSubTileLOD(Tile, SubTile, CameraLocation);

					// Periodic debug logging (only for root sub-tiles of structural tiles)
					if (bDoPeriodicDebug && PriorityIdx == 0 && SubTile.Depth == 0)
					{
						FVector SubTileCenter = SubTile.Bounds.GetCenter();
						float Distance = FVector::Dist(CameraLocation, SubTileCenter);
						UE_LOG(LogSemanticTiles, Log, TEXT("  %s: Depth=%d, Frags=%d, Coverage=%.4f%%, Dist=%.1fcm, LOD=%d → %d"),
						       *Tile->IFCClassName, SubTile.Depth, SubTile.FragmentIDs.Num(),
						       SubTile.ScreenCoverage * 100.0f, Distance,
						       static_cast<int32>(SubTile.CurrentLOD), static_cast<int32>(SubTile.TargetLOD));
					}

					// Process child sub-tiles
					for (int32 i = 0; i < 8; i++)
					{
						if (SubTile.ChildIndices[i] != -1)
						{
							ProcessSubTileRecursive(SubTile.ChildIndices[i]);
						}
					}
				};

				// Start recursive processing from root sub-tile
				ProcessSubTileRecursive(Tile->RootSubTileIndex);
			}
			else
			{
				// Phase 2: Legacy whole-tile LOD processing (no spatial subdivision)
				// Calculate screen coverage
				float ScreenCoverage = CalculateScreenCoverage(Tile, CameraLocation, CameraRotation,
				                                                FOV, ViewportHeight);
				Tile->ScreenCoverage = ScreenCoverage;
				Tile->LastUpdateTime = CurrentTime;

				// Determine target LOD
				ESemanticLOD TargetLOD = DetermineLODLevel(ScreenCoverage);
				Tile->TargetLOD = TargetLOD;

				// Periodic debug logging
				if (bDoPeriodicDebug && PriorityIdx == 0) // Only log structural tiles to avoid spam
				{
					FVector TileCenter = Tile->CombinedBounds.GetCenter();
					float Distance = FVector::Dist(CameraLocation, TileCenter);
					UE_LOG(LogSemanticTiles, Log, TEXT("  %s: Coverage=%.4f%%, Dist=%.1fcm, LOD=%d → %d"),
					       *Tile->IFCClassName, ScreenCoverage * 100.0f, Distance,
					       static_cast<int32>(Tile->CurrentLOD), static_cast<int32>(TargetLOD));
				}

				// Transition to target LOD if different from current
				if (Tile->CurrentLOD != TargetLOD)
				{
					TransitionToLOD(Tile, TargetLOD);
				}

				// CRITICAL FIX: Redraw wireframe every frame (DrawDebugBox only lasts one frame)
				if (Tile->CurrentLOD == ESemanticLOD::Wireframe)
				{
					ShowWireframe(Tile);
				}
			}
		}
	}

	// Draw debug bounds if enabled (in addition to LOD visualization)
	if (Config.bDrawDebugBounds && Importer)
	{
		AActor* Owner = Importer->GetOwnerRef();
		if (Owner)
		{
			UWorld* World = Owner->GetWorld();
			if (World)
			{
				for (USemanticTile* Tile : AllSemanticTiles)
				{
					// Draw white debug bounds (separate from LOD wireframe)
					DrawDebugBox(World, Tile->CombinedBounds.GetCenter(),
					            Tile->CombinedBounds.GetExtent(), FColor::White,
					            false, -1.0f, 0, 1.0f);
				}
			}
		}
	}
}

USemanticTile* UFragmentSemanticTileManager::GetSemanticTile(const FString& IFCClassName) const
{
	USemanticTile* const* TilePtr = SemanticTileMap.Find(IFCClassName);
	return TilePtr ? *TilePtr : nullptr;
}

TArray<USemanticTile*> UFragmentSemanticTileManager::GetTilesByPriority(EFragmentPriority Priority) const
{
	int32 Index = static_cast<int32>(Priority);
	if (Index >= 0 && Index < 4)
	{
		return TilesByPriority[Index];
	}
	return TArray<USemanticTile*>();
}

int32 UFragmentSemanticTileManager::GetTotalFragmentCount() const
{
	int32 TotalCount = 0;
	for (USemanticTile* Tile : AllSemanticTiles)
	{
		TotalCount += Tile->Count;
	}
	return TotalCount;
}

FString UFragmentSemanticTileManager::ExtractIFCClass(int32 LocalID)
{
	if (!Importer)
	{
		return TEXT("Unknown");
	}

	// Get fragment item
	FFragmentItem* Item = Importer->GetFragmentItemByLocalId(LocalID, ModelGuid);
	if (!Item)
	{
		return TEXT("Unknown");
	}

	// Extract IFC class from category string
	// Category format is typically "IfcWall", "IfcWindow", etc.
	FString Category = Item->Category;
	if (Category.IsEmpty())
	{
		return TEXT("Unknown");
	}

	// Return the category as the IFC class name
	return Category;
}

EFragmentPriority UFragmentSemanticTileManager::DeterminePriority(const FString& IFCClassName)
{
	// Structural elements (highest priority - load first)
	if (IFCClassName.Contains(TEXT("Wall")) ||
	    IFCClassName.Contains(TEXT("Floor")) ||
	    IFCClassName.Contains(TEXT("Roof")) ||
	    IFCClassName.Contains(TEXT("Slab")) ||
	    IFCClassName.Contains(TEXT("Beam")) ||
	    IFCClassName.Contains(TEXT("Column")))
	{
		return EFragmentPriority::STRUCTURAL;
	}

	// Openings (second priority)
	if (IFCClassName.Contains(TEXT("Window")) ||
	    IFCClassName.Contains(TEXT("Door")) ||
	    IFCClassName.Contains(TEXT("Opening")) ||
	    IFCClassName.Contains(TEXT("CurtainWall")))
	{
		return EFragmentPriority::OPENINGS;
	}

	// Furnishings (third priority)
	if (IFCClassName.Contains(TEXT("Furniture")) ||
	    IFCClassName.Contains(TEXT("Fixture")) ||
	    IFCClassName.Contains(TEXT("Equipment")) ||
	    IFCClassName.Contains(TEXT("FurnishingElement")))
	{
		return EFragmentPriority::FURNISHINGS;
	}

	// Details (lowest priority - load last)
	return EFragmentPriority::DETAILS;
}

FLinearColor UFragmentSemanticTileManager::GetRepresentativeColor(const FString& IFCClassName)
{
	// Assign colors based on IFC class for visual distinction in wireframe LOD

	// Structural - gray tones
	if (IFCClassName.Contains(TEXT("Wall"))) return FLinearColor(0.7f, 0.7f, 0.7f);
	if (IFCClassName.Contains(TEXT("Floor"))) return FLinearColor(0.5f, 0.5f, 0.5f);
	if (IFCClassName.Contains(TEXT("Roof"))) return FLinearColor(0.6f, 0.4f, 0.4f);
	if (IFCClassName.Contains(TEXT("Slab"))) return FLinearColor(0.5f, 0.5f, 0.5f);
	if (IFCClassName.Contains(TEXT("Beam"))) return FLinearColor(0.8f, 0.5f, 0.3f);
	if (IFCClassName.Contains(TEXT("Column"))) return FLinearColor(0.8f, 0.5f, 0.3f);

	// Openings - blue tones
	if (IFCClassName.Contains(TEXT("Window"))) return FLinearColor(0.3f, 0.6f, 0.9f);
	if (IFCClassName.Contains(TEXT("Door"))) return FLinearColor(0.5f, 0.4f, 0.7f);
	if (IFCClassName.Contains(TEXT("CurtainWall"))) return FLinearColor(0.4f, 0.7f, 1.0f);

	// Furnishings - green/yellow tones
	if (IFCClassName.Contains(TEXT("Furniture"))) return FLinearColor(0.5f, 0.7f, 0.3f);
	if (IFCClassName.Contains(TEXT("Fixture"))) return FLinearColor(0.7f, 0.7f, 0.4f);
	if (IFCClassName.Contains(TEXT("Equipment"))) return FLinearColor(0.6f, 0.6f, 0.3f);

	// Details - light gray
	if (IFCClassName.Contains(TEXT("Railing"))) return FLinearColor(0.6f, 0.6f, 0.6f);
	if (IFCClassName.Contains(TEXT("Fastener"))) return FLinearColor(0.4f, 0.4f, 0.4f);

	// Default - gray
	return FLinearColor::Gray;
}

void UFragmentSemanticTileManager::CalculateCombinedBounds(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	Tile->CombinedBounds = FBox(ForceInit);

	// Get model wrapper for bounds access
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		return;
	}

	const Model* ParsedModel = Wrapper->GetParsedModel();
	if (!ParsedModel || !ParsedModel->meshes())
	{
		return;
	}

	const Meshes* MeshesRef = ParsedModel->meshes();

	// Accumulate bounds from all fragments in this tile
	for (int32 LocalID : Tile->FragmentIDs)
	{
		FFragmentItem* Item = Importer->GetFragmentItemByLocalId(LocalID, ModelGuid);
		if (Item && Item->Samples.Num() > 0)
		{
			FBox FragmentBounds(ForceInit);
			bool bHasValidBounds = false;

			// Calculate bounds from all samples (representations)
			for (const FFragmentSample& Sample : Item->Samples)
			{
				if (Sample.RepresentationIndex < 0 ||
				    Sample.RepresentationIndex >= static_cast<int32>(MeshesRef->representations()->size()))
				{
					continue;
				}

				const Representation* Rep = MeshesRef->representations()->Get(Sample.RepresentationIndex);
				if (!Rep)
				{
					continue;
				}

				// Get bounding box from representation (returns reference, not pointer)
				const BoundingBox& bbox = Rep->bbox();

				// Get min/max vectors (returns references, not pointers)
				const FloatVector& minVec = bbox.min();
				const FloatVector& maxVec = bbox.max();

				// Convert to Unreal coordinates (cm)
				// Note: Coordinate transform (x,y,z) → (x*100, z*100, y*100)
				FVector Min(
					minVec.x() * 100.0f,
					minVec.z() * 100.0f,
					minVec.y() * 100.0f
				);
				FVector Max(
					maxVec.x() * 100.0f,
					maxVec.z() * 100.0f,
					maxVec.y() * 100.0f
				);

				FBox RepBounds(Min, Max);

				// Transform by global transform
				FBox TransformedBounds = RepBounds.TransformBy(Item->GlobalTransform);

				if (bHasValidBounds)
				{
					FragmentBounds += TransformedBounds;
				}
				else
				{
					FragmentBounds = TransformedBounds;
					bHasValidBounds = true;
				}
			}

			// Add fragment bounds to tile combined bounds
			if (bHasValidBounds)
			{
				Tile->CombinedBounds += FragmentBounds;
			}
			else
			{
				// Fallback to position if no valid bounds
				FVector Position = Item->GlobalTransform.GetLocation();
				FBox PointBounds(Position, Position);
				Tile->CombinedBounds += PointBounds.ExpandBy(50.0f);
			}
		}
	}

	if (Config.bEnableDebugLogging)
	{
		FVector Size = Tile->CombinedBounds.GetSize();
		UE_LOG(LogSemanticTiles, Verbose, TEXT("  %s: Bounds size = (%.2f, %.2f, %.2f)"),
		       *Tile->IFCClassName, Size.X, Size.Y, Size.Z);
	}
}

// ===================================================================
// Phase 2: LOD Management Implementation
// ===================================================================

float UFragmentSemanticTileManager::CalculateScreenCoverage(USemanticTile* Tile,
                                                              const FVector& CameraLocation,
                                                              const FRotator& CameraRotation,
                                                              float FOV, float ViewportHeight)
{
	if (!Tile || !Tile->CombinedBounds.IsValid)
	{
		return 0.0f;
	}

	// Calculate distance from camera to tile center
	FVector TileCenter = Tile->CombinedBounds.GetCenter();
	float Distance = FVector::Dist(CameraLocation, TileCenter);

	if (Distance < 1.0f)
	{
		Distance = 1.0f; // Prevent division by zero
	}

	// Get tile bounding box size (use maximum dimension)
	FVector TileSize = Tile->CombinedBounds.GetSize();
	float TileDimension = TileSize.GetMax();

	// Calculate screen coverage using engine_fragment formula:
	// ScreenSize = (Dimension / Distance) * (ViewportHeight / (2 * tan(FOV/2)))
	float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	float TanHalfFOV = FMath::Tan(HalfFOVRadians);

	if (TanHalfFOV < SMALL_NUMBER)
	{
		return 0.0f;
	}

	// Screen size in pixels
	float ScreenSizePixels = (TileDimension / Distance) * (ViewportHeight / (2.0f * TanHalfFOV));

	// Convert to percentage of viewport height (0.0 to 1.0)
	float ScreenCoverage = ScreenSizePixels / ViewportHeight;

	return FMath::Clamp(ScreenCoverage, 0.0f, 1.0f);
}

ESemanticLOD UFragmentSemanticTileManager::DetermineLODLevel(float ScreenCoverage)
{
	if (!Config.bEnableLOD)
	{
		return ESemanticLOD::HighDetail; // Always high detail if LOD disabled
	}

	// Determine LOD based on screen coverage thresholds
	if (ScreenCoverage >= Config.LOD1ToLOD2Threshold)
	{
		return ESemanticLOD::HighDetail; // > 5% = LOD 2
	}
	else if (ScreenCoverage >= Config.LOD0ToLOD1Threshold)
	{
		return ESemanticLOD::SimpleBox; // 1% - 5% = LOD 1
	}
	else if (ScreenCoverage > 0.0f)
	{
		return ESemanticLOD::Wireframe; // < 1% = LOD 0
	}
	else
	{
		return ESemanticLOD::Unloaded; // Not visible
	}
}

void UFragmentSemanticTileManager::TransitionToLOD(USemanticTile* Tile, ESemanticLOD TargetLOD)
{
	if (!Tile || Tile->CurrentLOD == TargetLOD)
	{
		return; // Already at target LOD
	}

	// Hide current LOD
	HideLOD(Tile);

	// Show target LOD
	switch (TargetLOD)
	{
	case ESemanticLOD::Unloaded:
		// Already hidden
		break;

	case ESemanticLOD::Wireframe:
		ShowWireframe(Tile);
		break;

	case ESemanticLOD::SimpleBox:
		ShowSimpleBox(Tile);
		break;

	case ESemanticLOD::HighDetail:
		ShowHighDetail(Tile);
		break;
	}

	Tile->CurrentLOD = TargetLOD;

	if (Config.bEnableDebugLogging)
	{
		UE_LOG(LogSemanticTiles, Verbose, TEXT("  %s: LOD transition → %d (coverage: %.3f%%)"),
		       *Tile->IFCClassName, static_cast<int32>(TargetLOD), Tile->ScreenCoverage * 100.0f);
	}
}

void UFragmentSemanticTileManager::ShowWireframe(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSemanticTiles, Warning, TEXT("ShowWireframe: Null tile or importer"));
			bLoggedOnce = true;
		}
		return;
	}

	// Get world for drawing
	AActor* Owner = Importer->GetOwnerRef();
	if (!Owner)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSemanticTiles, Warning, TEXT("ShowWireframe: No owner actor"));
			bLoggedOnce = true;
		}
		return;
	}

	UWorld* World = Owner->GetWorld();
	if (!World)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSemanticTiles, Warning, TEXT("ShowWireframe: No world from owner actor"));
			bLoggedOnce = true;
		}
		return;
	}

	// Draw wireframe bounding box using DrawDebugBox
	// Use brighter, more visible colors and thicker lines
	FColor WireframeColor = Tile->RepresentativeColor.ToFColor(true);
	DrawDebugBox(World, Tile->CombinedBounds.GetCenter(),
	            Tile->CombinedBounds.GetExtent(), WireframeColor,
	            false, 0.0f, 0, 3.0f); // Non-persistent (redrawn every frame), depth priority 0, thickness 3.0
}

void UFragmentSemanticTileManager::ShowSimpleBox(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	AActor* Owner = Importer->GetOwnerRef();
	if (!Owner)
	{
		return;
	}

	// Create procedural mesh component if it doesn't exist
	if (!Tile->SimpleBoxMesh)
	{
		Tile->SimpleBoxMesh = NewObject<UProceduralMeshComponent>(Owner);
		Tile->SimpleBoxMesh->RegisterComponent();
		Tile->SimpleBoxMesh->AttachToComponent(Owner->GetRootComponent(),
		                                         FAttachmentTransformRules::KeepWorldTransform);

		// Create simple material instance that shows the representative color
		// ProceduralMeshComponent will use vertex colors by default with a basic material
		UMaterialInterface* BaseMaterial = Tile->SimpleBoxMesh->GetMaterial(0);
		if (!BaseMaterial)
		{
			// Use the engine's default material for procedural meshes
			UMaterialInstanceDynamic* DynMaterial = Tile->SimpleBoxMesh->CreateAndSetMaterialInstanceDynamic(0);
			if (DynMaterial)
			{
				DynMaterial->SetVectorParameterValue(FName("BaseColor"), Tile->RepresentativeColor);
			}
		}
	}

	// Generate box mesh data
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> VertexColors;

	FVector Center = Tile->CombinedBounds.GetCenter();
	FVector Extent = Tile->CombinedBounds.GetExtent();

	// Define 8 corners of the box
	FVector Corners[8] = {
		Center + FVector(-Extent.X, -Extent.Y, -Extent.Z),
		Center + FVector(Extent.X, -Extent.Y, -Extent.Z),
		Center + FVector(Extent.X, Extent.Y, -Extent.Z),
		Center + FVector(-Extent.X, Extent.Y, -Extent.Z),
		Center + FVector(-Extent.X, -Extent.Y, Extent.Z),
		Center + FVector(Extent.X, -Extent.Y, Extent.Z),
		Center + FVector(Extent.X, Extent.Y, Extent.Z),
		Center + FVector(-Extent.X, Extent.Y, Extent.Z)
	};

	// Build 6 faces (12 triangles) with PROPER NORMALS

	// Front face (-Y)
	int32 BaseIdx = Vertices.Num();
	Vertices.Append({Corners[0], Corners[1], Corners[5], Corners[4]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, -1, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Back face (+Y)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[3], Corners[2], Corners[6], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 1, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Left face (-X)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[3], Corners[0], Corners[4], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(-1, 0, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Right face (+X)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[1], Corners[2], Corners[6], Corners[5]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(1, 0, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Bottom face (-Z)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[0], Corners[3], Corners[2], Corners[1]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 0, -1)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Top face (+Z)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[4], Corners[5], Corners[6], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 0, 1)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Create mesh section
	Tile->SimpleBoxMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals,
	                                                     UVs, VertexColors, TArray<FProcMeshTangent>(), true);
	Tile->SimpleBoxMesh->SetVisibility(true);
}

void UFragmentSemanticTileManager::ShowHighDetail(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	// Phase 2: Placeholder - triggers existing tile manager spawning
	// In Phase 4, this will integrate with octree spatial subdivision
	// For now, mark tile as loaded (actual spawning happens via existing FragmentTileManager)
	Tile->bIsLoaded = true;

	UE_LOG(LogSemanticTiles, Verbose, TEXT("  %s: High detail loading triggered (%d fragments)"),
	       *Tile->IFCClassName, Tile->Count);
}

void UFragmentSemanticTileManager::HideLOD(USemanticTile* Tile)
{
	if (!Tile)
	{
		return;
	}

	// Hide simple box mesh if it exists
	if (Tile->SimpleBoxMesh)
	{
		Tile->SimpleBoxMesh->SetVisibility(false);
	}

	// Wireframe is drawn with DrawDebugBox (persistent = false), so it clears automatically
	// High detail hiding will be handled by FragmentTileManager in Phase 4
}

// ===================================================================
// Phase 4: Octree Spatial Subdivision Implementation
// ===================================================================

void UFragmentSemanticTileManager::BuildSpatialSubdivision(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Calculate world-space bounds for each fragment
	TMap<int32, FBox> FragmentBounds;
	CalculateFragmentBounds(Tile, FragmentBounds);

	// Create root sub-tile containing all fragments
	FSemanticSubTile RootSubTile;
	RootSubTile.Bounds = Tile->CombinedBounds;
	RootSubTile.FragmentIDs = Tile->FragmentIDs;
	RootSubTile.CurrentLOD = ESemanticLOD::Unloaded;
	RootSubTile.TargetLOD = ESemanticLOD::Unloaded;
	RootSubTile.SimpleBoxMesh = nullptr;
	RootSubTile.ScreenCoverage = 0.0f;
	RootSubTile.Depth = 0;

	// Initialize child indices to -1 (no children)
	for (int32 i = 0; i < 8; i++)
	{
		RootSubTile.ChildIndices[i] = -1;
	}

	Tile->SpatialSubTiles.Add(RootSubTile);
	Tile->RootSubTileIndex = 0;

	// Recursively subdivide root
	SubdivideSubTile(Tile, 0, 0, FragmentBounds);

	const double ElapsedTime = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	UE_LOG(LogSemanticTiles, Log, TEXT("  %s: Spatial subdivision complete - %d sub-tiles (%.2f ms)"),
	       *Tile->IFCClassName, Tile->SpatialSubTiles.Num(), ElapsedTime);
}

void UFragmentSemanticTileManager::SubdivideSubTile(USemanticTile* Tile, int32 SubTileIndex,
                                                      int32 CurrentDepth,
                                                      const TMap<int32, FBox>& FragmentBounds)
{
	if (!Tile || SubTileIndex < 0 || SubTileIndex >= Tile->SpatialSubTiles.Num())
	{
		return;
	}

	FSemanticSubTile& SubTile = Tile->SpatialSubTiles[SubTileIndex];

	// Check subdivision termination conditions
	bool bShouldSubdivide = true;

	// Condition 1: Max depth reached
	if (CurrentDepth >= Config.MaxSubdivisionDepth)
	{
		bShouldSubdivide = false;
	}

	// Condition 2: Too few fragments
	if (SubTile.FragmentIDs.Num() < Config.MinFragmentsPerSubTile)
	{
		bShouldSubdivide = false;
	}

	// Condition 3: Sub-tile too small
	FVector SubTileSize = SubTile.Bounds.GetSize();
	if (SubTileSize.GetMax() < Config.MinSubTileSize)
	{
		bShouldSubdivide = false;
	}

	if (!bShouldSubdivide)
	{
		return; // Leaf node
	}

	// Subdivide into 8 octants
	FVector Center = SubTile.Bounds.GetCenter();
	FVector Extent = SubTile.Bounds.GetExtent();
	FVector HalfExtent = Extent * 0.5f;

	// Octant offsets
	FVector Offsets[8] = {
		FVector(-HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z), // 0: ---
		FVector( HalfExtent.X, -HalfExtent.Y, -HalfExtent.Z), // 1: +--
		FVector(-HalfExtent.X,  HalfExtent.Y, -HalfExtent.Z), // 2: -+-
		FVector( HalfExtent.X,  HalfExtent.Y, -HalfExtent.Z), // 3: ++-
		FVector(-HalfExtent.X, -HalfExtent.Y,  HalfExtent.Z), // 4: --+
		FVector( HalfExtent.X, -HalfExtent.Y,  HalfExtent.Z), // 5: +--+
		FVector(-HalfExtent.X,  HalfExtent.Y,  HalfExtent.Z), // 6: -++
		FVector( HalfExtent.X,  HalfExtent.Y,  HalfExtent.Z)  // 7: +++
	};

	// Create child sub-tiles
	for (int32 Octant = 0; Octant < 8; Octant++)
	{
		// Calculate octant bounds
		FVector OctantCenter = Center + Offsets[Octant];
		FBox OctantBounds(OctantCenter - HalfExtent, OctantCenter + HalfExtent);

		// Find fragments that intersect this octant
		TArray<int32> OctantFragments;
		for (int32 FragID : SubTile.FragmentIDs)
		{
			const FBox* FragBounds = FragmentBounds.Find(FragID);
			if (FragBounds && OctantBounds.Intersect(*FragBounds))
			{
				OctantFragments.Add(FragID);
			}
		}

		// Only create child if it contains fragments
		if (OctantFragments.Num() > 0)
		{
			// Calculate TIGHT bounding box for fragments in this octant
			FBox TightBounds(ForceInit);
			for (int32 FragID : OctantFragments)
			{
				const FBox* FragBounds = FragmentBounds.Find(FragID);
				if (FragBounds)
				{
					TightBounds += *FragBounds;
				}
			}

			// Use tight bounds instead of uniform octant bounds
			FSemanticSubTile ChildSubTile;
			ChildSubTile.Bounds = TightBounds; // FIXED: Use actual fragment bounds
			ChildSubTile.FragmentIDs = OctantFragments;
			ChildSubTile.CurrentLOD = ESemanticLOD::Unloaded;
			ChildSubTile.TargetLOD = ESemanticLOD::Unloaded;
			ChildSubTile.SimpleBoxMesh = nullptr;
			ChildSubTile.ScreenCoverage = 0.0f;
			ChildSubTile.Depth = CurrentDepth + 1;

			// Initialize child indices
			for (int32 i = 0; i < 8; i++)
			{
				ChildSubTile.ChildIndices[i] = -1;
			}

			// Add child to array
			int32 ChildIndex = Tile->SpatialSubTiles.Add(ChildSubTile);

			// Link parent to child
			SubTile.ChildIndices[Octant] = ChildIndex;

			// Recursively subdivide child
			SubdivideSubTile(Tile, ChildIndex, CurrentDepth + 1, FragmentBounds);
		}
	}
}

void UFragmentSemanticTileManager::CalculateFragmentBounds(USemanticTile* Tile,
                                                             TMap<int32, FBox>& OutFragmentBounds)
{
	if (!Tile || !Importer)
	{
		return;
	}

	// Get model wrapper for bounds access
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		return;
	}

	const Model* ParsedModel = Wrapper->GetParsedModel();
	if (!ParsedModel || !ParsedModel->meshes())
	{
		return;
	}

	const Meshes* MeshesRef = ParsedModel->meshes();

	// Calculate world-space bounds for each fragment
	for (int32 LocalID : Tile->FragmentIDs)
	{
		FFragmentItem* Item = Importer->GetFragmentItemByLocalId(LocalID, ModelGuid);
		if (!Item || Item->Samples.Num() == 0)
		{
			continue;
		}

		FBox FragmentBounds(ForceInit);
		bool bHasValidBounds = false;

		// Calculate bounds from all samples (representations)
		for (const FFragmentSample& Sample : Item->Samples)
		{
			if (Sample.RepresentationIndex < 0 ||
			    Sample.RepresentationIndex >= static_cast<int32>(MeshesRef->representations()->size()))
			{
				continue;
			}

			const Representation* Rep = MeshesRef->representations()->Get(Sample.RepresentationIndex);
			if (!Rep)
			{
				continue;
			}

			// Get bounding box from representation
			const BoundingBox& bbox = Rep->bbox();
			const FloatVector& minVec = bbox.min();
			const FloatVector& maxVec = bbox.max();

			// Convert to Unreal coordinates (cm)
			FVector Min(minVec.x() * 100.0f, minVec.z() * 100.0f, minVec.y() * 100.0f);
			FVector Max(maxVec.x() * 100.0f, maxVec.z() * 100.0f, maxVec.y() * 100.0f);

			FBox RepBounds(Min, Max);

			// Transform by global transform
			FBox TransformedBounds = RepBounds.TransformBy(Item->GlobalTransform);

			if (bHasValidBounds)
			{
				FragmentBounds += TransformedBounds;
			}
			else
			{
				FragmentBounds = TransformedBounds;
				bHasValidBounds = true;
			}
		}

		// Store fragment bounds (or fallback to position)
		if (bHasValidBounds)
		{
			OutFragmentBounds.Add(LocalID, FragmentBounds);
		}
		else
		{
			// Fallback to point bounds
			FVector Position = Item->GlobalTransform.GetLocation();
			OutFragmentBounds.Add(LocalID, FBox(Position, Position).ExpandBy(50.0f));
		}
	}
}

void UFragmentSemanticTileManager::UpdateSubTileLOD(USemanticTile* Tile, FSemanticSubTile& SubTile,
                                                      const FVector& CameraLocation)
{
	// Determine LOD from distance (orientation-independent)
	ESemanticLOD TargetLOD = DetermineLODFromDistance(SubTile, CameraLocation);
	SubTile.TargetLOD = TargetLOD;

	// Transition if LOD changed
	if (SubTile.CurrentLOD != TargetLOD)
	{
		TransitionSubTileToLOD(Tile, SubTile, TargetLOD);
	}

	// Redraw wireframe every frame (DrawDebugBox only lasts one frame)
	if (SubTile.CurrentLOD == ESemanticLOD::Wireframe)
	{
		ShowSubTileWireframe(Tile, SubTile);
	}
}

ESemanticLOD UFragmentSemanticTileManager::DetermineLODFromDistance(const FSemanticSubTile& SubTile,
                                                                     const FVector& CameraLocation) const
{
	if (!SubTile.Bounds.IsValid)
	{
		return ESemanticLOD::Unloaded;
	}

	// Calculate bounding sphere radius (diagonal of box)
	FVector Extent = SubTile.Bounds.GetExtent();
	float BoundingSphereRadius = Extent.Size(); // sqrt(X² + Y² + Z²)

	// Calculate distance from camera to box center
	FVector BoxCenter = SubTile.Bounds.GetCenter();
	float Distance = FVector::Dist(CameraLocation, BoxCenter);

	// Distance-based LOD thresholds (orientation-independent)
	float LOD2Distance = BoundingSphereRadius * Config.LOD2DistanceMultiplier;
	float LOD1Distance = BoundingSphereRadius * Config.LOD1DistanceMultiplier;

	// Debug logging for LOD transitions
	UE_LOG(LogFragments, Log, TEXT("LOD Check - Radius: %.1f, Distance: %.1f, LOD2Thresh: %.1f, LOD1Thresh: %.1f, Extent: %s, Center: %s"),
	       BoundingSphereRadius, Distance, LOD2Distance, LOD1Distance, *Extent.ToString(), *BoxCenter.ToString());

	if (Distance < LOD2Distance)
	{
		return ESemanticLOD::HighDetail;  // Close - show full meshes
	}
	else if (Distance < LOD1Distance)
	{
		return ESemanticLOD::SimpleBox;   // Medium - show colored boxes
	}
	else
	{
		return ESemanticLOD::Wireframe;   // Far - show wireframe
	}
}

void UFragmentSemanticTileManager::TransitionSubTileToLOD(USemanticTile* Tile,
                                                            FSemanticSubTile& SubTile,
                                                            ESemanticLOD TargetLOD)
{
	if (SubTile.CurrentLOD == TargetLOD)
	{
		return; // Already at target LOD
	}

	// Debug logging for LOD transitions
	const TCHAR* LODNames[] = { TEXT("Unloaded"), TEXT("Wireframe"), TEXT("SimpleBox"), TEXT("HighDetail") };
	UE_LOG(LogFragments, Log, TEXT("LOD Transition - %s: %s → %s (Frags: %d, Bounds: %s)"),
	       *Tile->IFCClassName,
	       LODNames[static_cast<int32>(SubTile.CurrentLOD)],
	       LODNames[static_cast<int32>(TargetLOD)],
	       SubTile.FragmentIDs.Num(),
	       *SubTile.Bounds.ToString());

	// Hide current LOD
	HideSubTileLOD(SubTile);

	// Show target LOD
	switch (TargetLOD)
	{
	case ESemanticLOD::Unloaded:
		// Already hidden
		break;

	case ESemanticLOD::Wireframe:
		ShowSubTileWireframe(Tile, SubTile);
		break;

	case ESemanticLOD::SimpleBox:
		ShowSubTileSimpleBox(Tile, SubTile);
		break;

	case ESemanticLOD::HighDetail:
		// High detail loading will be integrated with existing FragmentTileManager
		// For now, just mark as loaded
		break;
	}

	SubTile.CurrentLOD = TargetLOD;
}

void UFragmentSemanticTileManager::ShowSubTileWireframe(USemanticTile* Tile,
                                                          const FSemanticSubTile& SubTile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	// Get world for drawing
	AActor* Owner = Importer->GetOwnerRef();
	if (!Owner)
	{
		return;
	}

	UWorld* World = Owner->GetWorld();
	if (!World)
	{
		return;
	}

	// Draw wireframe bounding box using DrawDebugBox
	// Use brighter, more visible colors and thicker lines
	FColor WireframeColor = Tile->RepresentativeColor.ToFColor(true);
	DrawDebugBox(World, SubTile.Bounds.GetCenter(),
	            SubTile.Bounds.GetExtent(), WireframeColor,
	            false, -1.0f, 0, 3.0f); // Persistent (lifetime -1 = infinite), depth priority 0, thickness 3.0
}

void UFragmentSemanticTileManager::ShowSubTileSimpleBox(USemanticTile* Tile,
                                                          FSemanticSubTile& SubTile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	AActor* Owner = Importer->GetOwnerRef();
	if (!Owner)
	{
		return;
	}

	// Create procedural mesh component if it doesn't exist
	if (!SubTile.SimpleBoxMesh)
	{
		SubTile.SimpleBoxMesh = NewObject<UProceduralMeshComponent>(Owner);
		SubTile.SimpleBoxMesh->RegisterComponent();
		SubTile.SimpleBoxMesh->AttachToComponent(Owner->GetRootComponent(),
		                                           FAttachmentTransformRules::KeepWorldTransform);

		// ProceduralMeshComponent uses vertex colors by default
		// We'll use vertex colors to show the representative color
	}

	// Generate box mesh data
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> VertexColors;

	FVector Center = SubTile.Bounds.GetCenter();
	FVector Extent = SubTile.Bounds.GetExtent();

	// Define 8 corners of the box
	FVector Corners[8] = {
		Center + FVector(-Extent.X, -Extent.Y, -Extent.Z),
		Center + FVector(Extent.X, -Extent.Y, -Extent.Z),
		Center + FVector(Extent.X, Extent.Y, -Extent.Z),
		Center + FVector(-Extent.X, Extent.Y, -Extent.Z),
		Center + FVector(-Extent.X, -Extent.Y, Extent.Z),
		Center + FVector(Extent.X, -Extent.Y, Extent.Z),
		Center + FVector(Extent.X, Extent.Y, Extent.Z),
		Center + FVector(-Extent.X, Extent.Y, Extent.Z)
	};

	// Build 6 faces (12 triangles) with PROPER NORMALS

	// Front face (-Y)
	int32 BaseIdx = Vertices.Num();
	Vertices.Append({Corners[0], Corners[1], Corners[5], Corners[4]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, -1, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Back face (+Y)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[3], Corners[2], Corners[6], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 1, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Left face (-X)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[3], Corners[0], Corners[4], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(-1, 0, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Right face (+X)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[1], Corners[2], Corners[6], Corners[5]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(1, 0, 0)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Bottom face (-Z)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[0], Corners[3], Corners[2], Corners[1]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 0, -1)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Top face (+Z)
	BaseIdx = Vertices.Num();
	Vertices.Append({Corners[4], Corners[5], Corners[6], Corners[7]});
	Triangles.Append({BaseIdx + 0, BaseIdx + 2, BaseIdx + 1, BaseIdx + 0, BaseIdx + 3, BaseIdx + 2});
	for (int32 i = 0; i < 4; i++) { Normals.Add(FVector(0, 0, 1)); UVs.Add(FVector2D(0, 0)); VertexColors.Add(Tile->RepresentativeColor); }

	// Create mesh section
	SubTile.SimpleBoxMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals,
	                                                       UVs, VertexColors, TArray<FProcMeshTangent>(), true);
	SubTile.SimpleBoxMesh->SetVisibility(true);
}

void UFragmentSemanticTileManager::HideSubTileLOD(FSemanticSubTile& SubTile)
{
	// Hide simple box mesh if it exists
	if (SubTile.SimpleBoxMesh)
	{
		SubTile.SimpleBoxMesh->SetVisibility(false);
	}

	// Wireframe is drawn with DrawDebugBox (persistent = false), so it clears automatically
	// High detail hiding will be handled by FragmentTileManager integration
}
