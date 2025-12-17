#include "Streaming/FragmentOctree.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"

UFragmentOctree::UFragmentOctree()
{
}

bool UFragmentOctree::BuildFromModel(UFragmentModelWrapper* ModelWrapper, const FFragmentOctreeConfig& InConfig)
{
	if (!ModelWrapper)
	{
		UE_LOG(LogTemp, Error, TEXT("FragmentOctree: Cannot build from null model wrapper"));
		return false;
	}

	SourceModel = ModelWrapper;
	Config = InConfig;

	// Get all fragment local IDs from the model
	TArray<int32> AllFragmentIDs;
	const TMap<int32, TSharedPtr<FFragmentItem>>& FragmentMap = ModelWrapper->GetFragmentItems();

	for (const auto& Pair : FragmentMap)
	{
		if (Pair.Value.IsValid())
		{
			AllFragmentIDs.Add(Pair.Value->LocalId);
		}
	}

	if (AllFragmentIDs.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FragmentOctree: No fragments found in model"));
		return false;
	}

	// Calculate bounding box for entire model
	FBox ModelBounds = CalculateBoundingBox(AllFragmentIDs);

	// Log model bounds for debugging
	FVector BoundsSize = ModelBounds.GetSize();
	UE_LOG(LogTemp, Log, TEXT("FragmentOctree: Model bounds size: (%.2f, %.2f, %.2f) cm, Min: %.2f cm"),
		BoundsSize.X, BoundsSize.Y, BoundsSize.Z, BoundsSize.GetMin());

	// Auto-adjust MinTileSize if it's too large for this model
	// MinTileSize should be at most 1/8th of the smallest model dimension to allow subdivision
	float AdaptiveMinTileSize = Config.MinTileSize;
	float ModelMinDimension = BoundsSize.GetMin();
	float MaxAllowedMinTileSize = ModelMinDimension / 8.0f;

	if (Config.MinTileSize > MaxAllowedMinTileSize)
	{
		AdaptiveMinTileSize = FMath::Max(1.0f, MaxAllowedMinTileSize); // At least 1 cm
		UE_LOG(LogTemp, Warning, TEXT("FragmentOctree: MinTileSize (%.2f) too large for model size. Auto-adjusting to %.2f cm"),
			Config.MinTileSize, AdaptiveMinTileSize);
		Config.MinTileSize = AdaptiveMinTileSize; // Update config
	}

	UE_LOG(LogTemp, Log, TEXT("FragmentOctree: Config - MaxDepth: %d, MaxFragmentsPerTile: %d, MinTileSize: %.2f cm"),
		Config.MaxDepth, Config.MaxFragmentsPerTile, Config.MinTileSize);

	// Create root tile
	RootTile = MakeShared<FFragmentTile>(
		TEXT("L0_X0_Y0_Z0"),
		0, // Level
		FIntVector(0, 0, 0), // Indices
		ModelBounds
	);

	// Calculate geometric error for root
	RootTile->GeometricError = CalculateGeometricError(RootTile, AllFragmentIDs);

	// Start recursive subdivision
	SubdivideTile(RootTile, AllFragmentIDs);

	UE_LOG(LogTemp, Log, TEXT("FragmentOctree: Built octree with %d total fragments"), AllFragmentIDs.Num());

	return true;
}

