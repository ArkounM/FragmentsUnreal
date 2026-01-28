#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/**
 * Result of a budget allocation request.
 */
struct FBudgetAllocationResult
{
	/** Whether any budget was allocated */
	bool bHasBudget = false;

	/** Allocated budget in milliseconds */
	float BudgetMs = 0.0f;

	FBudgetAllocationResult() = default;
	FBudgetAllocationResult(bool bInHasBudget, float InBudgetMs)
		: bHasBudget(bInHasBudget), BudgetMs(InBudgetMs) {}
};

/**
 * Coordinates frame time budget across geometry processing and tile spawning.
 * Prevents budget multiplication when multiple models are loaded simultaneously.
 *
 * Usage:
 *   FrameBudgetCoordinator.BeginFrame();
 *
 *   auto GeoBudget = FrameBudgetCoordinator.AllocateGeometryBudget();
 *   if (GeoBudget.bHasBudget)
 *       ProcessCompletedGeometry(GeoBudget.BudgetMs);
 *
 *   for (int i = 0; i < TileManagers.Num(); i++)
 *   {
 *       if (FrameBudgetCoordinator.IsBudgetExhausted()) break;
 *       auto SpawnBudget = FrameBudgetCoordinator.AllocateSpawnBudget(TileManagers.Num(), i);
 *       if (SpawnBudget.bHasBudget)
 *           TileManager->ProcessSpawnChunkWithBudget(SpawnBudget.BudgetMs);
 *   }
 *
 *   FrameBudgetCoordinator.EndFrame();
 */
struct FFrameBudgetCoordinator
{
	/** Total frame time budget in milliseconds (default: 4ms for 60 FPS with headroom) */
	float TotalFrameBudgetMs = 4.0f;

	/** Ratio of total budget allocated to geometry processing (0.0 to 1.0) */
	float GeometryBudgetRatio = 0.5f;

	/** Minimum budget threshold - skip phase if allocation would be below this (ms) */
	float MinimumBudgetThresholdMs = 0.5f;

	/** Enable adaptive budget adjustment based on actual frame times */
	bool bEnableAdaptiveBudget = true;

	/**
	 * Begin a new frame. Call at the start of ProcessAllTileManagerChunks.
	 */
	void BeginFrame()
	{
		FrameStartTime = FPlatformTime::Seconds();
		AllocatedBudgetMs = 0.0f;
		bInFrame = true;
	}

	/**
	 * End the current frame and update adaptive history.
	 * Call at the end of ProcessAllTileManagerChunks.
	 */
	void EndFrame()
	{
		if (!bInFrame) return;

		const float ActualFrameTimeMs = static_cast<float>((FPlatformTime::Seconds() - FrameStartTime) * 1000.0);

		// Update rolling average for adaptive budgeting
		if (bEnableAdaptiveBudget)
		{
			FrameTimeHistory[FrameHistoryIndex] = ActualFrameTimeMs;
			FrameHistoryIndex = (FrameHistoryIndex + 1) % FrameHistorySize;
			FrameHistoryCount = FMath::Min(FrameHistoryCount + 1, FrameHistorySize);
		}

		// Log statistics periodically (every 5 seconds)
		FramesSinceLastLog++;
		const double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastLogTime >= 5.0)
		{
			LogBudgetStatistics(ActualFrameTimeMs);
			LastLogTime = CurrentTime;
			FramesSinceLastLog = 0;
		}

