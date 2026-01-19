#include "Spatial/PerSampleVisibilityController.h"
#include "Spatial/FragmentRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogPerSampleVisibility, Log, All);

UPerSampleVisibilityController::UPerSampleVisibilityController()
{
}

void UPerSampleVisibilityController::Initialize(UFragmentRegistry* InRegistry)
{
	Registry = InRegistry;

	// Pre-allocate visible samples array based on registry size
	if (Registry && Registry->IsBuilt())
	{
		VisibleSamples.Reserve(Registry->GetFragmentCount());
	}

	// Reset state
	CurrentFrameIndex = 0;
	LastCameraPosition = FVector::ZeroVector;
	LastCameraRotation = FRotator::ZeroRotator;

	UE_LOG(LogPerSampleVisibility, Log, TEXT("PerSampleVisibilityController initialized with %d fragments"),
	       Registry ? Registry->GetFragmentCount() : 0);
}

void UPerSampleVisibilityController::UpdateVisibility(const FVector& CameraPos, const FRotator& CameraRot,
                                                       float FOV, float AspectRatio, float ViewportHeight)
{
	if (!Registry || !Registry->IsBuilt())
	{
		UE_LOG(LogPerSampleVisibility, Warning, TEXT("UpdateVisibility: No valid registry"));
		return;
	}

	// Update view state
	ViewState.CameraPosition = CameraPos;
	ViewState.CameraForward = CameraRot.Vector();
	ViewState.FOV = FOV;
	ViewState.ViewportHeight = ViewportHeight;
	ViewState.ViewportWidth = ViewportHeight * AspectRatio;
	ViewState.GraphicsQuality = GraphicsQuality;

	// Build frustum planes
	BuildFrustumPlanes(CameraPos, CameraRot, FOV, AspectRatio);

	// Clear previous results
	VisibleSamples.Reset();

	const TArray<FFragmentVisibilityData>& AllFragments = Registry->GetAllFragments();
	const int32 TotalFragments = AllFragments.Num();

	// Calculate range for frame spreading (if enabled)
	int32 StartIndex = 0;
	int32 EndIndex = TotalFragments;

	if (bEnableFrameSpreading && FrameSpreadCount > 1)
	{
		const int32 ChunkSize = (TotalFragments + FrameSpreadCount - 1) / FrameSpreadCount;
		StartIndex = CurrentFrameIndex * ChunkSize;
		EndIndex = FMath::Min(StartIndex + ChunkSize, TotalFragments);

		CurrentFrameIndex = (CurrentFrameIndex + 1) % FrameSpreadCount;
	}

	// Pre-compute quality-adjusted thresholds
	const float SmallScreen = SmallScreenSize * GraphicsQuality;
	const float MediumScreen = MediumScreenSize * GraphicsQuality;

	int32 FrustumCulled = 0;
	int32 LodCulled = 0;

	// === MAIN VISIBILITY LOOP ===
	// This is the core per-sample evaluation that tests EACH fragment individually
	for (int32 i = StartIndex; i < EndIndex; ++i)
	{
		const FFragmentVisibilityData& Sample = AllFragments[i];

		// === LOD MODE OVERRIDE ===
		if (LodMode == EFragmentLodMode::AllVisible)
		{
			// Skip frustum test, show everything
			FFragmentVisibilityResult Result;
			Result.LocalId = Sample.LocalId;
			Result.LodLevel = EFragmentLod::Geometry;
			Result.ScreenSize = ViewportHeight; // Max screen size
			Result.Distance = 0.0f;
			Result.MaterialIndex = Sample.MaterialIndex;
			Result.bIsSmallObject = Sample.bIsSmallObject;
			Result.BoundsCenter = Sample.WorldBounds.GetCenter();
			VisibleSamples.Add(Result);
			continue;
		}

		// === FRUSTUM TEST (per-fragment, not per-tile!) ===
		if (!IsInFrustum(Sample.WorldBounds))
		{
			FrustumCulled++;
			continue;
		}

		// === DISTANCE & SCREEN SIZE CALCULATION ===
		const float Distance = GetDistanceToBox(Sample.WorldBounds);
		const float ScreenSize = CalculateScreenSize(Sample.MaxDimension, Distance);

		// === LOD EVALUATION ===
		const EFragmentLod LodLevel = EvaluateLod(Sample, ScreenSize);

		if (LodLevel == EFragmentLod::Invisible)
		{
			LodCulled++;
			continue;
		}

		// === ADD TO VISIBLE SAMPLES ===
		FFragmentVisibilityResult Result;
		Result.LocalId = Sample.LocalId;
		Result.LodLevel = LodLevel;
		Result.ScreenSize = ScreenSize;
		Result.Distance = Distance;
		Result.MaterialIndex = Sample.MaterialIndex;
		Result.bIsSmallObject = Sample.bIsSmallObject;
		Result.BoundsCenter = Sample.WorldBounds.GetCenter();

		VisibleSamples.Add(Result);
	}

	// Update last camera state
	LastCameraPosition = CameraPos;
	LastCameraRotation = CameraRot;

	UE_LOG(LogPerSampleVisibility, Verbose,
	       TEXT("Visibility update: %d/%d visible, %d frustum culled, %d lod culled"),
	       VisibleSamples.Num(), EndIndex - StartIndex, FrustumCulled, LodCulled);
}

