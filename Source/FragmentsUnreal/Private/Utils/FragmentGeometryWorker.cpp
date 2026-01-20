#include "Utils/FragmentGeometryWorker.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "tesselator.h"
#include "Algo/Reverse.h"

DEFINE_LOG_CATEGORY_STATIC(LogGeometryWorker, Log, All);

//////////////////////////////////////////////////////////////////////////
// FFragmentGeometryWorker Implementation
//////////////////////////////////////////////////////////////////////////

FFragmentGeometryWorker::FFragmentGeometryWorker(
	TQueue<FGeometryWorkItem, EQueueMode::Mpsc>* InWorkQueue,
	TQueue<FRawGeometryData, EQueueMode::Mpsc>* InCompletionQueue,
	FThreadSafeCounter* InPendingWorkCount,
	int32 InWorkerId)
	: WorkQueue(InWorkQueue)
	, CompletionQueue(InCompletionQueue)
	, PendingWorkCount(InPendingWorkCount)
	, WorkerId(InWorkerId)
{
	bShouldStop = false;
	bIsRunning = false;
	WorkAvailableEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

FFragmentGeometryWorker::~FFragmentGeometryWorker()
{
	StopThread();

	if (WorkAvailableEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WorkAvailableEvent);
		WorkAvailableEvent = nullptr;
	}
}

bool FFragmentGeometryWorker::Init()
{
	UE_LOG(LogGeometryWorker, Log, TEXT("Geometry Worker %d initializing"), WorkerId);
	return true;
}

uint32 FFragmentGeometryWorker::Run()
{
	bIsRunning = true;
	UE_LOG(LogGeometryWorker, Log, TEXT("Geometry Worker %d started"), WorkerId);

	while (!bShouldStop)
	{
		FGeometryWorkItem WorkItem;

		// Try to dequeue work
		if (WorkQueue->Dequeue(WorkItem))
		{
			// Process the work item
			FRawGeometryData Result = ProcessWorkItem(WorkItem);

			// Push result to completion queue
			CompletionQueue->Enqueue(MoveTemp(Result));

			// Decrement pending count
			PendingWorkCount->Decrement();
		}
		else
		{
			// No work available, wait for signal or timeout
			WorkAvailableEvent->Wait(10); // 10ms timeout to check bShouldStop
		}
	}

	bIsRunning = false;
	UE_LOG(LogGeometryWorker, Log, TEXT("Geometry Worker %d stopped"), WorkerId);
	return 0;
}

void FFragmentGeometryWorker::Stop()
{
	bShouldStop = true;
	if (WorkAvailableEvent)
	{
		WorkAvailableEvent->Trigger();
	}
}

void FFragmentGeometryWorker::Exit()
{
	UE_LOG(LogGeometryWorker, Log, TEXT("Geometry Worker %d exiting"), WorkerId);
}

void FFragmentGeometryWorker::StartThread()
{
	if (Thread == nullptr)
	{
		bShouldStop = false;
		Thread = FRunnableThread::Create(
			this,
			*FString::Printf(TEXT("FragmentGeometryWorker_%d"), WorkerId),
			0, // Default stack size
			TPri_BelowNormal // Lower priority than game thread
		);
	}
}