		bInFrame = false;
	}

	/**
	 * Get remaining budget for this frame.
	 * @return Remaining budget in milliseconds
	 */
	float GetRemainingBudgetMs() const
	{
		if (!bInFrame) return 0.0f;

		const float ElapsedMs = static_cast<float>((FPlatformTime::Seconds() - FrameStartTime) * 1000.0);
		const float Remaining = TotalFrameBudgetMs - ElapsedMs;
		return FMath::Max(0.0f, Remaining);
	}

	/**
	 * Check if frame budget is exhausted.
	 * @return true if remaining budget is below minimum threshold
	 */
	bool IsBudgetExhausted() const
	{
		return GetRemainingBudgetMs() < MinimumBudgetThresholdMs;
	}

	/**
	 * Allocate budget for geometry processing phase.
	 * Gets GeometryBudgetRatio of total budget.
	 * @return Allocation result with budget amount
	 */
	FBudgetAllocationResult AllocateGeometryBudget()
	{
		if (!bInFrame || IsBudgetExhausted())
		{
			return FBudgetAllocationResult(false, 0.0f);
		}

		const float GeometryBudgetMs = TotalFrameBudgetMs * GeometryBudgetRatio;

		// Clamp to remaining budget
		const float ActualBudget = FMath::Min(GeometryBudgetMs, GetRemainingBudgetMs());

		if (ActualBudget < MinimumBudgetThresholdMs)
		{
			return FBudgetAllocationResult(false, 0.0f);
		}

		AllocatedBudgetMs += ActualBudget;
		return FBudgetAllocationResult(true, ActualBudget);
	}

	/**
	 * Allocate budget for spawn processing phase.
	 * Distributes remaining budget proportionally among TileManagers.
	 * @param TotalTileManagers Total number of tile managers to distribute among
	 * @param Index Index of current tile manager (0-based)
	 * @return Allocation result with budget amount
	 */
	FBudgetAllocationResult AllocateSpawnBudget(int32 TotalTileManagers, int32 Index)
	{
		if (!bInFrame || IsBudgetExhausted() || TotalTileManagers <= 0)
		{
			return FBudgetAllocationResult(false, 0.0f);
		}

		const float RemainingBudget = GetRemainingBudgetMs();

		// Distribute remaining budget among remaining TileManagers
		const int32 RemainingManagers = TotalTileManagers - Index;
		if (RemainingManagers <= 0)
		{
			return FBudgetAllocationResult(false, 0.0f);
		}

		// Each manager gets an equal share of remaining budget
		const float PerManagerBudget = RemainingBudget / static_cast<float>(RemainingManagers);

		if (PerManagerBudget < MinimumBudgetThresholdMs)
		{
			return FBudgetAllocationResult(false, 0.0f);
		}

		AllocatedBudgetMs += PerManagerBudget;
		return FBudgetAllocationResult(true, PerManagerBudget);
	}

	/**
	 * Get average frame time from history (for adaptive budgeting).
	 * @return Average frame time in milliseconds, or TotalFrameBudgetMs if no history
	 */
	float GetAverageFrameTimeMs() const
	{
		if (FrameHistoryCount == 0)
		{
			return TotalFrameBudgetMs;
		}

		float Sum = 0.0f;
		for (int32 i = 0; i < FrameHistoryCount; i++)
		{
			Sum += FrameTimeHistory[i];
		}
		return Sum / static_cast<float>(FrameHistoryCount);
	}

private:
	/** Frame start time in seconds */
	double FrameStartTime = 0.0;

	/** Total budget allocated this frame */
	float AllocatedBudgetMs = 0.0f;

	/** Whether we're currently in a frame */
	bool bInFrame = false;

	/** Frame time history for adaptive budgeting */
	static constexpr int32 FrameHistorySize = 60; // 1 second at 60 FPS
	float FrameTimeHistory[FrameHistorySize] = {0.0f};
	int32 FrameHistoryIndex = 0;
	int32 FrameHistoryCount = 0;

	/** Logging */
	double LastLogTime = 0.0;
	int32 FramesSinceLastLog = 0;

	void LogBudgetStatistics(float LastFrameTimeMs) const
	{
		const float AvgFrameTime = GetAverageFrameTimeMs();
		const float BudgetUtilization = (AllocatedBudgetMs / TotalFrameBudgetMs) * 100.0f;

		UE_LOG(LogTemp, Log, TEXT("[FrameBudgetCoordinator] Budget: %.1fms, Last: %.2fms, Avg: %.2fms, Utilization: %.0f%%"),
			TotalFrameBudgetMs, LastFrameTimeMs, AvgFrameTime, BudgetUtilization);
	}
};
