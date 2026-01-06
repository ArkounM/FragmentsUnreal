#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Spatial/FragmentTile.h"
#include "FragmentTileManager.generated.h"

// Forward declarations
class UFragmentOctree;
class UFragmentsImporter;

/**
 * Manages tile-based fragment streaming based on camera frustum.
 * Handles tile state transitions, spawning, and unloading.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentTileManager : public UObject
{
	GENERATED_BODY()

public:
	UFragmentTileManager();

	/**
	 * Initialize the tile manager with model data
	 * @param InModelGuid Model identifier
	 * @param InOctree Spatial octree for frustum queries
	 * @param InImporter Reference to importer for spawning
	 */
	void Initialize(const FString& InModelGuid, UFragmentOctree* InOctree, UFragmentsImporter* InImporter);

	/**
	 * Update visible tiles based on camera frustum
	 * @param CameraLocation Camera world position
	 * @param CameraRotation Camera rotation
	 * @param FOV Field of view in degrees
	 * @param AspectRatio Screen aspect ratio (width/height)
	 */
	void UpdateVisibleTiles(const FVector& CameraLocation, const FRotator& CameraRotation,
	                        float FOV, float AspectRatio);

	/**
	 * Process one spawn chunk (called per frame by timer)
	 * Spawns 1 fragment from loading tiles
	 */
	void ProcessSpawnChunk();

	/**
	 * Get current spawn progress (0.0 to 1.0)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetSpawnProgress() const { return SpawnProgress; }

	/**
	 * Get current loading stage description
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	FString GetLoadingStage() const { return LoadingStage; }

	/**
	 * Check if currently loading tiles
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	bool IsLoading() const;

	// --- Configuration ---

	/** How often to update visible tiles (seconds) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	float CameraUpdateInterval = 0.2f;

	/** How long after leaving frustum before unloading (seconds) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	float UnloadHysteresis = 5.0f;

	/** Number of fragments to spawn per chunk (1 = one per frame) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	int32 FragmentsPerChunk = 1;

	/** Minimum camera movement to trigger update (cm) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	float MinCameraMovement = 500.0f;

private:
	// --- State ---

	/** Model identifier */
	FString ModelGuid;

	/** Spatial octree for queries */
	UPROPERTY()
	UFragmentOctree* Octree = nullptr;

	/** Importer reference for spawning */
	UPROPERTY()
	UFragmentsImporter* Importer = nullptr;

	/** Currently visible tiles */
	UPROPERTY()
	TArray<UFragmentTile*> VisibleTiles;

	/** All loaded tiles (visible + in hysteresis) */
	UPROPERTY()
	TArray<UFragmentTile*> LoadedTiles;

	/** Last camera position used for update */
	FVector LastCameraPosition = FVector::ZeroVector;

	/** Last update time */
	double LastUpdateTime = 0.0;

	/** Total fragments to spawn across all loading tiles */
	int32 TotalFragmentsToSpawn = 0;

	/** Total fragments spawned so far */
	int32 FragmentsSpawned = 0;

	/** Current spawn progress (0.0 to 1.0) */
	float SpawnProgress = 0.0f;

	/** Current loading stage description */
	FString LoadingStage = TEXT("Idle");

	// --- Helper Methods ---

	/**
	 * Update tile states based on new visible set
	 * @param NewVisibleTiles Tiles returned from frustum query
	 */
	void UpdateTileStates(const TArray<UFragmentTile*>& NewVisibleTiles);

	/**
	 * Start loading a tile (transition Unloaded → Loading)
	 * @param Tile Tile to start loading
	 */
	void StartLoadingTile(UFragmentTile* Tile);

	/**
	 * Mark tile as visible (transition Loaded → Visible)
	 * @param Tile Tile to show
	 */
	void ShowTile(UFragmentTile* Tile);

	/**
	 * Start unload timer for tile (transition Visible → Loaded)
	 * @param Tile Tile to hide
	 */
	void HideTile(UFragmentTile* Tile);

	/**
	 * Unload tile actors (transition Loaded → Unloaded)
	 * @param Tile Tile to unload
	 */
	void UnloadTile(UFragmentTile* Tile);

	/**
	 * Spawn a single fragment from a loading tile
	 * @param Tile Tile to spawn from
	 * @return true if fragment was spawned
	 */
	bool SpawnFragmentFromTile(UFragmentTile* Tile);

	/**
	 * Build frustum volume from camera parameters
	 * @param CameraLocation Camera world position
	 * @param CameraRotation Camera rotation
	 * @param FOV Field of view in degrees
	 * @param AspectRatio Screen aspect ratio
	 * @return Frustum volume for culling
	 */
	FConvexVolume BuildCameraFrustum(const FVector& CameraLocation, const FRotator& CameraRotation,
	                                  float FOV, float AspectRatio) const;

	/**
	 * Update spawn progress tracking
	 */
	void UpdateSpawnProgress();

	/**
	 * Process unload hysteresis timers
	 * @param DeltaTime Time since last update
	 */
	void ProcessUnloadTimers(float DeltaTime);

	/**
	 * Recursively add fragment and all children to set (hierarchy preservation)
	 * @param LocalID Fragment local ID to add
	 * @param Wrapper Model wrapper for hierarchy access
	 * @param OutSet Output set of all fragment IDs in subtree
	 */
	void AddFragmentSubtree(int32 LocalID, class UFragmentModelWrapper* Wrapper, TSet<int32>& OutSet);

	/**
	 * Recursively add fragment and all children in parent-first order (hierarchy preservation)
	 * @param LocalID Fragment local ID to add
	 * @param Wrapper Model wrapper for hierarchy access
	 * @param VisitedSet Set of already visited fragment IDs (avoid duplicates)
	 * @param OutOrderedList Output array in parent-first order
	 */
	void AddFragmentSubtreeOrdered(int32 LocalID, class UFragmentModelWrapper* Wrapper,
	                                TSet<int32>& VisitedSet, TArray<int32>& OutOrderedList);

	/**
	 * Find the parent FFragmentItem for a given fragment in the hierarchy
	 * @param Root Root of hierarchy to search
	 * @param Target Fragment to find parent for
	 * @return Parent FFragmentItem or nullptr if not found/is root
	 */
	FFragmentItem* FindParentFragmentItem(const FFragmentItem* Root, const FFragmentItem* Target);
};