void FFragmentGeometryWorker::StopThread()
{
	bShouldStop = true;

	if (WorkAvailableEvent)
	{
		WorkAvailableEvent->Trigger();
	}

	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

FRawGeometryData FFragmentGeometryWorker::ProcessWorkItem(const FGeometryWorkItem& WorkItem)
{
	switch (WorkItem.Type)
	{
	case FGeometryWorkItem::EWorkType::Shell:
		return ProcessShell(WorkItem);

	case FGeometryWorkItem::EWorkType::CircleExtrusion:
		return ProcessCircleExtrusion(WorkItem);

	default:
		FRawGeometryData ErrorResult;
		ErrorResult.bSuccess = false;
		ErrorResult.ErrorMessage = TEXT("Unknown work item type");
		ErrorResult.WorkItemId = WorkItem.WorkItemId;
		return ErrorResult;
	}
}

FRawGeometryData FFragmentGeometryWorker::ProcessShell(const FGeometryWorkItem& WorkItem)
{
	FRawGeometryData Result;
	Result.WorkItemId = WorkItem.WorkItemId;
	Result.LocalId = WorkItem.LocalId;
	Result.SampleIndex = WorkItem.SampleIndex;
	Result.ModelGuid = WorkItem.ModelGuid;
	Result.MeshName = WorkItem.MeshName;
	Result.PackagePath = WorkItem.PackagePath;
	Result.LocalTransform = WorkItem.LocalTransform;
	Result.GlobalTransform = WorkItem.GlobalTransform;
	Result.Category = WorkItem.Category;
	Result.ParentLocalId = WorkItem.ParentLocalId;
	Result.R = WorkItem.R;
	Result.G = WorkItem.G;
	Result.B = WorkItem.B;
	Result.A = WorkItem.A;
	Result.bIsGlass = WorkItem.bIsGlass;

	// Process each profile
	TArray<FVector> AllVertices;
	TArray<uint32> AllIndices;

	for (int32 ProfileIdx = 0; ProfileIdx < WorkItem.ProfileIndices.Num(); ProfileIdx++)
	{
		const TArray<int32>& ProfileVertexIndices = WorkItem.ProfileIndices[ProfileIdx];

		if (ProfileVertexIndices.Num() < 3)
		{
			continue;
		}

		// Check if this profile has holes
		bool bHasHoles = ProfileIdx < WorkItem.ProfileHoles.Num() && WorkItem.ProfileHoles[ProfileIdx].Num() > 0;

		if (!bHasHoles)
		{
			// Simple polygon - create triangle fan
			int32 BaseIndex = AllVertices.Num();

			// Add vertices
			for (int32 Idx : ProfileVertexIndices)
			{
				if (Idx >= 0 && Idx < WorkItem.Points.Num())
				{
					AllVertices.Add(WorkItem.Points[Idx]);
				}
			}

			// Create triangle fan (for convex polygons)
			// For non-convex, we should still tessellate, but this is a fast path for simple cases
			int32 NumVerts = ProfileVertexIndices.Num();
			for (int32 i = 1; i < NumVerts - 1; i++)
			{
				AllIndices.Add(BaseIndex);
				AllIndices.Add(BaseIndex + i);
				AllIndices.Add(BaseIndex + i + 1);
			}
		}
		else
		{
			// Profile with holes - use tessellation
			TArray<FVector> OutVertices;
			TArray<int32> OutIndices;

			if (TriangulatePolygonWithHoles(
				WorkItem.Points,
				ProfileVertexIndices,
				WorkItem.ProfileHoles[ProfileIdx],
				OutVertices,
				OutIndices))
			{
				int32 BaseIndex = AllVertices.Num();

				// Add tessellated vertices
				AllVertices.Append(OutVertices);

				// Add indices with offset
				for (int32 Idx : OutIndices)
				{
					AllIndices.Add(BaseIndex + Idx);
				}
			}
			else
			{
				UE_LOG(LogGeometryWorker, Warning, TEXT("Tessellation failed for profile %d in mesh %s"),
					ProfileIdx, *WorkItem.MeshName);
			}
		}
	}

	if (AllVertices.Num() == 0 || AllIndices.Num() == 0)
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("No geometry produced");
		return Result;
	}

	// Convert to FVector3f for output
	Result.Positions.Reserve(AllVertices.Num());
	for (const FVector& V : AllVertices)
	{
		Result.Positions.Add(FVector3f(V));
	}

	Result.Indices = MoveTemp(AllIndices);

	// Compute normals
	Result.Normals.SetNum(Result.Positions.Num());
	for (FVector3f& N : Result.Normals)
	{
		N = FVector3f::ZeroVector;
	}

	// Compute face normals and accumulate to vertices
	for (int32 i = 0; i < Result.Indices.Num(); i += 3)
	{
		uint32 I0 = Result.Indices[i];
		uint32 I1 = Result.Indices[i + 1];
		uint32 I2 = Result.Indices[i + 2];

		if (I0 < (uint32)Result.Positions.Num() &&
			I1 < (uint32)Result.Positions.Num() &&
			I2 < (uint32)Result.Positions.Num())
		{
			FVector3f V0 = Result.Positions[I0];
			FVector3f V1 = Result.Positions[I1];
			FVector3f V2 = Result.Positions[I2];

			FVector3f Edge1 = V1 - V0;
			FVector3f Edge2 = V2 - V0;
			FVector3f FaceNormal = FVector3f::CrossProduct(Edge1, Edge2).GetSafeNormal();

			Result.Normals[I0] += FaceNormal;
			Result.Normals[I1] += FaceNormal;
			Result.Normals[I2] += FaceNormal;
		}
	}

	// Normalize accumulated normals
	for (FVector3f& N : Result.Normals)
	{
		N = N.GetSafeNormal();
		if (N.IsNearlyZero())
		{
			N = FVector3f(0, 0, 1);
		}
	}

	// Generate simple UVs (planar projection)
	Result.UVs.SetNum(Result.Positions.Num());
	for (int32 i = 0; i < Result.Positions.Num(); i++)
	{
		// Simple planar projection
		Result.UVs[i] = FVector2f(Result.Positions[i].X * 0.01f, Result.Positions[i].Y * 0.01f);
	}

	Result.bSuccess = true;
	return Result;
}

