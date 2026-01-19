#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "FragmentTileManager.generated.h"

// Forward declarations
class UFragmentsImporter;
class UFragmentRegistry;
class UPerSampleVisibilityController;
class UDynamicTileGenerator;
class UOcclusionSpawnController;
class UFragmentModelWrapper;
struct FFragmentItem;

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
	 * @param InImporter Reference to importer for spawning
	 */
	void Initialize(const FString& InModelGuid, UFragmentsImporter* InImporter);

	/**
	 * Initialize per-sample visibility system
	 * @param InRegistry Fragment registry from model wrapper
	 */
	void InitializePerSampleVisibility(UFragmentRegistry* InRegistry);

	/**
	 * Update visible fragments based on camera frustum (per-sample visibility)
	 * @param CameraLocation Camera world position
	 * @param CameraRotation Camera rotation
	 * @param FOV Field of view in degrees
	 * @param AspectRatio Screen aspect ratio (width/height)
	 * @param ViewportHeight Viewport height in pixels (for SSE calculation)
	 */
	void UpdateVisibleTiles(const FVector& CameraLocation, const FRotator& CameraRotation,
	                        float FOV, float AspectRatio, float ViewportHeight);

	/**
	 * Process spawning/unloading based on dynamic tiles (per-sample visibility)
	 * Called each frame by timer
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
	float MinCameraMovement = 100.0f; // 1 meter - matches engine_fragment per-frame updates

	/** Minimum camera rotation to trigger update (degrees) */
	UPROPERTY(EditAnywhere, Category = "Streaming", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MinCameraRotation = 10.0f;

	/** Show all fragments regardless of frustum (debug mode) */
	UPROPERTY(EditAnywhere, Category = "Streaming")
	bool bShowAllVisible = false;

	/** Graphics quality multiplier (affects screen size thresholds) */
	UPROPERTY(EditAnywhere, Category = "Streaming", meta = (ClampMin = "0.5", ClampMax = "2.0"))
	float GraphicsQuality = 1.0f;

	/** Enable occlusion-based spawn deferral (fragments behind walls spawn later) */
	UPROPERTY(EditAnywhere, Category = "Streaming|Occlusion")
	bool bEnableOcclusionDeferral = true;

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
	float GetCacheUsageMB() const { return PerSampleCacheBytes / (1024.0f * 1024.0f); }

	/** Get cache limit in megabytes */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetCacheLimitMB() const { return MaxCachedBytes / (1024.0f * 1024.0f); }

	/** Get cache usage as percentage (0-100) */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	float GetCacheUsagePercent() const
	{
		if (MaxCachedBytes == 0) return 0.0f;
		return (PerSampleCacheBytes * 100.0f) / MaxCachedBytes;
	}

	/** Get number of visible fragments */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	int32 GetVisibleFragmentCount() const { return SpawnedFragments.Num(); }

	/** Get number of hidden (cached) fragments */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	int32 GetHiddenFragmentCount() const { return HiddenFragments.Num(); }

	/** Get total cached fragments (visible + hidden) */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	int32 GetTotalCachedFragmentCount() const { return SpawnedFragmentActors.Num(); }

private:
	// --- State ---

	/** Model identifier */
	FString ModelGuid;

	/** Importer reference for spawning */
	UPROPERTY()
	UFragmentsImporter* Importer = nullptr;

	/** Fragment registry for per-sample visibility */
	UPROPERTY()
	UFragmentRegistry* FragmentRegistry = nullptr;

	/** Per-sample visibility controller */
	UPROPERTY()
	UPerSampleVisibilityController* SampleVisibility = nullptr;

	/** Dynamic tile generator for CRC-based grouping */
	UPROPERTY()
	UDynamicTileGenerator* TileGenerator = nullptr;

	/** Occlusion-based spawn controller for deferred spawning */
	UPROPERTY()
	UOcclusionSpawnController* OcclusionController = nullptr;

	/** Set of currently spawned (visible) fragments */
	TSet<int32> SpawnedFragments;

	/** Set of currently hidden (but cached) fragments */
	TSet<int32> HiddenFragments;

	/** Map of spawned fragment actors (LocalId -> Actor) */
	UPROPERTY()
	TMap<int32, class AFragment*> SpawnedFragmentActors;

	/** Current memory used by per-sample cached fragments (bytes) */
	int64 PerSampleCacheBytes = 0;

	/** Last used time for each fragment (for LRU eviction) */
	TMap<int32, double> FragmentLastUsedTime;

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

	// --- Helper Methods ---

	/**
	 * Update spawn progress tracking
	 */
	void UpdateSpawnProgress();

	/**
	 * Find the parent FFragmentItem for a given fragment in the hierarchy
	 * @param Root Root of hierarchy to search
	 * @param Target Fragment to find parent for
	 * @return Parent FFragmentItem or nullptr if not found/is root
	 */
	FFragmentItem* FindParentFragmentItem(const FFragmentItem* Root, const FFragmentItem* Target);

private:
	/**
	 * Spawn a single fragment (per-sample mode).
	 * @param LocalId Fragment local ID to spawn
	 * @return true if fragment was spawned successfully
	 */
	bool SpawnFragmentById(int32 LocalId);

	/**
	 * Hide a single fragment (per-sample mode) - keeps in cache.
	 * Matches engine_fragment behavior: visibility toggle instead of destroy.
	 * @param LocalId Fragment local ID to hide
	 */
	void HideFragmentById(int32 LocalId);

	/**
	 * Show a previously hidden fragment (per-sample mode) - cache hit.
	 * @param LocalId Fragment local ID to show
	 * @return true if fragment was shown (existed in cache)
	 */
	bool ShowFragmentById(int32 LocalId);

	/**
	 * Destroy and unload a single fragment (per-sample mode).
	 * Only called during memory pressure eviction.
	 * @param LocalId Fragment local ID to unload
	 */
	void UnloadFragmentById(int32 LocalId);

	/**
	 * Calculate approximate memory usage of a single fragment actor.
	 * @param Actor Fragment actor to measure
	 * @return Memory usage in bytes
	 */
	int64 CalculateFragmentMemoryUsage(class AFragment* Actor) const;

	/**
	 * Evict least recently used fragments to fit under memory budget (per-sample mode).
	 * Matches engine_fragment: only evict when memory overflow AND fragment invisible.
	 */
	void EvictFragmentsToFitBudget();

	/**
	 * Mark fragment as recently used (updates LRU tracking).
	 * @param LocalId Fragment that was accessed
	 */
	void TouchFragment(int32 LocalId);

	/**
	 * Check if memory is over budget (per-sample mode).
	 * @return true if over budget
	 */
	bool IsPerSampleMemoryOverBudget() const;

	/**
	 * Collect set of fragments that were rendered this frame.
	 * Uses GetLastRenderTimeOnScreen() to detect GPU-rendered fragments.
	 * @return Set of LocalIds that were rendered
	 */
	TSet<int32> CollectRenderedFragments() const;

	/**
	 * Update occlusion tracking for deferred spawning.
	 * Called after rendering to track which fragments are occluded.
	 */
	void UpdateOcclusionTracking();
};
