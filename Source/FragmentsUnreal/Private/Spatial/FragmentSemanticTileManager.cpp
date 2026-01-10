#include "Spatial/FragmentSemanticTileManager.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "DrawDebugHelpers.h"
#include "ProceduralMeshComponent.h"

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
		UE_LOG(LogSemanticTiles, Log, TEXT("=== Semantic Tile LOD Status (Camera: %s) ==="),
		       *CameraLocation.ToString());
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
	FColor WireframeColor = Tile->RepresentativeColor.ToFColor(true);
	DrawDebugBox(World, Tile->CombinedBounds.GetCenter(),
	            Tile->CombinedBounds.GetExtent(), WireframeColor,
	            false, -1.0f, 0, 2.0f); // Persistent, depth priority 0, thickness 2
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

	// Build 6 faces (12 triangles)
	// Front face (-Y)
	Vertices.Append({Corners[0], Corners[1], Corners[5], Corners[4]});
	Triangles.Append({0, 2, 1, 0, 3, 2});

	// Back face (+Y)
	Vertices.Append({Corners[3], Corners[2], Corners[6], Corners[7]});
	Triangles.Append({4, 6, 5, 4, 7, 6});

	// Left face (-X)
	Vertices.Append({Corners[3], Corners[0], Corners[4], Corners[7]});
	Triangles.Append({8, 10, 9, 8, 11, 10});

	// Right face (+X)
	Vertices.Append({Corners[1], Corners[2], Corners[6], Corners[5]});
	Triangles.Append({12, 14, 13, 12, 15, 14});

	// Bottom face (-Z)
	Vertices.Append({Corners[0], Corners[3], Corners[2], Corners[1]});
	Triangles.Append({16, 18, 17, 16, 19, 18});

	// Top face (+Z)
	Vertices.Append({Corners[4], Corners[5], Corners[6], Corners[7]});
	Triangles.Append({20, 22, 21, 20, 23, 22});

	// Generate normals and vertex colors
	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		Normals.Add(FVector::UpVector); // Placeholder normal
		UVs.Add(FVector2D(0, 0)); // Placeholder UV
		VertexColors.Add(Tile->RepresentativeColor);
	}

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