FRawGeometryData FFragmentGeometryWorker::ProcessCircleExtrusion(const FGeometryWorkItem& WorkItem)
{
	// TODO: Implement CircleExtrusion processing
	// For now, return an error - CircleExtrusion will fall back to sync path
	FRawGeometryData Result;
	Result.WorkItemId = WorkItem.WorkItemId;
	Result.LocalId = WorkItem.LocalId;
	Result.SampleIndex = WorkItem.SampleIndex;
	Result.ModelGuid = WorkItem.ModelGuid;
	Result.MeshName = WorkItem.MeshName;
	Result.bSuccess = false;
	Result.ErrorMessage = TEXT("CircleExtrusion async processing not yet implemented");
	return Result;
}

FFragmentGeometryWorker::FPlaneProjection FFragmentGeometryWorker::BuildProjectionPlane(
	const TArray<FVector>& Points, const TArray<int32>& Profile)
{
	FPlaneProjection Projection;
	Projection.Origin = FVector::ZeroVector;
	Projection.AxisX = FVector::ForwardVector;
	Projection.AxisY = FVector::RightVector;

	if (Profile.Num() < 3)
	{
		return Projection;
	}

	// Get first valid point as origin
	for (int32 Idx : Profile)
	{
		if (Idx >= 0 && Idx < Points.Num())
		{
			Projection.Origin = Points[Idx];
			break;
		}
	}

	// Find two non-collinear edges to define the plane
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;
	bool bFound = false;

	for (int32 i = 1; i < Profile.Num() - 1 && !bFound; i++)
	{
		int32 Idx0 = Profile[0];
		int32 Idx1 = Profile[i];
		int32 Idx2 = Profile[i + 1];

		if (Idx0 >= 0 && Idx0 < Points.Num() &&
			Idx1 >= 0 && Idx1 < Points.Num() &&
			Idx2 >= 0 && Idx2 < Points.Num())
		{
			A = Points[Idx1] - Points[Idx0];
			B = Points[Idx2] - Points[Idx0];

			if (!A.IsNearlyZero() && !B.IsNearlyZero() && !A.Equals(B, KINDA_SMALL_NUMBER))
			{
				bFound = true;
			}
		}
	}

	if (!bFound)
	{
		return Projection;
	}

	FVector Normal = FVector::CrossProduct(A, B).GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		return Projection;
	}

	Projection.AxisX = A.GetSafeNormal();
	Projection.AxisY = FVector::CrossProduct(Normal, Projection.AxisX);

	return Projection;
}

bool FFragmentGeometryWorker::IsClockwise(const TArray<FVector2D>& Points)
{
	if (Points.Num() < 3)
	{
		return false;
	}

	double Sum = 0.0;
	for (int32 i = 0; i < Points.Num(); i++)
	{
		int32 Next = (i + 1) % Points.Num();
		Sum += (Points[Next].X - Points[i].X) * (Points[Next].Y + Points[i].Y);
	}

	return Sum > 0.0;
}

