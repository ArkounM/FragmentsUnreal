#pragma once

#include "CoreMinimal.h"
#include "Streaming/FragmentTile.h"
#include "Importer/FragmentModelWrapper.h"
#include "FragmentOctree.generated.h"

/**
 * Configuration for octree construction
 */
USTRUCT(BlueprintType)
struct FFragmentOctreeConfig
{
	GENERATED_BODY()

	// Maximum octree depth (0 = root only, 4 = typical)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree")
	int32 MaxDepth = 4;

	// Maximum fragments per leaf tile before subdivision
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree")
	int32 MaxFragmentsPerTile = 100;

	// Minimum tile size (cm) - don't subdivide below this
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Octree")
	float MinTileSize = 1000.0f; // 10 meters

	FFragmentOctreeConfig() = default;
};

/**
 * Spatial octree for fragment tiles.
 * Subdivides the model into a hierarchy of tiles for efficient streaming.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentOctree : public UObject
{
	GENERATED_BODY()

public:
	UFragmentOctree();

	/**
	 * Build octree from fragment model
	 * @param ModelWrapper The fragment model to subdivide
	 * @param Config Octree configuration
	 * @return True if successful
	 */
	bool BuildFromModel(UFragmentModelWrapper* ModelWrapper, const FFragmentOctreeConfig& Config);

	/**
	 * Query tiles visible to camera frustum
	 * @param Frustum Camera frustum planes
	 * @param CameraLocation Camera position in world space
	 * @param VerticalFOV Camera vertical field of view (degrees)
	 * @param ViewportHeight Viewport height in pixels
	 * @param MaxScreenSpaceError Maximum allowed screen space error (pixels)
	 * @return Array of visible tiles to render
	 */
	TArray<TSharedPtr<FFragmentTile>> QueryVisibleTiles(
		const FConvexVolume& Frustum,
		const FVector& CameraLocation,
		float VerticalFOV,
		float ViewportHeight,
		float MaxScreenSpaceError) const;

	/**
	 * Get all tiles (for debugging)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	TArray<FFragmentTile> GetAllTiles() const;

	/**
	 * Get root tile
	 */
	TSharedPtr<FFragmentTile> GetRootTile() const { return RootTile; }

	/**
	 * Get tile count at each level
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	TArray<int32> GetTileCountPerLevel() const;

private:
	// Root tile of the octree
	TSharedPtr<FFragmentTile> RootTile;

	// Configuration used to build this octree
	UPROPERTY()
	FFragmentOctreeConfig Config;

	// Reference to source model
	UPROPERTY()
	TObjectPtr<UFragmentModelWrapper> SourceModel;

	/**
	 * Recursively subdivide a tile
	 */
	void SubdivideTile(TSharedPtr<FFragmentTile> Tile, const TArray<int32>& FragmentLocalIDs);

	/**
	 * Calculate bounding box for a set of fragments
	 */
	FBox CalculateBoundingBox(const TArray<int32>& FragmentLocalIDs) const;

	/**
	 * Calculate geometric error for a tile
	 */
	float CalculateGeometricError(const TSharedPtr<FFragmentTile>& Tile, const TArray<int32>& FragmentLocalIDs) const;

	/**
	 * Assign fragments to child tiles based on spatial position
	 */
	void AssignFragmentsToChildren(const TSharedPtr<FFragmentTile>& ParentTile, const TArray<int32>& FragmentLocalIDs);

	/**
	 * Recursively query visible tiles
	 */
	void QueryVisibleTilesRecursive(
		const TSharedPtr<FFragmentTile>& Tile,
		const FConvexVolume& Frustum,
		const FVector& CameraLocation,
		float VerticalFOV,
		float ViewportHeight,
		float MaxScreenSpaceError,
		TArray<TSharedPtr<FFragmentTile>>& OutVisibleTiles) const;

	/**
	 * Recursively collect all tiles
	 */
	void CollectAllTilesRecursive(const TSharedPtr<FFragmentTile>& Tile, TArray<FFragmentTile>& OutTiles) const;
};
