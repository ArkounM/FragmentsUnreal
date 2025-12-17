#pragma once

#include "CoreMinimal.h"
#include "Math/Box.h"
#include "Fragment/Fragment.h"
#include "FragmentTile.generated.h"

/**
 * Represents the current state of a tile in the streaming system
 */
UENUM(BlueprintType)
enum class EFragmentTileState : uint8
{
	Unloaded,       // Not loaded, no resources allocated
	Loading,        // Currently loading on worker thread
	Loaded,         // Loaded but not visible (hidden)
	Visible,        // Currently visible and rendered
	Unloading       // Being unloaded, resources being freed
};

/**
 * A spatial tile containing a subset of fragments within a bounding box.
 * This is the fundamental unit of streaming in the optimized system.
 */
USTRUCT(BlueprintType)
struct FRAGMENTSUNREAL_API FFragmentTile
{
	GENERATED_BODY()

	// Unique identifier for this tile (e.g., "L0_X2_Y3_Z1")
	UPROPERTY(BlueprintReadOnly)
	FString TileID;

	// Octree level (0 = root, higher = more detailed)
	UPROPERTY(BlueprintReadOnly)
	int32 Level = 0;

	// Spatial indices in the octree
	UPROPERTY(BlueprintReadOnly)
	FIntVector Indices = FIntVector::ZeroValue;

	// Axis-aligned bounding box of this tile in world space
	UPROPERTY(BlueprintReadOnly)
	FBox BoundingBox = FBox(ForceInit);

	// Current state of the tile
	UPROPERTY(BlueprintReadOnly)
	EFragmentTileState State = EFragmentTileState::Unloaded;

	// Local IDs of fragments contained in this tile
	UPROPERTY()
	TArray<int32> FragmentLocalIDs;

	// Spawned fragment actors (when State == Visible)
	UPROPERTY()
	TArray<TObjectPtr<AFragment>> SpawnedFragments;

	// Geometric error (maximum distance from ideal representation, in cm)
	UPROPERTY(BlueprintReadOnly)
	float GeometricError = 0.0f;

	// Memory used by this tile (bytes)
	UPROPERTY(BlueprintReadOnly)
	int64 MemoryUsageBytes = 0;

	// Last access time (for LRU cache)
	UPROPERTY()
	double LastAccessTime = 0.0;

	// Parent tile (nullptr for root)
	FFragmentTile* Parent = nullptr;

	// Child tiles (8 for octree)
	TArray<TSharedPtr<FFragmentTile>> Children;

	FFragmentTile() = default;

	FFragmentTile(const FString& InTileID, int32 InLevel, const FIntVector& InIndices, const FBox& InBoundingBox)
		: TileID(InTileID)
		, Level(InLevel)
		, Indices(InIndices)
		, BoundingBox(InBoundingBox)
		, State(EFragmentTileState::Unloaded)
	{
	}

	// Calculate screen space error for this tile
	float CalculateScreenSpaceError(const FVector& CameraLocation, float VerticalFOV, float ViewportHeight) const;

	// Check if tile intersects camera frustum
	bool IntersectsFrustum(const FConvexVolume& Frustum) const;

	// Update last access time
	void Touch()
	{
		LastAccessTime = FPlatformTime::Seconds();
	}

	// Check if tile is evictable (loaded but not visible)
	bool IsEvictable() const
	{
		return State == EFragmentTileState::Loaded;
	}
};