bool FFragmentGeometryWorker::TriangulatePolygonWithHoles(
	const TArray<FVector>& Points,
	const TArray<int32>& ProfileIndices,
	const TArray<TArray<int32>>& Holes,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutIndices)
{
	TESStesselator* Tess = tessNewTess(nullptr);
	if (!Tess)
	{
		return false;
	}

	// Build projection plane
	FPlaneProjection Projection = BuildProjectionPlane(Points, ProfileIndices);

	auto ProjectPoint = [&Projection](const FVector& P) -> FVector2D
	{
		FVector Local = P - Projection.Origin;
		return FVector2D(
			FVector::DotProduct(Local, Projection.AxisX),
			FVector::DotProduct(Local, Projection.AxisY)
		);
	};

	auto AddContour = [&](const TArray<int32>& Indices, bool bIsHole)
	{
		TArray<FVector2D> Projected;
		TArray<float> Contour;

		for (int32 Index : Indices)
		{
			if (Index >= 0 && Index < Points.Num())
			{
				FVector2D P2d = ProjectPoint(Points[Index]);
				Projected.Add(P2d);
			}
		}

		if (Projected.Num() < 3)
		{
			return;
		}

		// Remove duplicate consecutive points
		TArray<FVector2D> UniqueProjected;
		for (int32 i = 0; i < Projected.Num(); ++i)
		{
			if (i == 0 || !Projected[i].Equals(Projected[i - 1], 0.001))
			{
				UniqueProjected.Add(Projected[i]);
			}
		}
		Projected = MoveTemp(UniqueProjected);

		if (Projected.Num() < 3)
		{
			return;
		}

		// Fix winding
		bool bClockwise = IsClockwise(Projected);
		if (!bIsHole && bClockwise)
		{
			Algo::Reverse(Projected); // Outer should be CCW
		}
		else if (bIsHole && !bClockwise)
		{
			Algo::Reverse(Projected); // Holes should be CW
		}

		for (const FVector2D& P : Projected)
		{
			Contour.Add(P.X);
			Contour.Add(P.Y);
		}

		tessAddContour(Tess, 2, Contour.GetData(), sizeof(float) * 2, Projected.Num());
	};

	// Add outer contour
	AddContour(ProfileIndices, false);

	// Add holes
	for (const TArray<int32>& Hole : Holes)
	{
		AddContour(Hole, true);
	}

	// Tessellate
	if (!tessTesselate(Tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, nullptr))
	{
		tessDeleteTess(Tess);
		return false;
	}

	// Extract results
	const float* Verts = tessGetVertices(Tess);
	const int* Elems = tessGetElements(Tess);
	int NumVerts = tessGetVertexCount(Tess);
	int NumElems = tessGetElementCount(Tess);

	if (NumVerts == 0 || NumElems == 0)
	{
		tessDeleteTess(Tess);
		return false;
	}

	// Convert 2D tessellation back to 3D
	OutVertices.Reserve(NumVerts);
	for (int i = 0; i < NumVerts; i++)
	{
		FVector2D P2D(Verts[i * 2], Verts[i * 2 + 1]);
		FVector P3D = Projection.Origin + Projection.AxisX * P2D.X + Projection.AxisY * P2D.Y;
		OutVertices.Add(P3D);
	}

	// Extract triangles
	OutIndices.Reserve(NumElems * 3);
	for (int i = 0; i < NumElems; i++)
	{
		int I0 = Elems[i * 3];
		int I1 = Elems[i * 3 + 1];
		int I2 = Elems[i * 3 + 2];

		if (I0 != TESS_UNDEF && I1 != TESS_UNDEF && I2 != TESS_UNDEF)
		{
			OutIndices.Add(I0);
			OutIndices.Add(I1);
			OutIndices.Add(I2);
		}
	}

	tessDeleteTess(Tess);
	return OutIndices.Num() > 0;
}

//////////////////////////////////////////////////////////////////////////
// FGeometryWorkerPool Implementation
//////////////////////////////////////////////////////////////////////////

FGeometryWorkerPool::FGeometryWorkerPool()
{
	NextWorkItemId.Set(1);
}

FGeometryWorkerPool::~FGeometryWorkerPool()
{
	Shutdown();
}

void FGeometryWorkerPool::Initialize(int32 NumWorkers)
{
	if (bIsInitialized)
	{
		return;
	}

	// Default to half the available cores, minimum 2, maximum 8
	if (NumWorkers <= 0)
	{
		NumWorkers = FMath::Clamp(FPlatformMisc::NumberOfCores() / 2, 2, 8);
	}

	UE_LOG(LogGeometryWorker, Log, TEXT("Initializing Geometry Worker Pool with %d workers"), NumWorkers);

	Workers.Reserve(NumWorkers);
	for (int32 i = 0; i < NumWorkers; i++)
	{
		FFragmentGeometryWorker* Worker = new FFragmentGeometryWorker(
			&WorkQueue,
			&CompletionQueue,
			&PendingWorkCount,
			i
		);
		Worker->StartThread();
		Workers.Add(Worker);
	}

	bIsInitialized = true;
}

