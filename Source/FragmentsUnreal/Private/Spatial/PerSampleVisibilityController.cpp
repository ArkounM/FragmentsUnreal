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

	// Pre-compute quality-adjusted threshold
	const float MinScreen = MinScreenSize * GraphicsQuality;

	// === MAIN VISIBILITY LOOP ===
	// This is the core per-sample evaluation that tests EACH fragment individually
	for (int32 i = StartIndex; i < EndIndex; ++i)
	{
		const FFragmentVisibilityData& Sample = AllFragments[i];

		// === DEBUG MODE: SHOW ALL ===
		if (bShowAllVisible)
		{
			// Skip frustum test, show everything at full detail
			FFragmentVisibilityResult Result;
			Result.LocalId = Sample.LocalId;
			Result.LodLevel = EFragmentLod::FullDetail;
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
			continue;
		}

		// === DISTANCE AND SCREEN SIZE CALCULATION ===
		const float Distance = GetDistanceToBox(Sample.WorldBounds);
		const float ScreenSize = CalculateScreenSize(Sample.MaxDimension, Distance);

		// === DETERMINE LOD LEVEL ===
		const EFragmentLod LodLevel = DetermineLodLevel(ScreenSize);

		// Skip invisible fragments
		if (LodLevel == EFragmentLod::Invisible)
		{
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

EFragmentLod UPerSampleVisibilityController::DetermineLodLevel(float ScreenSize) const
{
	if (!bEnableLodSystem)
	{
		// Binary mode: visible or invisible only
		return (ScreenSize >= MinScreenSize * GraphicsQuality)
			? EFragmentLod::FullDetail
			: EFragmentLod::Invisible;
	}

	const float QualityMin = MinScreenSize * GraphicsQuality;
	const float QualityBoundingBox = BoundingBoxThreshold * GraphicsQuality;
	const float QualitySimplified = SimplifiedThreshold * GraphicsQuality;

	if (ScreenSize < QualityMin)
	{
		return EFragmentLod::Invisible;
	}
	else if (ScreenSize < QualityBoundingBox)
	{
		return EFragmentLod::BoundingBox;
	}
	else if (ScreenSize < QualitySimplified)
	{
		return EFragmentLod::Simplified;
	}
	else
	{
		return EFragmentLod::FullDetail;
	}
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
	//
	// Match engine_fragment: when camera is inside/very close to object,
	// return maximum screen size (object fills entire screen).
	// In engine_fragment, Distance=0 causes division by zero → INFINITY screen size.
	// INFINITY < any_threshold = FALSE → always visible.
	// We emulate this by returning a guaranteed-large value.
	if (Distance < 1.0f)
	{
		// Camera inside or touching bounds - object fills screen
		return ViewState.ViewportHeight * 10.0f;  // Guaranteed to pass any threshold
	}

	const float ViewDimension = GetViewDimension(Distance);

	// Avoid division by zero
	if (ViewDimension < KINDA_SMALL_NUMBER)
	{
		return ViewState.ViewportHeight * 10.0f;  // Object fills screen
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
	// FIX: Corrected frustum plane test
	// The Gribb/Hartmann extraction produces planes with normals pointing OUTWARD from frustum.
	// For a box to be "inside" the frustum, it must be on the NEGATIVE side of all planes.
	//
	// We use the "n-vertex" (negative vertex) - the corner CLOSEST to the plane in normal direction.
	// If the n-vertex is in front of the plane (positive side), the entire box is outside.

	const FVector Normal(Plane.X, Plane.Y, Plane.Z);

	// Find the "negative vertex" (n-vertex) - corner closest in normal direction
	// This is the OPPOSITE of p-vertex selection
	FVector NVertex;
	NVertex.X = (Normal.X >= 0.0f) ? Box.Min.X : Box.Max.X;
	NVertex.Y = (Normal.Y >= 0.0f) ? Box.Min.Y : Box.Max.Y;
	NVertex.Z = (Normal.Z >= 0.0f) ? Box.Min.Z : Box.Max.Z;

	// Distance from n-vertex to plane
	const float Distance = Plane.PlaneDot(NVertex);

	// If n-vertex is in front of plane (positive), entire box is outside frustum
	// Box is inside/intersecting if n-vertex is behind or on plane (negative or zero)
	return Distance <= 0.0f;
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

	// Extract 5 frustum planes using Gribb/Hartmann method
	// NOTE: We skip the NEAR plane to prevent close objects from being culled
	// This matches engine_fragment behavior where close objects get large screen sizes
	// and should remain visible, not be clipped by near plane
	ViewState.FrustumPlanes.Empty(5);

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

	// SKIP Near plane (Row3 + Row2) - this was causing close objects to be culled
	// Near plane culling is handled by the renderer, not visibility system

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

	UE_LOG(LogPerSampleVisibility, VeryVerbose, TEXT("Built %d frustum planes (near plane excluded)"),
	       ViewState.FrustumPlanes.Num());
}
