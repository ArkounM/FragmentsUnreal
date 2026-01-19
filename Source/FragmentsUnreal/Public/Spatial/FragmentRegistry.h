#pragma once

#include "CoreMinimal.h"
#include "Utils/FragmentOcclusionTypes.h"
#include "FragmentRegistry.generated.h"

// Forward declarations
class UFragmentModelWrapper;

/**
 * Per-fragment visibility data for sample-based streaming.
 * Stores pre-computed information needed for per-sample visibility evaluation.
 */
USTRUCT(BlueprintType)
struct FFragmentVisibilityData
{
	GENERATED_BODY()

	/** Fragment local ID (unique within model) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 LocalId = -1;

	/** Pre-computed world-space axis-aligned bounding box */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FBox WorldBounds;

	/** Maximum dimension of the bounding box (for screen size calculation) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	float MaxDimension = 0.0f;

	/** Material index for tile grouping (CRC hash input) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 MaterialIndex = 0;

	/** Whether this is a small object (dimension < 200cm) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bIsSmallObject = false;

	/** Primary representation index for geometry lookup */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 RepresentationIndex = -1;

	/** Global ID (GUID string) for external references */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	/** Category for filtering (e.g., "Wall", "Door", "Window") */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	/** Occlusion role classification for GPU occlusion culling */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	EOcclusionRole OcclusionRole = EOcclusionRole::Occludee;

	/** Material alpha value for transparency detection (0-255) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	uint8 MaterialAlpha = 255;

	FFragmentVisibilityData()
		: WorldBounds(ForceInit)
	{
	}
};

/**
 * Fragment Registry - flat array of all fragments with pre-computed visibility data.
 *
 * This replaces the hierarchical octree approach with a flat list that enables
 * per-sample (per-fragment) visibility testing. Each fragment is tested individually
 * against the camera frustum, preventing the tile-based culling bug where camera
 * inside a tile causes the entire tile to disappear.
 *
 * Key differences from octree approach:
 * - Flat array for cache-friendly iteration (64 bytes per entry)
 * - Individual fragment bounds, not tile bounds
 * - LocalId lookup for fast access when grouping
 * - Pre-computed MaxDimension for screen size calculation
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentRegistry : public UObject
{
	GENERATED_BODY()

public:
	UFragmentRegistry();

	/**
	 * Build registry from fragment model.
	 * Extracts bounding boxes and visibility data for all fragments.
	 * @param ModelWrapper Source model with parsed FlatBuffers data
	 * @param ModelGuid Model identifier for logging
	 */
	void BuildFromModel(const UFragmentModelWrapper* ModelWrapper, const FString& ModelGuid);

	/**
	 * Get all fragments in the registry (const reference for iteration).
	 * @return Array of fragment visibility data
	 */
	const TArray<FFragmentVisibilityData>& GetAllFragments() const { return Fragments; }

	/**
	 * Get fragment count.
	 * @return Number of registered fragments
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	int32 GetFragmentCount() const { return Fragments.Num(); }

	/**
	 * Find fragment by local ID.
	 * @param LocalId Fragment local ID to find
	 * @return Pointer to visibility data, or nullptr if not found
	 */
	const FFragmentVisibilityData* FindFragment(int32 LocalId) const;

	/**
	 * Get index of fragment by local ID.
	 * @param LocalId Fragment local ID
	 * @return Index in Fragments array, or INDEX_NONE if not found
	 */
	int32 GetFragmentIndex(int32 LocalId) const;

	/**
	 * Get total memory usage estimate for the registry.
	 * @return Memory in bytes
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	int64 GetMemoryUsage() const;

	/**
	 * Get world bounds encompassing all fragments.
	 * @return Combined bounding box of all fragments
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	FBox GetWorldBounds() const { return WorldBounds; }

	/**
	 * Check if registry has been built.
	 * @return true if BuildFromModel has been called successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Registry")
	bool IsBuilt() const { return bIsBuilt; }

	/** Small object size threshold in cm (200 = 2m, matching engine_fragment) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Registry")
	float SmallObjectSize = 200.0f;

private:
	/** Flat array of all fragment visibility data */
	UPROPERTY()
	TArray<FFragmentVisibilityData> Fragments;

	/** Fast lookup from LocalId to array index */
	UPROPERTY()
	TMap<int32, int32> LocalIdToIndex;

	/** World bounds encompassing all fragments */
	FBox WorldBounds;

	/** Model GUID reference */
	FString ModelGuidRef;

	/** Whether registry has been built */
	bool bIsBuilt = false;

	/**
	 * Recursively collect fragment visibility data from hierarchy.
	 * @param Item Current fragment item to process
	 * @param ParsedModel FlatBuffers model for bounding box extraction
	 */
	void CollectFragmentData(const struct FFragmentItem& Item, const struct Model* ParsedModel);
};