void FGeometryWorkerPool::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	UE_LOG(LogGeometryWorker, Log, TEXT("Shutting down Geometry Worker Pool"));

	// Stop all workers
	for (FFragmentGeometryWorker* Worker : Workers)
	{
		if (Worker)
		{
			Worker->StopThread();
			delete Worker;
		}
	}
	Workers.Empty();

	// Clear queues
	FGeometryWorkItem DummyWork;
	while (WorkQueue.Dequeue(DummyWork)) {}

	FRawGeometryData DummyResult;
	while (CompletionQueue.Dequeue(DummyResult)) {}

	PendingWorkCount.Reset();
	bIsInitialized = false;
}

void FGeometryWorkerPool::SubmitWork(const FGeometryWorkItem& WorkItem)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogGeometryWorker, Warning, TEXT("Cannot submit work - pool not initialized"));
		return;
	}

	PendingWorkCount.Increment();
	WorkQueue.Enqueue(WorkItem);
}

void FGeometryWorkerPool::SubmitWork(FGeometryWorkItem&& WorkItem)
{
	if (!bIsInitialized)
	{
		UE_LOG(LogGeometryWorker, Warning, TEXT("Cannot submit work - pool not initialized"));
		return;
	}

	PendingWorkCount.Increment();
	WorkQueue.Enqueue(MoveTemp(WorkItem));
}

bool FGeometryWorkerPool::HasCompletedWork() const
{
	return !CompletionQueue.IsEmpty();
}

bool FGeometryWorkerPool::DequeueCompletedWork(FRawGeometryData& OutResult)
{
	return CompletionQueue.Dequeue(OutResult);
}

uint64 FGeometryWorkerPool::GenerateWorkItemId()
{
	return static_cast<uint64>(NextWorkItemId.Increment());
}

//////////////////////////////////////////////////////////////////////////
// FGeometryDataExtractor Implementation
//////////////////////////////////////////////////////////////////////////

