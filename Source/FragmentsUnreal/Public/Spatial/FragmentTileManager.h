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
	 * @param ViewportHeight Viewport height in pixels (for SSE calculation)
	 */
	void UpdateVisibleTiles(const FVector& CameraLocation, const FRotator& CameraRotation,
	                        float FOV, float AspectRatio, float ViewportHeight);

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
	float CameraUpdateInterval = 0.1f; // 10 FPS for smoother rotation tracking

	/** How long after leaving frustum before unloading (seconds) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	float UnloadHysteresis = 10.0f; // CHANGED from 5.0f

	/** Maximum time to spend spawning per frame (milliseconds) */
	UPROPERTY(EditAnywhere, Category = "Streaming", meta = (ClampMin = "1.0", ClampMax = "16.0"))
	float MaxSpawnTimeMs = 4.0f; // 4ms like engine_fragment

	/** Minimum camera movement to trigger update (cm) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	float MinCameraMovement = 2500.0f; // CHANGED from 500.0f (5m → 25m)

	/** Minimum camera rotation to trigger update (degrees) */
	UPROPERTY(EditAnywhere, Category = "Streaming", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MinCameraRotation = 10.0f;

	/** Maximum screen space error in pixels (Cesium default: 16, lower = higher quality) */
	UPROPERTY(EditAnywhere, Category = "Streaming", meta = (ClampMin = "1.0", ClampMax = "128.0"))
	float MaximumScreenSpaceError = 16.0f;

	// --- Cache Configuration ---

	/** Maximum memory budget for tile cache in bytes (default: 512 MB) */
	UPROPERTY(EditAnywhere, Category = "Streaming|Cache")
	int64 MaxCachedBytes = 512 * 1024 * 1024;

	/** Enable tile caching (hide tiles instead of destroying them) */
	UPROPERTY(EditAnywhere, Category = "Streaming|Cache")
	bool bEnableTileCache = true;

	/** Use device RAM to auto-calculate cache budget (100 MB per GB) */
	UPROPERTY(EditAnywhere, Category = "Streaming|Cache")
	bool bAutoDetectCacheBudget = true;

	/** Minimum time a tile must be out of frustum before being eligible for eviction (seconds) */
	UPROPERTY(EditAnywhere, Category = "Streaming|Cache")
	float MinTimeBeforeUnload = 10.0f;

	// --- Cache Statistics ---

	/** Get current cache usage in megabytes */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetCacheUsageMB() const { return CurrentCacheBytes / (1024.0f * 1024.0f); }

	/** Get cache limit in megabytes */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetCacheLimitMB() const { return MaxCachedBytes / (1024.0f * 1024.0f); }

	/** Get cache usage as percentage (0-100) */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetCacheUsagePercent() const
	{
		if (MaxCachedBytes == 0) return 0.0f;
		return (CurrentCacheBytes * 100.0f) / MaxCachedBytes;
	}

	/** Get number of loaded tiles (in cache) */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	int32 GetLoadedTileCount() const { return LoadedTiles.Num(); }

	/** Get number of visible tiles */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	int32 GetVisibleTileCount() const { return VisibleTiles.Num(); }

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

	/** Last camera rotation used for update */
	FRotator LastCameraRotation = FRotator::ZeroRotator;

	/** Last update time */
	double LastUpdateTime = 0.0;

	/** Time of last significant camera movement (for deferring eviction/unload) */
	double LastCameraMovementTime = 0.0;

	/** Last camera location for priority sorting */
	FVector LastPriorityCameraLocation = FVector::ZeroVector;

	/** Last FOV for priority sorting */
	float LastPriorityFOV = 90.0f;

	/** Hash of last frustum state (for change detection) */
	uint32 LastFrustumHash = 0;

	/** Last aspect ratio used for frustum */
	float LastAspectRatio = 1.777f;

	/** Total fragments to spawn across all loading tiles */
	int32 TotalFragmentsToSpawn = 0;

	/** Total fragments spawned so far */
	int32 FragmentsSpawned = 0;

	/** Current spawn progress (0.0 to 1.0) */
	float SpawnProgress = 0.0f;

	/** Current loading stage description */
	FString LoadingStage = TEXT("Idle");

	// --- Cache State ---

	/** Current memory used by cached tiles (bytes) */
	int64 CurrentCacheBytes = 0;

	/** Last used time for each tile (for LRU eviction) */
	UPROPERTY()
	TMap<UFragmentTile*, double> TileLastUsedTime;

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
	 * Calculate screen space error for tile (Cesium/3D Tiles standard)
	 * SSE = (geometricError × viewportHeight) / (2 × distance × tan(fov/2))
	 * @param Tile Tile to evaluate
	 * @param CameraLocation Camera world position
	 * @param FOV Field of view in degrees
	 * @param ViewportHeight Viewport height in pixels
	 * @return Screen space error in pixels
	 */
	float CalculateScreenSpaceError(const UFragmentTile* Tile, const FVector& CameraLocation,
	                                 float FOV, float ViewportHeight) const;

	/**
	 * Calculate loading priority for a tile
	 * Higher priority = load first
	 * @param Tile Tile to prioritize
	 * @param CameraLocation Camera position
	 * @param FOV Field of view
	 * @return Priority score (higher = more important)
	 */
	float CalculateTilePriority(const UFragmentTile* Tile, const FVector& CameraLocation, float FOV) const;

	/**
	 * Draw debug wireframe for tile bounds (if debug enabled)
	 * @param Tile Tile to visualize
	 * @param Color Box color
	 * @param Thickness Line thickness
	 */
	void DrawDebugTileBounds(UFragmentTile* Tile, const FColor& Color, float Thickness = 2.0f);

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

	// --- Cache Management Methods ---

	/**
	 * Calculate approximate memory usage of a tile
	 * @param Tile Tile to measure
	 * @return Memory usage in bytes
	 */
	int64 CalculateTileMemoryUsage(UFragmentTile* Tile) const;

	/**
	 * Evict least recently used tiles to fit under memory budget
	 */
	void EvictTilesToFitBudget();

	/**
	 * Mark tile as recently used (updates LRU tracking)
	 * @param Tile Tile that was accessed
	 */
	void TouchTile(UFragmentTile* Tile);
};