bool UPerSampleVisibilityController::NeedsUpdate(const FVector& NewPosition, const FRotator& NewRotation) const
{
	// Check position change
	const float DistanceMoved = FVector::Dist(LastCameraPosition, NewPosition);
	if (DistanceMoved >= MinCameraMovement)
	{
		return true;
	}

	// Check rotation change
	const FRotator RotationDelta = NewRotation - LastCameraRotation;
	const float RotationChange = FMath::Max3(
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Pitch)),
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Yaw)),
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Roll))
	);

	return RotationChange >= MinCameraRotation;
}

int32 UPerSampleVisibilityController::GetCountByLod(EFragmentLod LodLevel) const
{
	int32 Count = 0;
	for (const FFragmentVisibilityResult& Result : VisibleSamples)
	{
		if (Result.LodLevel == LodLevel)
		{
			Count++;
		}
	}
	return Count;
}

EFragmentLod UPerSampleVisibilityController::EvaluateLod(const FFragmentVisibilityData& Data, float ScreenSize) const
{
	// Direct port of engine_fragment's fetchLodLevel() logic
	// (virtual-tiles-controller.ts lines 775-835)

	// Apply graphics quality to thresholds
	const float SmallScreen = SmallScreenSize * GraphicsQuality;
	const float MediumScreen = MediumScreenSize * GraphicsQuality;
	const float LargeScreen = LargeScreenSize * GraphicsQuality;

	// Screen size classification
	const bool bIsSmallInScreen = ScreenSize < SmallScreen;      // < 2px
	const bool bIsMediumInScreen = ScreenSize < MediumScreen;    // < 4px
	const bool bIsLargeInScreen = ScreenSize < LargeScreen;      // < 16px

	// Combined conditions (matching engine_fragment)
	const bool bSmallAndFar = Data.bIsSmallObject && bIsMediumInScreen;
	const bool bLargeAndVeryFar = !Data.bIsSmallObject && bIsSmallInScreen;
	const bool bSmallAndClose = Data.bIsSmallObject && bIsLargeInScreen;
	const bool bLargeAndFar = !Data.bIsSmallObject && bIsMediumInScreen;

	// Very far away -> completely hide
	if (bSmallAndFar || bLargeAndVeryFar)
	{
		return EFragmentLod::Invisible;
	}

	// All geometry mode -> show everything that passed frustum culling
	if (LodMode == EFragmentLodMode::AllGeometry)
	{
		return EFragmentLod::Geometry;
	}

	// Far away but still visible -> use wireframe/simplified LOD
	if (bSmallAndClose || bLargeAndFar)
	{
		return EFragmentLod::Wires;
	}

	// Default: full geometry detail
	return EFragmentLod::Geometry;
}

float UPerSampleVisibilityController::GetViewDimension(float Distance) const
{
	// Port of engine_fragment's getViewDimension()
	if (ViewState.OrthogonalDimension > 0.0f)
	{
		return ViewState.OrthogonalDimension;
	}

	// Cache tan(fov/2)
	if (ViewState.FOV != CachedFOV)
	{
		const float RadFactor = PI / 180.0f;
		CachedTanHalfFOV = FMath::Tan(ViewState.FOV * 0.5f * RadFactor);
		CachedFOV = ViewState.FOV;
	}

	return Distance * CachedTanHalfFOV;
}

float UPerSampleVisibilityController::CalculateScreenSize(float Dimension, float Distance) const
{
	// Port of engine_fragment's screenSize()

	// Avoid division by zero for very close objects
	if (Distance < 1.0f)
	{
		Distance = 1.0f;
	}

	const float ViewDimension = GetViewDimension(Distance);

	// Avoid division by zero
	if (ViewDimension < KINDA_SMALL_NUMBER)
	{
		return ViewState.ViewportHeight; // Object fills screen
	}

	const float ScreenDimension = Dimension / ViewDimension;
	return ScreenDimension * ViewState.ViewportHeight;
}