void UFragmentOctree::SubdivideTile(TSharedPtr<FFragmentTile> Tile, const TArray<int32>& FragmentLocalIDs)
{
	// Check if we should stop subdividing
	const bool bMaxDepthReached = Tile->Level >= Config.MaxDepth;
	const bool bFewFragments = FragmentLocalIDs.Num() <= Config.MaxFragmentsPerTile;
	const FVector TileSize = Tile->BoundingBox.GetSize();
	const bool bMinSizeReached = TileSize.GetMin() <= Config.MinTileSize;

	if (bMaxDepthReached || bFewFragments || bMinSizeReached)
	{
		// This is a leaf tile - store fragments here
		Tile->FragmentLocalIDs = FragmentLocalIDs;

		// Log why we stopped subdividing
		if (Tile->Level == 0 && FragmentLocalIDs.Num() > 100)
		{
			UE_LOG(LogTemp, Warning, TEXT("FragmentOctree: Root tile NOT subdividing! Reason: MaxDepth=%d, FewFragments=%d, MinSizeReached=%d (TileSize=%.2f, MinTileSize=%.2f)"),
				bMaxDepthReached ? 1 : 0, bFewFragments ? 1 : 0, bMinSizeReached ? 1 : 0,
				TileSize.GetMin(), Config.MinTileSize);
		}

		UE_LOG(LogTemp, Verbose, TEXT("FragmentOctree: Leaf tile %s with %d fragments at level %d"),
			*Tile->TileID, FragmentLocalIDs.Num(), Tile->Level);
		return;
	}

	// Create 8 child tiles (octree subdivision)
	const FVector Center = Tile->BoundingBox.GetCenter();
	const FVector HalfSize = Tile->BoundingBox.GetSize() * 0.5f;
	const int32 ChildLevel = Tile->Level + 1;

	Tile->Children.Reserve(8);

	for (int32 Z = 0; Z < 2; ++Z)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			for (int32 X = 0; X < 2; ++X)
			{
				// Calculate child bounding box
				const FVector ChildMin = Center + FVector(
					(X == 0) ? -HalfSize.X : 0.0f,
					(Y == 0) ? -HalfSize.Y : 0.0f,
					(Z == 0) ? -HalfSize.Z : 0.0f
				);
				const FVector ChildMax = ChildMin + HalfSize;
				const FBox ChildBounds(ChildMin, ChildMax);

				// Calculate child indices
				const FIntVector ChildIndices(
					Tile->Indices.X * 2 + X,
					Tile->Indices.Y * 2 + Y,
					Tile->Indices.Z * 2 + Z
				);

				// Create child tile ID
				const FString ChildTileID = FString::Printf(TEXT("L%d_X%d_Y%d_Z%d"),
					ChildLevel, ChildIndices.X, ChildIndices.Y, ChildIndices.Z);

				// Create child tile
				TSharedPtr<FFragmentTile> ChildTile = MakeShared<FFragmentTile>(
					ChildTileID,
					ChildLevel,
					ChildIndices,
					ChildBounds
				);
				ChildTile->Parent = Tile.Get();

				Tile->Children.Add(ChildTile);
			}
		}
	}

	// Assign fragments to children based on spatial position
	AssignFragmentsToChildren(Tile, FragmentLocalIDs);

	// Recursively subdivide children
	for (TSharedPtr<FFragmentTile>& Child : Tile->Children)
	{
		if (Child->FragmentLocalIDs.Num() > 0)
		{
			// Calculate geometric error for child
			Child->GeometricError = CalculateGeometricError(Child, Child->FragmentLocalIDs);

			// Recursively subdivide
			SubdivideTile(Child, Child->FragmentLocalIDs);
		}
	}
}

void UFragmentOctree::AssignFragmentsToChildren(const TSharedPtr<FFragmentTile>& ParentTile, const TArray<int32>& FragmentLocalIDs)
{
	// Assign each fragment to the appropriate child tile based on centroid
	for (int32 LocalID : FragmentLocalIDs)
	{
		// Get fragment transform
		FTransform FragmentTransform;
		if (SourceModel->GetFragmentTransform(LocalID, FragmentTransform))
		{
			const FVector FragmentLocation = FragmentTransform.GetLocation();

			// Find which child contains this fragment
			for (TSharedPtr<FFragmentTile>& Child : ParentTile->Children)
			{
				if (Child->BoundingBox.IsInside(FragmentLocation))
				{
					Child->FragmentLocalIDs.Add(LocalID);
					break;
				}
			}
		}
	}
}

FBox UFragmentOctree::CalculateBoundingBox(const TArray<int32>& FragmentLocalIDs) const
{
	FBox Bounds(ForceInit);

	int32 ValidTransforms = 0;
	int32 ZeroTransforms = 0;

	for (int32 LocalID : FragmentLocalIDs)
	{
		FTransform FragmentTransform;
		if (SourceModel->GetFragmentTransform(LocalID, FragmentTransform))
		{
			ValidTransforms++;
			FVector Location = FragmentTransform.GetLocation();

			// Check if transform is at origin
			if (Location.IsNearlyZero(0.01f))
			{
				ZeroTransforms++;
			}

			// For now, just use fragment location as point
			// TODO: Incorporate actual fragment geometry bounds
			Bounds += Location;
		}
	}

	// Debug logging for first call
	static bool bFirstCall = true;
	if (bFirstCall)
	{
		bFirstCall = false;
		UE_LOG(LogTemp, Warning, TEXT("FragmentOctree: CalculateBoundingBox - Total: %d, ValidTransforms: %d, ZeroTransforms: %d"),
			FragmentLocalIDs.Num(), ValidTransforms, ZeroTransforms);

		// Sample first few transforms
		for (int32 i = 0; i < FMath::Min(5, FragmentLocalIDs.Num()); i++)
		{
			FTransform SampleTransform;
			if (SourceModel->GetFragmentTransform(FragmentLocalIDs[i], SampleTransform))
			{
				FVector Loc = SampleTransform.GetLocation();
				UE_LOG(LogTemp, Log, TEXT("  Sample LocalID %d: Location = (%.2f, %.2f, %.2f)"),
					FragmentLocalIDs[i], Loc.X, Loc.Y, Loc.Z);
			}
		}
	}

	// Add padding to bounds (10% on each side)
	const FVector Padding = Bounds.GetSize() * 0.1f;
	Bounds = Bounds.ExpandBy(Padding);

	return Bounds;
}

