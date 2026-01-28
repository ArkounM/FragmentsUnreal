#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "Async/AsyncWork.h"

/**
 * Metadata for a single profile in the flattened geometry data.
 * Used to reconstruct profile indices from flat arrays.
 */
struct FTessProfileInfo
{
	int32 IndicesStart = 0;    // Offset into AllProfileIndices
	int32 IndicesCount = 0;    // Number of indices in this profile
	int32 FirstHoleIdx = INDEX_NONE;  // Index into HoleInfos (INDEX_NONE if no holes)
	int32 HoleCount = 0;       // Number of holes for this profile
};

/**
 * Metadata for a single hole in the flattened geometry data.
 * Used to reconstruct hole indices from flat arrays.
 */
struct FTessHoleInfo
{
	int32 IndicesStart = 0;    // Offset into AllHoleIndices
	int32 IndicesCount = 0;    // Number of indices in this hole
};

/**
 * Input/output data for tessellation task.
 * All data is owned by the task - no shared pointers to game thread objects.
 *
 * Uses TCHAR arrays instead of FString to avoid thread-local memory allocator issues
 * when transferring data between threads.
 */
struct FTessellationTaskData
{
	// === INPUT (set before StartBackgroundTask) ===

	// Geometry (flat arrays for thread safety)
	TArray<FVector> Points;
	TArray<int32> AllProfileIndices;    // All profile indices concatenated
	TArray<int32> AllHoleIndices;       // All hole indices concatenated
	TArray<FTessProfileInfo> ProfileInfos;  // Metadata for each profile (offsets/counts)
	TArray<FTessHoleInfo> HoleInfos;        // Metadata for each hole (offsets/counts)

	// Material
	uint8 R = 255;
	uint8 G = 255;
	uint8 B = 255;
	uint8 A = 255;
	bool bIsGlass = false;

	// Identification (for matching result to pending fragment)
	uint64 TaskId = 0;
	int32 LocalId = -1;
	int32 SampleIndex = -1;
	int32 RepresentationId = -1;

	// Paths as TCHAR arrays to avoid FString thread issues
	// Fixed sizes are acceptable (paths/names have reasonable max lengths)
	TCHAR ModelGuid[64];
	TCHAR MeshName[256];
	TCHAR PackagePath[512];
	TCHAR Category[128];

	// Transforms
	FTransform LocalTransform;
	FTransform GlobalTransform;

	// === OUTPUT (set by DoWork) ===
	TArray<FVector3f> OutPositions;
	TArray<uint32> OutIndices;
	TArray<FVector3f> OutNormals;
	TArray<FVector2f> OutUVs;

	bool bSuccess = false;
	TCHAR ErrorMessage[256];

	// Default constructor - initialize TCHAR arrays
	FTessellationTaskData()
	{
		FMemory::Memzero(ModelGuid, sizeof(ModelGuid));
		FMemory::Memzero(MeshName, sizeof(MeshName));
		FMemory::Memzero(PackagePath, sizeof(PackagePath));
		FMemory::Memzero(Category, sizeof(Category));
		FMemory::Memzero(ErrorMessage, sizeof(ErrorMessage));
	}

	// === HELPER METHODS ===

	/** Get the vertex indices for a specific profile */
	TArray<int32> GetProfileIndices(int32 ProfileIdx) const
	{
		TArray<int32> Result;
		if (ProfileIdx >= 0 && ProfileIdx < ProfileInfos.Num())
		{
			const FTessProfileInfo& Info = ProfileInfos[ProfileIdx];
			if (Info.IndicesCount > 0 && Info.IndicesStart >= 0 &&
				Info.IndicesStart + Info.IndicesCount <= AllProfileIndices.Num())
			{
				Result.Reserve(Info.IndicesCount);
				for (int32 i = 0; i < Info.IndicesCount; i++)
				{
					Result.Add(AllProfileIndices[Info.IndicesStart + i]);
				}
			}
		}
		return Result;
	}

	/** Get all holes for a specific profile as nested arrays */
	TArray<TArray<int32>> GetAllHolesForProfile(int32 ProfileIdx) const
	{
		TArray<TArray<int32>> Result;
		if (ProfileIdx >= 0 && ProfileIdx < ProfileInfos.Num())
		{
			const FTessProfileInfo& ProfInfo = ProfileInfos[ProfileIdx];
			if (ProfInfo.FirstHoleIdx != INDEX_NONE && ProfInfo.HoleCount > 0)
			{
				Result.Reserve(ProfInfo.HoleCount);
				for (int32 h = 0; h < ProfInfo.HoleCount; h++)
				{
					int32 HoleIdx = ProfInfo.FirstHoleIdx + h;
					if (HoleIdx >= 0 && HoleIdx < HoleInfos.Num())
					{
						const FTessHoleInfo& HInfo = HoleInfos[HoleIdx];
						TArray<int32> HoleIndices;
						if (HInfo.IndicesCount > 0 && HInfo.IndicesStart >= 0 &&
							HInfo.IndicesStart + HInfo.IndicesCount <= AllHoleIndices.Num())
						{
							HoleIndices.Reserve(HInfo.IndicesCount);
							for (int32 i = 0; i < HInfo.IndicesCount; i++)
							{
								HoleIndices.Add(AllHoleIndices[HInfo.IndicesStart + i]);
							}
						}
						Result.Add(MoveTemp(HoleIndices));
					}
				}
			}
		}
		return Result;
	}

	/** Check if a profile has any holes */
	bool ProfileHasHoles(int32 ProfileIdx) const
	{
		if (ProfileIdx >= 0 && ProfileIdx < ProfileInfos.Num())
		{
			return ProfileInfos[ProfileIdx].HoleCount > 0;
		}
		return false;
	}

	/** Get the number of profiles */
	int32 GetNumProfiles() const
	{
		return ProfileInfos.Num();
	}
};

/**
 * Async task for shell tessellation using libtess2.
 * Runs on Unreal's thread pool, completely off game thread.
 *
 * This replaces the previous FRunnable-based worker pool approach
 * which had issues with TQueue and Unreal's thread-local allocators.
 * FAsyncTask is Unreal's recommended pattern for background work.
 */
class FTessellationTask : public FNonAbandonableTask
{
	friend class FAsyncTask<FTessellationTask>;

public:
	FTessellationTaskData Data;

	FTessellationTask() = default;

	/**
	 * Main work function - runs on background thread.
	 * Performs libtess2 tessellation on the input geometry.
	 */
	void DoWork();

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FTessellationTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	/** Simple plane projection for 2D tessellation */
	struct FPlaneProjection
	{
		FVector Origin;
		FVector AxisX;
		FVector AxisY;
	};

	/** Build projection plane for 2D tessellation */
	FPlaneProjection BuildProjectionPlane(const TArray<FVector>& Points, const TArray<int32>& Profile);

	/** Check if polygon winding is clockwise */
	bool IsClockwise(const TArray<FVector2D>& Points);

	/** Triangulate a polygon with holes using libtess2 */
	bool TriangulatePolygonWithHoles(
		const TArray<FVector>& Points,
		const TArray<int32>& ProfileIndices,
		const TArray<TArray<int32>>& Holes,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutIndices);
};
