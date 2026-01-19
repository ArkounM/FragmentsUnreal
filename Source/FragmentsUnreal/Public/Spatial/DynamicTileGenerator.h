#pragma once

#include "CoreMinimal.h"
#include "Spatial/PerSampleVisibilityController.h"
#include "Spatial/FragmentRegistry.h"
#include "DynamicTileGenerator.generated.h"

/**
 * Dynamic render tile generated from visible samples.
 * Groups fragments by material, LOD level, and spatial grid position
 * using CRC-32 hashing (matching engine_fragment's tile grouping).
 */
USTRUCT(BlueprintType)
struct FDynamicRenderTile
{
	GENERATED_BODY()

	/** Unique tile ID (CRC-32 hash of material+LOD+grid position) - not exposed to Blueprint */
	uint32 TileId = 0;

	/** Fragment local IDs belonging to this tile */
	UPROPERTY(BlueprintReadOnly, Category = "Tile")
	TArray<int32> FragmentLocalIds;

	/** Material index shared by fragments in this tile */
	UPROPERTY(BlueprintReadOnly, Category = "Tile")
	int32 MaterialIndex = 0;

	/** LOD level for this tile's fragments */
	UPROPERTY(BlueprintReadOnly, Category = "Tile")
	EFragmentLod LodLevel = EFragmentLod::Geometry;

	/** Grid position (snapped to tile dimension) */
	UPROPERTY(BlueprintReadOnly, Category = "Tile")
	FVector GridPosition = FVector::ZeroVector;

	/** Combined bounding box of all fragments in tile */
	UPROPERTY(BlueprintReadOnly, Category = "Tile")
	FBox Bounds;

	FDynamicRenderTile()
		: Bounds(ForceInit)
	{
	}
};

/**
 * Dynamic Tile Generator - CRC-based grouping of visible fragments.
 *
 * After per-sample visibility evaluation, this class groups the visible
 * fragments into dynamic tiles based on:
 * 1. Material index (batches same-material fragments for rendering)
 * 2. LOD level (separates Geometry from Wires)
 * 3. Spatial grid position (3D grid snap using CRC hash)
 *
 * This creates render batches that can be efficiently drawn together,
 * while allowing fragments to be regrouped every frame based on camera.
 *
 * Algorithm (matching engine_fragment's CRC tile ID generation):
 * 1. For each visible sample:
 *    - Snap bounds center to 3D grid
 *    - Compute CRC-32(materialIndex, lodLevel, gridX, gridY, gridZ)
 *    - Add fragment to tile with that CRC ID
 * 2. Output: Map of TileId -> FDynamicRenderTile
 */
UCLASS()
class FRAGMENTSUNREAL_API UDynamicTileGenerator : public UObject
{
	GENERATED_BODY()

public:
	UDynamicTileGenerator();

	/**
	 * Generate dynamic tiles from visible samples.
	 * Groups fragments by material+LOD+grid using CRC hashing.
	 *
	 * @param VisibleSamples Visibility results from PerSampleVisibilityController
	 * @param Registry Fragment registry for bounds lookup
	 */
	void GenerateTiles(const TArray<FFragmentVisibilityResult>& VisibleSamples,
	                   const UFragmentRegistry* Registry);

	/**
	 * Get generated tiles (const reference).
	 * @return Map of tile ID to tile data
	 */
	const TMap<uint32, FDynamicRenderTile>& GetTiles() const { return Tiles; }

	/**
	 * Get tile count.
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Tiles")
	int32 GetTileCount() const { return Tiles.Num(); }

	/**
	 * Get total fragment count across all tiles.
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Tiles")
	int32 GetTotalFragmentCount() const;

	/**
	 * Find tile containing a specific fragment.
	 * @param LocalId Fragment local ID
	 * @return Tile ID containing fragment, or 0 if not found
	 */
	uint32 FindTileForFragment(int32 LocalId) const;

	/**
	 * Get fragments that need to be spawned (not yet spawned).
	 * @param SpawnedFragments Set of already-spawned fragment IDs
	 * @return Array of fragment IDs that need spawning
	 */
	TArray<int32> GetFragmentsToSpawn(const TSet<int32>& SpawnedFragments) const;

	/**
	 * Get fragments that should be unloaded (no longer in any tile).
	 * @param SpawnedFragments Set of currently spawned fragment IDs
	 * @return Array of fragment IDs that should be unloaded
	 */
	TArray<int32> GetFragmentsToUnload(const TSet<int32>& SpawnedFragments) const;

	// --- Configuration ---

	/** Grid dimension for geometry tiles (cm) - default 320cm = 3.2m */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tiles")
	float GeometryTileDimension = 320.0f;

	/** Grid dimension for wire tiles (cm) - default 32cm = 0.32m */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tiles")
	float WiresTileDimension = 32.0f;

	/** Minimum fragments per tile to create (avoids excessive small tiles) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tiles", meta = (ClampMin = "1", ClampMax = "100"))
	int32 MinFragmentsPerTile = 1;

private:
	/** Generated tiles (tile ID -> tile data) */
	TMap<uint32, FDynamicRenderTile> Tiles;

	/** Reverse lookup (fragment ID -> tile ID) */
	TMap<int32, uint32> FragmentToTileId;

	/** All fragments currently in tiles */
	TSet<int32> AllTiledFragments;

	/**
	 * Compute CRC-32 tile ID for a fragment.
	 * Port of engine_fragment's tile ID generation.
	 *
	 * @param MaterialIndex Fragment material index
	 * @param Lod LOD level
	 * @param Center Bounds center position
	 * @return CRC-32 hash for tile grouping
	 */
	uint32 ComputeTileId(int32 MaterialIndex, EFragmentLod Lod, const FVector& Center) const;

	/**
	 * Compute CRC-32 checksum.
	 * Simple CRC-32 implementation for tile ID generation.
	 */
	static uint32 CRC32(const void* Data, int32 Length, uint32 PreviousCRC = 0);
};
