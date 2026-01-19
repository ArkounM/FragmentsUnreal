#include "Spatial/OcclusionSpawnController.h"
#include "Spatial/FragmentRegistry.h"
#include "Fragment/Fragment.h"

DEFINE_LOG_CATEGORY_STATIC(LogOcclusionController, Log, All);

UOcclusionSpawnController::UOcclusionSpawnController()
{
}

void UOcclusionSpawnController::Initialize(UFragmentRegistry* InRegistry)
{
	FragmentRegistry = InRegistry;
	Reset();

	UE_LOG(LogOcclusionController, Log, TEXT("OcclusionSpawnController initialized (FramesBeforeDefer=%d, FramesToUnDefer=%d)"),
	       FramesBeforeDefer, FramesToUnDefer);
}

void UOcclusionSpawnController::UpdateOcclusionTracking(
	const TSet<int32>& RenderedFragments,
	const TSet<int32>& AllVisibleFragments)
{
	if (!bEnableOcclusionDeferral)
	{
		return;
	}

	// Update tracking for all visible fragments
	for (int32 LocalId : AllVisibleFragments)
	{
		const bool bWasRendered = RenderedFragments.Contains(LocalId);
		UpdateFragmentTracking(LocalId, bWasRendered, true);
	}

	// Clean up tracking for fragments that are no longer visible
	// (This prevents stale data accumulation)
	TArray<int32> ToRemove;
	for (auto& Pair : OcclusionTracking)
	{
		if (!AllVisibleFragments.Contains(Pair.Key))
		{
			// Fragment is no longer visible - remove from tracking after a while
			// to allow for re-spawning without immediate deferral
			Pair.Value.OccludedFrameCount = 0;
			Pair.Value.VisibleFrameCount = 0;

			// Remove from deferred set if it was deferred
			if (DeferredFragments.Contains(Pair.Key))
			{
				DeferredFragments.Remove(Pair.Key);
			}
		}
	}

	UE_LOG(LogOcclusionController, VeryVerbose,
	       TEXT("Occlusion tracking: %d rendered / %d visible, %d deferred"),
	       RenderedFragments.Num(), AllVisibleFragments.Num(), DeferredFragments.Num());
}

void UOcclusionSpawnController::UpdateFragmentTracking(int32 LocalId, bool bWasRendered, bool bWasVisible)
{
	FOcclusionTrackingData& Data = OcclusionTracking.FindOrAdd(LocalId);

	if (bWasRendered)
	{
		// Fragment was rendered - reset occluded count, increment visible count
		Data.OccludedFrameCount = 0;
		Data.VisibleFrameCount++;

		// Check if we should un-defer this fragment
		if (Data.bDeferred && Data.VisibleFrameCount >= FramesToUnDefer)
		{
			Data.bDeferred = false;
			DeferredFragments.Remove(LocalId);

			UE_LOG(LogOcclusionController, Verbose,
			       TEXT("Fragment %d restored from deferred state (visible for %d frames)"),
			       LocalId, Data.VisibleFrameCount);
		}
	}
	else if (bWasVisible)
	{
		// Fragment was visible but NOT rendered (occluded) - increment occluded count
		Data.VisibleFrameCount = 0;
		Data.OccludedFrameCount++;

		// Check if we should defer this fragment
		if (!Data.bDeferred && Data.OccludedFrameCount >= FramesBeforeDefer)
		{
			Data.bDeferred = true;
			DeferredFragments.Add(LocalId);

			UE_LOG(LogOcclusionController, Verbose,
			       TEXT("Fragment %d deferred (occluded for %d frames)"),
			       LocalId, Data.OccludedFrameCount);
		}
	}
}

bool UOcclusionSpawnController::ShouldDeferSpawn(int32 LocalId) const
{
	if (!bEnableOcclusionDeferral)
	{
		return false;
	}

	return DeferredFragments.Contains(LocalId);
}

float UOcclusionSpawnController::GetSpawnPriority(int32 LocalId, float BaseDistance) const
{
	if (!bEnableOcclusionDeferral)
	{
		return BaseDistance;
	}

	// Deferred fragments get a penalty multiplier to push them back in the queue
	if (ShouldDeferSpawn(LocalId))
	{
		return BaseDistance * DeferredPriorityPenalty;
	}

	return BaseDistance;
}

bool UOcclusionSpawnController::IsFragmentDeferred(int32 LocalId) const
{
	return DeferredFragments.Contains(LocalId);
}

void UOcclusionSpawnController::Reset()
{
	OcclusionTracking.Empty();
	DeferredFragments.Empty();

	UE_LOG(LogOcclusionController, Log, TEXT("OcclusionSpawnController reset"));
}
