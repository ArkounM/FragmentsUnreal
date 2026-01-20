#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"

/**
 * Raw geometry data produced by worker threads.
 * This is a thread-safe data structure that contains all the information
 * needed to create a UStaticMesh on the game thread.
 */
struct FRawGeometryData
{
	// Geometry data
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<uint32> Indices;
	TArray<FVector2f> UVs;

	// Material data
	uint8 R = 255;
	uint8 G = 255;
	uint8 B = 255;
	uint8 A = 255;
	bool bIsGlass = false;

	// Fragment identification
	int32 LocalId = -1;
	int32 SampleIndex = -1;
	FString ModelGuid;
	FString MeshName;
	FString PackagePath;
	FTransform LocalTransform;
	FTransform GlobalTransform;
	FString Category;

	// For hierarchy reconstruction
	int32 ParentLocalId = -1;

	// Work item reference for matching
	uint64 WorkItemId = 0;

	// Success flag
	bool bSuccess = false;
	FString ErrorMessage;

	FRawGeometryData() = default;
};

/**
 * Work item submitted to geometry worker threads.
 * Contains all the data needed to process geometry on a background thread.
 */
struct FGeometryWorkItem
{
	enum class EWorkType : uint8
	{
		Shell,
		CircleExtrusion
	};

	// Work identification
	uint64 WorkItemId = 0;
	EWorkType Type = EWorkType::Shell;

	// Geometry source data (copied from FlatBuffers for thread safety)
	// Shell data
	TArray<FVector> Points;
	TArray<TArray<int32>> ProfileIndices;  // Each profile's vertex indices
	TArray<TArray<TArray<int32>>> ProfileHoles;  // Holes per profile

	// CircleExtrusion data - simplified for now
	// TODO: Add CircleExtrusion data when implementing that path

	// Material data
	uint8 R = 255;
	uint8 G = 255;
	uint8 B = 255;
	uint8 A = 255;
	bool bIsGlass = false;

	// Fragment identification
	int32 LocalId = -1;
	int32 SampleIndex = -1;
	FString ModelGuid;
	FString MeshName;
	FString PackagePath;
	FTransform LocalTransform;
	FTransform GlobalTransform;
	FString Category;
	int32 ParentLocalId = -1;

	// Parent actor for spawning (weak reference, only valid on game thread)
	TWeakObjectPtr<AActor> ParentActor;

	// Note: We don't copy FFragmentItem as it contains raw pointers (FragmentChildren)
	// that are not safe to copy across threads. All necessary data is extracted above.
	bool bSaveMeshes = false;

	FGeometryWorkItem() = default;
};

/**
 * Worker thread that processes geometry work items.
 * Reads from a shared work queue, processes geometry (tessellation, etc.),
 * and pushes results to a completion queue.
 */
class FRAGMENTSUNREAL_API FFragmentGeometryWorker : public FRunnable
{
public:
	FFragmentGeometryWorker(
		TQueue<FGeometryWorkItem, EQueueMode::Mpsc>* InWorkQueue,
		TQueue<FRawGeometryData, EQueueMode::Mpsc>* InCompletionQueue,
		FThreadSafeCounter* InPendingWorkCount,
		int32 InWorkerId);

	virtual ~FFragmentGeometryWorker();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	/** Start the worker thread */
	void StartThread();

	/** Request the worker to stop */
	void StopThread();

	/** Check if the worker is running */
	bool IsRunning() const { return bIsRunning; }

private:
	/** Process a single work item */
	FRawGeometryData ProcessWorkItem(const FGeometryWorkItem& WorkItem);

	/** Process Shell geometry (tessellation) */
	FRawGeometryData ProcessShell(const FGeometryWorkItem& WorkItem);

	/** Process CircleExtrusion geometry */
	FRawGeometryData ProcessCircleExtrusion(const FGeometryWorkItem& WorkItem);

	/** Triangulate a polygon with holes using libtess2 */
	bool TriangulatePolygonWithHoles(
		const TArray<FVector>& Points,
		const TArray<int32>& ProfileIndices,
		const TArray<TArray<int32>>& Holes,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutIndices);

	/** Simple plane projection for 2D tessellation (worker-local copy) */
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

private:
	// Shared queues (owned by pool)
	TQueue<FGeometryWorkItem, EQueueMode::Mpsc>* WorkQueue;
	TQueue<FRawGeometryData, EQueueMode::Mpsc>* CompletionQueue;
	FThreadSafeCounter* PendingWorkCount;

	// Thread management
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bShouldStop;
	FThreadSafeBool bIsRunning;
	int32 WorkerId;

	// Event for waking worker when work is available
	FEvent* WorkAvailableEvent = nullptr;
};

/**
 * Pool of geometry worker threads.
 * Manages work distribution and completion collection.
 */
class FRAGMENTSUNREAL_API FGeometryWorkerPool
{
public:
	FGeometryWorkerPool();
	~FGeometryWorkerPool();

	/** Initialize the worker pool with specified number of threads */
	void Initialize(int32 NumWorkers = 0);

	/** Shutdown all workers */
	void Shutdown();

	/** Submit a work item for processing */
	void SubmitWork(const FGeometryWorkItem& WorkItem);

	/** Submit a work item for processing (move version) */
	void SubmitWork(FGeometryWorkItem&& WorkItem);

	/** Check if there are completed items available */
	bool HasCompletedWork() const;

	/** Dequeue a completed work item (returns false if none available) */
	bool DequeueCompletedWork(FRawGeometryData& OutResult);

	/** Get number of pending work items */
	int32 GetPendingWorkCount() const { return PendingWorkCount.GetValue(); }

	/** Check if the pool is initialized */
	bool IsInitialized() const { return bIsInitialized; }

	/** Generate unique work item ID */
	uint64 GenerateWorkItemId();

private:
	// Worker threads
	TArray<FFragmentGeometryWorker*> Workers;

	// Work queues
	TQueue<FGeometryWorkItem, EQueueMode::Mpsc> WorkQueue;
	TQueue<FRawGeometryData, EQueueMode::Mpsc> CompletionQueue;

	// Tracking
	FThreadSafeCounter PendingWorkCount;
	FThreadSafeCounter NextWorkItemId;

	// State
	bool bIsInitialized = false;
};

/**
 * Helper class to extract geometry data from FlatBuffers structures
 * in a thread-safe manner (by copying the data).
 */
class FRAGMENTSUNREAL_API FGeometryDataExtractor
{
public:
	/**
	 * Extract Shell data from FlatBuffers into a work item.
	 * This copies all necessary data so the work item can be processed
	 * on a background thread without accessing FlatBuffers memory.
	 */
	static FGeometryWorkItem ExtractShellWorkItem(
		const Shell* ShellRef,
		const Material* MaterialRef,
		const FFragmentItem& FragmentItem,
		int32 SampleIndex,
		const FString& MeshName,
		const FString& PackagePath,
		const FTransform& LocalTransform,
		AActor* ParentActor,
		bool bSaveMeshes,
		uint64 WorkItemId);

	/**
	 * Extract CircleExtrusion data from FlatBuffers into a work item.
	 * TODO: Implement when adding CircleExtrusion async support.
	 */
	static FGeometryWorkItem ExtractCircleExtrusionWorkItem(
		const CircleExtrusion* ExtrusionRef,
		const Material* MaterialRef,
		const FFragmentItem& FragmentItem,
		int32 SampleIndex,
		const FString& MeshName,
		const FString& PackagePath,
		const FTransform& LocalTransform,
		AActor* ParentActor,
		bool bSaveMeshes,
		uint64 WorkItemId);
};
