#include "Spatial/FragmentSemanticTileManager.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "DrawDebugHelpers.h"

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
	// Simplified: No wireframe/simple box LOD - just determine if tiles should be loaded

	if (!Importer)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();

	// Debug: Log first tick
	static bool bFirstTick = true;
	if (bFirstTick)
	{
		UE_LOG(LogSemanticTiles, Log, TEXT("First Tick called - %d semantic tiles"),
		       AllSemanticTiles.Num());
		bFirstTick = false;
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

					// Update loading state for this sub-tile
					UpdateSubTileLoading(Tile, SubTile, CameraLocation);

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
				// Whole-tile loading (no spatial subdivision)
				Tile->LastUpdateTime = CurrentTime;

				// Determine if tile should be loaded based on distance
				bool bShouldLoad = ShouldLoadTile(Tile, CameraLocation);
				ESemanticLOD TargetLOD = bShouldLoad ? ESemanticLOD::Loaded : ESemanticLOD::Unloaded;
				Tile->TargetLOD = TargetLOD;

				// Transition to target LOD if different from current
				if (Tile->CurrentLOD != TargetLOD)
				{
					TransitionToLOD(Tile, TargetLOD);
				}
			}
		}
	}

	// Draw debug bounds if enabled
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
					// Draw white debug bounds
					DrawDebugBox(World, Tile->CombinedBounds.GetCenter(),
					            Tile->CombinedBounds.GetExtent(), FColor::White,
					            false, 0.0f, 0, 1.0f);
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
// Simplified LOD Management (Loaded/Unloaded only - no wireframe/simple box)
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

	// Calculate screen coverage using engine_fragment formula
	float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	float TanHalfFOV = FMath::Tan(HalfFOVRadians);

	if (TanHalfFOV < SMALL_NUMBER)
	{
		return 0.0f;
	}

	// Screen size in pixels
	float ScreenSizePixels = (TileDimension / Distance) * (ViewportHeight / (2.0f * TanHalfFOV));

	// Convert to percentage of viewport height (0.0 to 1.0)
	return FMath::Clamp(ScreenSizePixels / ViewportHeight, 0.0f, 1.0f);
}

bool UFragmentSemanticTileManager::ShouldLoadTile(USemanticTile* Tile, const FVector& CameraLocation)
{
	if (!Tile || !Tile->CombinedBounds.IsValid)
	{
		return false;
	}

	// Calculate bounding sphere radius
	FVector Extent = Tile->CombinedBounds.GetExtent();
	float BoundingSphereRadius = Extent.Size();

	// Calculate distance from camera to tile center
	FVector TileCenter = Tile->CombinedBounds.GetCenter();
	float Distance = FVector::Dist(CameraLocation, TileCenter);

	// Load if within distance threshold
	float LoadDistance = BoundingSphereRadius * Config.LoadDistanceMultiplier;
	return Distance < LoadDistance;
}

void UFragmentSemanticTileManager::TransitionToLOD(USemanticTile* Tile, ESemanticLOD TargetLOD)
{
	if (!Tile || Tile->CurrentLOD == TargetLOD)
	{
		return;
	}

	// Simple state transition - no visualization components to manage
	if (TargetLOD == ESemanticLOD::Loaded)
	{
		Tile->bIsLoaded = true;
		UE_LOG(LogSemanticTiles, Verbose, TEXT("  %s: Loading triggered (%d fragments)"),
		       *Tile->IFCClassName, Tile->Count);
	}
	else
	{
		Tile->bIsLoaded = false;
	}

	Tile->CurrentLOD = TargetLOD;
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

void UFragmentSemanticTileManager::UpdateSubTileLoading(USemanticTile* Tile, FSemanticSubTile& SubTile,
                                                          const FVector& CameraLocation)
{
	// Determine if sub-tile should be loaded based on distance
	bool bShouldLoad = ShouldLoadSubTile(SubTile, CameraLocation);
	ESemanticLOD TargetLOD = bShouldLoad ? ESemanticLOD::Loaded : ESemanticLOD::Unloaded;
	SubTile.TargetLOD = TargetLOD;

	// Transition if state changed
	if (SubTile.CurrentLOD != TargetLOD)
	{
		TransitionSubTileToLOD(Tile, SubTile, TargetLOD);
	}
}

bool UFragmentSemanticTileManager::ShouldLoadSubTile(const FSemanticSubTile& SubTile,
                                                       const FVector& CameraLocation) const
{
	if (!SubTile.Bounds.IsValid)
	{
		return false;
	}

	// Calculate bounding sphere radius (diagonal of box)
	FVector Extent = SubTile.Bounds.GetExtent();
	float BoundingSphereRadius = Extent.Size();

	// Calculate distance from camera to box center
	FVector BoxCenter = SubTile.Bounds.GetCenter();
	float Distance = FVector::Dist(CameraLocation, BoxCenter);

	// Load if within distance threshold
	float LoadDistance = BoundingSphereRadius * Config.LoadDistanceMultiplier;
	return Distance < LoadDistance;
}

void UFragmentSemanticTileManager::TransitionSubTileToLOD(USemanticTile* Tile,
                                                            FSemanticSubTile& SubTile,
                                                            ESemanticLOD TargetLOD)
{
	if (SubTile.CurrentLOD == TargetLOD)
	{
		return;
	}

	// Simple state logging
	const TCHAR* LODNames[] = { TEXT("Unloaded"), TEXT("Loaded") };
	UE_LOG(LogSemanticTiles, Verbose, TEXT("Sub-tile transition - %s: %s → %s (Frags: %d)"),
	       *Tile->IFCClassName,
	       LODNames[static_cast<int32>(SubTile.CurrentLOD)],
	       LODNames[static_cast<int32>(TargetLOD)],
	       SubTile.FragmentIDs.Num());

	SubTile.CurrentLOD = TargetLOD;
}