float UFragmentOctree::CalculateGeometricError(const TSharedPtr<FFragmentTile>& Tile, const TArray<int32>& FragmentLocalIDs) const
{
	// Geometric error is the maximum distance from this tile's representation to the ideal (full detail)
	// For now, use tile diagonal as a conservative estimate
	// Better: analyze actual geometry complexity
	const FVector TileSize = Tile->BoundingBox.GetSize();
	const float Diagonal = TileSize.Size();

	// Scale by level (coarser levels have higher error)
	const float LevelMultiplier = FMath::Pow(2.0f, static_cast<float>(Tile->Level));

	return Diagonal / LevelMultiplier;
}

TArray<TSharedPtr<FFragmentTile>> UFragmentOctree::QueryVisibleTiles(
	const FConvexVolume& Frustum,
	const FVector& CameraLocation,
	float VerticalFOV,
	float ViewportHeight,
	float MaxScreenSpaceError) const
{
	TArray<TSharedPtr<FFragmentTile>> VisibleTiles;

	if (!RootTile.IsValid())
	{
		return VisibleTiles;
	}

	// Recursively traverse octree and collect visible tiles
	QueryVisibleTilesRecursive(
		RootTile,
		Frustum,
		CameraLocation,
		VerticalFOV,
		ViewportHeight,
		MaxScreenSpaceError,
		VisibleTiles
	);

	return VisibleTiles;
}

void UFragmentOctree::QueryVisibleTilesRecursive(
	const TSharedPtr<FFragmentTile>& Tile,
	const FConvexVolume& Frustum,
	const FVector& CameraLocation,
	float VerticalFOV,
	float ViewportHeight,
	float MaxScreenSpaceError,
	TArray<TSharedPtr<FFragmentTile>>& OutVisibleTiles) const
{
	if (!Tile.IsValid())
	{
		return;
	}

	// Check frustum culling
	if (!Tile->IntersectsFrustum(Frustum))
	{
		return; // Tile not visible
	}

	// Calculate screen space error
	const float SSE = Tile->CalculateScreenSpaceError(CameraLocation, VerticalFOV, ViewportHeight);

	// Check if this tile is sufficient detail
	const bool bSufficientDetail = SSE <= MaxScreenSpaceError;
	const bool bIsLeaf = Tile->Children.Num() == 0;

	if (bSufficientDetail || bIsLeaf)
	{
		// Render this tile
		OutVisibleTiles.Add(Tile);
		return;
	}

	// Refine to children
	bool bAnyChildVisible = false;
	for (const TSharedPtr<FFragmentTile>& Child : Tile->Children)
	{
		if (Child.IsValid() && Child->FragmentLocalIDs.Num() > 0)
		{
			QueryVisibleTilesRecursive(
				Child,
				Frustum,
				CameraLocation,
				VerticalFOV,
				ViewportHeight,
				MaxScreenSpaceError,
				OutVisibleTiles
			);
			bAnyChildVisible = true;
		}
	}

	// If no children were visible/valid, render this tile as fallback
	if (!bAnyChildVisible)
	{
		OutVisibleTiles.Add(Tile);
	}
}

TArray<FFragmentTile> UFragmentOctree::GetAllTiles() const
{
	TArray<FFragmentTile> AllTiles;

	if (RootTile.IsValid())
	{
		CollectAllTilesRecursive(RootTile, AllTiles);
	}

	return AllTiles;
}

void UFragmentOctree::CollectAllTilesRecursive(const TSharedPtr<FFragmentTile>& Tile, TArray<FFragmentTile>& OutTiles) const
{
	if (!Tile.IsValid())
	{
		return;
	}

	OutTiles.Add(*Tile);

	for (const TSharedPtr<FFragmentTile>& Child : Tile->Children)
	{
		CollectAllTilesRecursive(Child, OutTiles);
	}
}

TArray<int32> UFragmentOctree::GetTileCountPerLevel() const
{
	TArray<int32> CountsPerLevel;
	CountsPerLevel.SetNum(Config.MaxDepth + 1);

	TArray<FFragmentTile> AllTiles = GetAllTiles();
	for (const FFragmentTile& Tile : AllTiles)
	{
		if (Tile.Level < CountsPerLevel.Num())
		{
			CountsPerLevel[Tile.Level]++;
		}
	}

	return CountsPerLevel;
}