FGeometryWorkItem FGeometryDataExtractor::ExtractShellWorkItem(
	const Shell* ShellRef,
	const Material* MaterialRef,
	const FFragmentItem& FragmentItem,
	int32 SampleIndex,
	const FString& MeshName,
	const FString& PackagePath,
	const FTransform& LocalTransform,
	AActor* ParentActor,
	bool bSaveMeshes,
	uint64 WorkItemId)
{
	FGeometryWorkItem WorkItem;
	WorkItem.WorkItemId = WorkItemId;
	WorkItem.Type = FGeometryWorkItem::EWorkType::Shell;

	// Copy identification data
	WorkItem.LocalId = FragmentItem.LocalId;
	WorkItem.SampleIndex = SampleIndex;
	WorkItem.ModelGuid = FragmentItem.ModelGuid;
	WorkItem.MeshName = MeshName;
	WorkItem.PackagePath = PackagePath;
	WorkItem.LocalTransform = LocalTransform;
	WorkItem.GlobalTransform = FragmentItem.GlobalTransform;
	WorkItem.Category = FragmentItem.Category;
	WorkItem.ParentActor = ParentActor;
	WorkItem.bSaveMeshes = bSaveMeshes;

	// Copy fragment item for spawning later
	WorkItem.FragmentItemCopy = FragmentItem;
	// Clear children to avoid deep copy issues
	WorkItem.FragmentItemCopy.FragmentChildren.Empty();

	// Extract material data
	if (MaterialRef)
	{
		WorkItem.R = MaterialRef->r();
		WorkItem.G = MaterialRef->g();
		WorkItem.B = MaterialRef->b();
		WorkItem.A = MaterialRef->a();
		// Glass is determined by alpha < 255 (transparency)
		WorkItem.bIsGlass = WorkItem.A < 255;
	}

	// Sanity check limits to prevent crashes from corrupted data
	constexpr uint32 MaxPointCount = 1000000;
	constexpr uint32 MaxProfileCount = 100000;
	constexpr uint32 MaxIndicesPerProfile = 100000;

	// Extract points from FlatBuffers (COPY for thread safety)
	if (ShellRef && ShellRef->points())
	{
		const auto* Points = ShellRef->points();
		const uint32 PointCount = Points->size();

		if (PointCount > MaxPointCount)
		{
			UE_LOG(LogGeometryWorker, Warning, TEXT("ExtractShellWorkItem: Point count %u exceeds limit, skipping mesh %s"),
				PointCount, *MeshName);
			return WorkItem;
		}

		WorkItem.Points.Reserve(PointCount);
		for (flatbuffers::uoffset_t i = 0; i < PointCount; i++)
		{
			const auto* P = Points->Get(i);
			if (P)
			{
				// Convert to Unreal coordinates: Z-up, cm units
				WorkItem.Points.Add(FVector(P->x() * 100, P->z() * 100, P->y() * 100));
			}
		}
	}

	// Extract profiles from FlatBuffers (COPY)
	if (ShellRef && ShellRef->profiles())
	{
		const auto* Profiles = ShellRef->profiles();
		const uint32 ProfileCount = Profiles->size();

		if (ProfileCount > MaxProfileCount)
		{
			UE_LOG(LogGeometryWorker, Warning, TEXT("ExtractShellWorkItem: Profile count %u exceeds limit, skipping mesh %s"),
				ProfileCount, *MeshName);
			return WorkItem;
		}

		// First, map holes to profiles
		TMap<int32, TArray<TArray<int32>>> ProfileHolesMap;
		if (ShellRef->holes())
		{
			const auto* Holes = ShellRef->holes();
			const uint32 HoleCount = Holes->size();

			for (flatbuffers::uoffset_t j = 0; j < HoleCount && j < MaxProfileCount; j++)
			{
				const auto* Hole = Holes->Get(j);
				if (!Hole) continue;

				int32 ProfileId = Hole->profile_id();

				TArray<int32> HoleIndices;
				if (Hole->indices())
				{
					const uint32 HoleIndexCount = Hole->indices()->size();
					if (HoleIndexCount <= MaxIndicesPerProfile)
					{
						HoleIndices.Reserve(HoleIndexCount);
						for (flatbuffers::uoffset_t k = 0; k < HoleIndexCount; k++)
						{
							HoleIndices.Add(Hole->indices()->Get(k));
						}
					}
				}

				ProfileHolesMap.FindOrAdd(ProfileId).Add(MoveTemp(HoleIndices));
			}
		}

		WorkItem.ProfileIndices.Reserve(ProfileCount);
		WorkItem.ProfileHoles.SetNum(ProfileCount);

		for (flatbuffers::uoffset_t i = 0; i < ProfileCount; i++)
		{
			const auto* Profile = Profiles->Get(i);
			TArray<int32> ProfileVertexIndices;

			if (Profile && Profile->indices())
			{
				const uint32 IndexCount = Profile->indices()->size();
				if (IndexCount <= MaxIndicesPerProfile)
				{
					ProfileVertexIndices.Reserve(IndexCount);
					for (flatbuffers::uoffset_t j = 0; j < IndexCount; j++)
					{
						ProfileVertexIndices.Add(Profile->indices()->Get(j));
					}
				}
				else
				{
					UE_LOG(LogGeometryWorker, Warning, TEXT("ExtractShellWorkItem: Profile %d has %u indices (limit %u), skipping"),
						i, IndexCount, MaxIndicesPerProfile);
				}
			}

			WorkItem.ProfileIndices.Add(MoveTemp(ProfileVertexIndices));

			// Copy holes for this profile
			if (TArray<TArray<int32>>* HolesForProfile = ProfileHolesMap.Find(i))
			{
				WorkItem.ProfileHoles[i] = *HolesForProfile;
			}
		}
	}

	return WorkItem;
}

FGeometryWorkItem FGeometryDataExtractor::ExtractCircleExtrusionWorkItem(
	const CircleExtrusion* ExtrusionRef,
	const Material* MaterialRef,
	const FFragmentItem& FragmentItem,
	int32 SampleIndex,
	const FString& MeshName,
	const FString& PackagePath,
	const FTransform& LocalTransform,
	AActor* ParentActor,
	bool bSaveMeshes,
	uint64 WorkItemId)
{
	// TODO: Implement CircleExtrusion data extraction
	// For now, return a minimal work item that will fail processing
	// This ensures CircleExtrusion falls back to synchronous path

	FGeometryWorkItem WorkItem;
	WorkItem.WorkItemId = WorkItemId;
	WorkItem.Type = FGeometryWorkItem::EWorkType::CircleExtrusion;
	WorkItem.LocalId = FragmentItem.LocalId;
	WorkItem.SampleIndex = SampleIndex;
	WorkItem.ModelGuid = FragmentItem.ModelGuid;
	WorkItem.MeshName = MeshName;
	WorkItem.PackagePath = PackagePath;

	return WorkItem;
}
