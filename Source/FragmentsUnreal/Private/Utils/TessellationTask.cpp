#include "Utils/TessellationTask.h"
#include "tesselator.h"
#include "Algo/Reverse.h"

DEFINE_LOG_CATEGORY_STATIC(LogTessellationTask, Log, All);

void FTessellationTask::DoWork()
{
	// Validate minimum required data
	if (Data.TaskId == 0)
	{
		FCString::Strcpy(Data.ErrorMessage, 256, TEXT("Invalid TaskId (0)"));
		Data.bSuccess = false;
		return;
	}

	if (Data.Points.Num() == 0)
	{
		FCString::Strcpy(Data.ErrorMessage, 256, TEXT("No points in geometry"));
		Data.bSuccess = false;
		return;
	}

	if (Data.GetNumProfiles() == 0)
	{
		FCString::Strcpy(Data.ErrorMessage, 256, TEXT("No profiles in geometry"));
		Data.bSuccess = false;
		return;
	}

	// Process each profile
	TArray<FVector> AllVertices;
	TArray<uint32> AllIndices;

	const int32 NumProfiles = Data.GetNumProfiles();
	const int32 NumPoints = Data.Points.Num();

	for (int32 ProfileIdx = 0; ProfileIdx < NumProfiles; ProfileIdx++)
	{
		// Get profile vertex indices using helper method (reconstructs from flat array)
		TArray<int32> ProfileVertexIndices = Data.GetProfileIndices(ProfileIdx);

		if (ProfileVertexIndices.Num() < 3)
		{
			continue;
		}

		// Check if this profile has holes
		bool bHasHoles = Data.ProfileHasHoles(ProfileIdx);

		if (!bHasHoles)
		{
			// Simple polygon - create triangle fan
			int32 BaseIndex = AllVertices.Num();

			// Add vertices
			for (int32 Idx : ProfileVertexIndices)
			{
				if (Idx >= 0 && Idx < Data.Points.Num())
				{
					AllVertices.Add(Data.Points[Idx]);
				}
			}

			// Create triangle fan (for convex polygons)
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
			TArray<TArray<int32>> ProfileHoles = Data.GetAllHolesForProfile(ProfileIdx);

			TArray<FVector> OutVertices;
			TArray<int32> OutIndices;

			if (TriangulatePolygonWithHoles(
				Data.Points,
				ProfileVertexIndices,
				ProfileHoles,
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
				UE_LOG(LogTessellationTask, Warning, TEXT("Tessellation failed for profile %d in mesh %s"),
					ProfileIdx, Data.MeshName);
			}
		}
	}

	if (AllVertices.Num() == 0 || AllIndices.Num() == 0)
	{
		FCString::Strcpy(Data.ErrorMessage, 256, TEXT("No geometry produced"));
		Data.bSuccess = false;
		return;
	}

	// Convert to FVector3f for output
	Data.OutPositions.Reserve(AllVertices.Num());
	for (const FVector& V : AllVertices)
	{
		Data.OutPositions.Add(FVector3f(V));
	}

	Data.OutIndices = MoveTemp(AllIndices);

	// Compute normals
	Data.OutNormals.SetNum(Data.OutPositions.Num());
	for (FVector3f& N : Data.OutNormals)
	{
		N = FVector3f::ZeroVector;
	}

	// Compute face normals and accumulate to vertices
	for (int32 i = 0; i < Data.OutIndices.Num(); i += 3)
	{
		uint32 I0 = Data.OutIndices[i];
		uint32 I1 = Data.OutIndices[i + 1];
		uint32 I2 = Data.OutIndices[i + 2];

		if (I0 < (uint32)Data.OutPositions.Num() &&
			I1 < (uint32)Data.OutPositions.Num() &&
			I2 < (uint32)Data.OutPositions.Num())
		{
			FVector3f V0 = Data.OutPositions[I0];
			FVector3f V1 = Data.OutPositions[I1];
			FVector3f V2 = Data.OutPositions[I2];

			FVector3f Edge1 = V1 - V0;
			FVector3f Edge2 = V2 - V0;
			FVector3f FaceNormal = FVector3f::CrossProduct(Edge1, Edge2).GetSafeNormal();

			Data.OutNormals[I0] += FaceNormal;
			Data.OutNormals[I1] += FaceNormal;
			Data.OutNormals[I2] += FaceNormal;
		}
	}

	// Normalize accumulated normals
	for (FVector3f& N : Data.OutNormals)
	{
		N = N.GetSafeNormal();
		if (N.IsNearlyZero())
		{
			N = FVector3f(0, 0, 1);
		}
	}

	// Generate simple UVs (planar projection)
	Data.OutUVs.SetNum(Data.OutPositions.Num());
	for (int32 i = 0; i < Data.OutPositions.Num(); i++)
	{
		// Simple planar projection
		Data.OutUVs[i] = FVector2f(Data.OutPositions[i].X * 0.01f, Data.OutPositions[i].Y * 0.01f);
	}

	Data.bSuccess = true;
}

FTessellationTask::FPlaneProjection FTessellationTask::BuildProjectionPlane(
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

bool FTessellationTask::IsClockwise(const TArray<FVector2D>& Points)
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

bool FTessellationTask::TriangulatePolygonWithHoles(
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
