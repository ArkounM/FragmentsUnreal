#pragma once

#include "CoreMinimal.h"
#include "OcclusionSpawnController.generated.h"

// Forward declarations
class UFragmentRegistry;
class AFragment;

/**
 * Tracking data for a single fragment's occlusion state.
 * Used to determine if a fragment should be deferred from spawning.
 */
USTRUCT()
struct FOcclusionTrackingData
{
	GENERATED_BODY()

	/** Number of consecutive frames this fragment has been occluded */
	int32 OccludedFrameCount = 0;

	/** Number of consecutive frames this fragment has been visible */
	int32 VisibleFrameCount = 0;

	/** Whether this fragment is currently marked as deferred */
	bool bDeferred = false;
};

/**
 * Controls deferred spawning based on GPU occlusion query results.
 *
 * Fragments that are consistently hidden behind walls (occluded for multiple
 * consecutive frames) are deprioritized in the spawn queue. When they become
 * visible again, they are restored to normal priority.
 *
 * Key features:
 * - Uses GetLastRenderTimeOnScreen() to detect rendered fragments
 * - Handles GPU query latency (1-2 frames) with configurable buffers
 * - Provides spawn priority adjustment based on occlusion state
 */
UCLASS()
class FRAGMENTSUNREAL_API UOcclusionSpawnController : public UObject
{
	GENERATED_BODY()

public:
	UOcclusionSpawnController();

	/**
	 * Initialize the controller with the fragment registry.
	 * @param InRegistry Fragment registry for accessing visibility data
	 */
	void Initialize(UFragmentRegistry* InRegistry);

	/**
	 * Update occlusion tracking based on rendering results.
	 * Call this each frame after rendering completes.
	 *
	 * @param RenderedFragments Set of LocalIds that were actually rendered this frame
	 * @param AllVisibleFragments Set of LocalIds that are currently visible (spawned and not hidden)
	 */
	void UpdateOcclusionTracking(
		const TSet<int32>& RenderedFragments,
		const TSet<int32>& AllVisibleFragments);

	/**
	 * Check if a fragment should be deprioritized in the spawn queue.
	 * A fragment is deferred if it has been consistently occluded.
	 *
	 * @param LocalId Fragment local ID to check
	 * @return true if the fragment should be spawned later
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	bool ShouldDeferSpawn(int32 LocalId) const;

	/**
	 * Get adjusted spawn priority for a fragment.
	 * Deferred fragments get lower priority (higher value = lower priority).
	 *
	 * @param LocalId Fragment local ID
	 * @param BaseDistance Base distance from camera
	 * @return Adjusted priority (lower = spawn first)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	float GetSpawnPriority(int32 LocalId, float BaseDistance) const;

	/**
	 * Check if a fragment is currently marked as deferred.
	 * @param LocalId Fragment local ID
	 * @return true if deferred
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	bool IsFragmentDeferred(int32 LocalId) const;

	/**
	 * Get the number of currently deferred fragments.
	 * @return Count of deferred fragments
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	int32 GetDeferredFragmentCount() const { return DeferredFragments.Num(); }

	/**
	 * Clear all occlusion tracking data.
	 * Call when loading a new model or resetting state.
	 */
	void Reset();

	// --- Configuration ---

	/** Number of consecutive occluded frames before deferring a fragment.
	 *  Set to 5 to handle 2 frames GPU latency + 3 frames buffer. */
	UPROPERTY(EditAnywhere, Category = "Occlusion", meta = (ClampMin = "1", ClampMax = "30"))
	int32 FramesBeforeDefer = 5;

	/** Number of consecutive visible frames to restore normal priority.
	 *  Set to 3 for quick recovery when camera moves. */
	UPROPERTY(EditAnywhere, Category = "Occlusion", meta = (ClampMin = "1", ClampMax = "10"))
	int32 FramesToUnDefer = 3;

	/** Enable occlusion-based deferral. Set to false to disable. */
	UPROPERTY(EditAnywhere, Category = "Occlusion")
	bool bEnableOcclusionDeferral = true;

	/** Priority penalty multiplier for deferred fragments.
	 *  Higher values push deferred fragments further back in the queue. */
	UPROPERTY(EditAnywhere, Category = "Occlusion", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float DeferredPriorityPenalty = 5.0f;

private:
	/** Reference to fragment registry */
	UPROPERTY()
	UFragmentRegistry* FragmentRegistry = nullptr;

	/** Per-fragment occlusion tracking data */
	TMap<int32, FOcclusionTrackingData> OcclusionTracking;

	/** Set of currently deferred fragments */
	TSet<int32> DeferredFragments;

	/**
	 * Update tracking for a single fragment.
	 * @param LocalId Fragment to update
	 * @param bWasRendered Whether fragment was rendered this frame
	 * @param bWasVisible Whether fragment was visible (spawned and not hidden)
	 */
	void UpdateFragmentTracking(int32 LocalId, bool bWasRendered, bool bWasVisible);
};
