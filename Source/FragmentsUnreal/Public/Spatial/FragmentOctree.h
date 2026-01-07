#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentTile.h"
#include "FragmentOctree.generated.h"

/**
 * Octree node (internal or leaf)
 */
USTRUCT()
struct FFragmentOctreeNode
{
	GENERATED_BODY()

	// Node bounds
	FBox Bounds;

	// Child nodes (8 octants, empty if leaf)
	TArray<FFragmentOctreeNode*> Children;

	// Tile data (only valid for leaf nodes)
	UFragmentTile* Tile = nullptr;

	// Depth in tree (root = 0)
	int32 Depth = 0;

	bool IsLeaf() const { return Children.Num() == 0; }

	~FFragmentOctreeNode()
	{
		for (FFragmentOctreeNode* Child : Children)
		{
			delete Child;
		}
		Children.Empty();
	}
};

/**
 * Spatial octree for fragment tiles.
 * Subdivides the model into a hierarchy of tiles for efficient frustum culling.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentOctree : public UObject
{
	GENERATED_BODY()

public:
	UFragmentOctree();
	~UFragmentOctree();

	/**
	 * Build octree from fragment model
	 * @param ModelWrapper The fragment model to subdivide
	 * @param ModelGuid Model identifier
	 */
	void BuildFromModel(const class UFragmentModelWrapper* ModelWrapper, const FString& ModelGuid);

	/**
	 * Query tiles intersecting camera frustum
	 * @param Frustum Camera frustum planes
	 * @param OutTiles Output array of visible tiles
	 */
	void QueryVisibleTiles(const FConvexVolume& Frustum, TArray<UFragmentTile*>& OutTiles);

	/**
	 * Query tiles within distance of point
	 * @param Location Center point
	 * @param Range Distance in cm
	 * @param OutTiles Output array of tiles in range
	 */
	void QueryTilesInRange(const FVector& Location, float Range, TArray<UFragmentTile*>& OutTiles);

	/**
	 * Get all tiles (for debug visualization)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	void GetAllTiles(TArray<UFragmentTile*>& OutTiles);

	// Configuration
	UPROPERTY(EditAnywhere, Category = "Octree")
	int32 MaxDepth = 4;

	UPROPERTY(EditAnywhere, Category = "Octree")
	int32 MaxFragmentsPerTile = 100;

	UPROPERTY(EditAnywhere, Category = "Octree")
	float MinTileSize = 1000.0f; // cm

	/**
	 * Calculate geometric error from bounding box (Entwine 2.1 formula)
	 * @param Box Tile bounding box
	 * @return Geometric error (max_half_extent / 8)
	 */
	static float CalculateGeometricError(const FBox& Box);

	/**
	 * Get root error multiplier for distant visibility
	 * Higher values = visible from farther away (default: 16.0 for buildings)
	 */
	static float GetRootErrorMultiplier() { return 16.0f; }

private:
	FFragmentOctreeNode* Root = nullptr;

	// Owned tiles (for memory management)
	UPROPERTY()
	TArray<UFragmentTile*> Tiles;

	// Model reference (stored during build)
	FString ModelGuidRef;

	/**
	 * Recursive node building
	 */
	void BuildNode(FFragmentOctreeNode* Node, const TArray<int32>& FragmentIDs,
	               const TMap<int32, FBox>& FragmentBounds, int32 CurrentDepth);

	/**
	 * Recursive frustum query
	 */
	void QueryNodeFrustum(FFragmentOctreeNode* Node, const FConvexVolume& Frustum,
	                      TArray<UFragmentTile*>& OutTiles);

	/**
	 * Recursive range query
	 */
	void QueryNodeRange(FFragmentOctreeNode* Node, const FVector& Location, float Range,
	                    TArray<UFragmentTile*>& OutTiles);

	/**
	 * Recursive all tiles collection
	 */
	void CollectAllTilesRecursive(FFragmentOctreeNode* Node, TArray<UFragmentTile*>& OutTiles);

	/**
	 * Calculate bounds for fragment list
	 */
	FBox CalculateBounds(const TArray<int32>& FragmentIDs,
	                     const TMap<int32, FBox>& FragmentBounds);

	/**
	 * Recursively collect fragment bounding boxes from hierarchy
	 */
	void CollectFragmentBounds(const FFragmentItem& Item,
	                            const Model* ParsedModel,
	                            TMap<int32, FBox>& OutBounds,
	                            TArray<int32>& OutFragmentIDs);
};