float UPerSampleVisibilityController::GetDistanceToBox(const FBox& Box) const
{
	// Port of Three.js Box3.distanceToPoint()
	// Returns 0 if point is inside box

	const FVector ClosestPoint(
		FMath::Clamp(ViewState.CameraPosition.X, Box.Min.X, Box.Max.X),
		FMath::Clamp(ViewState.CameraPosition.Y, Box.Min.Y, Box.Max.Y),
		FMath::Clamp(ViewState.CameraPosition.Z, Box.Min.Z, Box.Max.Z)
	);

	return FVector::Dist(ViewState.CameraPosition, ClosestPoint);
}

bool UPerSampleVisibilityController::BoxIntersectsPlane(const FBox& Box, const FPlane& Plane)
{
	// Port of engine_fragment's PlanesUtils.collides()
	// Optimized AABB-plane test using p-vertex

	const FVector Normal(Plane.X, Plane.Y, Plane.Z);

	// Find the "positive vertex" (p-vertex) - corner farthest in normal direction
	FVector PVertex;
	PVertex.X = (Normal.X >= 0.0f) ? Box.Max.X : Box.Min.X;
	PVertex.Y = (Normal.Y >= 0.0f) ? Box.Max.Y : Box.Min.Y;
	PVertex.Z = (Normal.Z >= 0.0f) ? Box.Max.Z : Box.Min.Z;

	// Distance from p-vertex to plane
	const float Distance = Plane.PlaneDot(PVertex);

	// If p-vertex is behind plane, entire box is outside frustum
	return Distance >= 0.0f;
}

bool UPerSampleVisibilityController::IsInFrustum(const FBox& Box) const
{
	// Port of engine_fragment's frustumCollide()
	// Box is inside if on positive side of ALL planes

	for (const FPlane& Plane : ViewState.FrustumPlanes)
	{
		if (!BoxIntersectsPlane(Box, Plane))
		{
			return false;
		}
	}

	return true;
}

void UPerSampleVisibilityController::BuildFrustumPlanes(const FVector& CameraLocation, const FRotator& CameraRotation,
                                                         float FOV, float AspectRatio)
{
	// Build view matrix
	const FMatrix ViewMatrix = FInverseRotationMatrix(CameraRotation) * FTranslationMatrix(-CameraLocation);

	// Build perspective projection matrix
	const float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	const float NearPlane = 10.0f;      // 10cm
	const float FarPlane = 10000000.0f; // 100km

	FMatrix ProjectionMatrix = FPerspectiveMatrix(
		HalfFOVRadians,
		AspectRatio,
		1.0f,
		NearPlane,
		FarPlane
	);

	// Combine into view-projection matrix
	const FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;

	// Extract 6 frustum planes using Gribb/Hartmann method
	ViewState.FrustumPlanes.Empty(6);

	const FVector4 Row0(ViewProjectionMatrix.M[0][0], ViewProjectionMatrix.M[0][1], ViewProjectionMatrix.M[0][2], ViewProjectionMatrix.M[0][3]);
	const FVector4 Row1(ViewProjectionMatrix.M[1][0], ViewProjectionMatrix.M[1][1], ViewProjectionMatrix.M[1][2], ViewProjectionMatrix.M[1][3]);
	const FVector4 Row2(ViewProjectionMatrix.M[2][0], ViewProjectionMatrix.M[2][1], ViewProjectionMatrix.M[2][2], ViewProjectionMatrix.M[2][3]);
	const FVector4 Row3(ViewProjectionMatrix.M[3][0], ViewProjectionMatrix.M[3][1], ViewProjectionMatrix.M[3][2], ViewProjectionMatrix.M[3][3]);

	// Left plane: Row3 + Row0
	{
		FVector4 P = Row3 + Row0;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}

	// Right plane: Row3 - Row0
	{
		FVector4 P = Row3 - Row0;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}

	// Bottom plane: Row3 + Row1
	{
		FVector4 P = Row3 + Row1;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}

	// Top plane: Row3 - Row1
	{
		FVector4 P = Row3 - Row1;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}

	// Near plane: Row3 + Row2
	{
		FVector4 P = Row3 + Row2;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}

	// Far plane: Row3 - Row2
	{
		FVector4 P = Row3 - Row2;
		float Length = FVector(P.X, P.Y, P.Z).Size();
		if (Length > KINDA_SMALL_NUMBER)
		{
			P /= Length;
			ViewState.FrustumPlanes.Add(FPlane(P.X, P.Y, P.Z, P.W));
		}
	}
}
