#include "Spatial/FragmentVisibility.h"
#include "Spatial/FragmentTile.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentVisibility, Log, All);

UFragmentVisibility::UFragmentVisibility()
{
}

void UFragmentVisibility::Initialize(const FFragmentVisibilityParams& InParams)
{
	Params = InParams;
	bShowAllVisible = false;

	// Reset cached values
	CachedTanHalfFOV = 0.0f;
	CachedFOV = 0.0f;
	LastCameraPosition = FVector::ZeroVector;
	LastCameraRotation = FRotator::ZeroRotator;
}

void UFragmentVisibility::UpdateView(const FVector& CameraLocation, const FRotator& CameraRotation,
                                      float FOV, float AspectRatio, float ViewportHeight)
{
	// Update view state
	ViewState.CameraPosition = CameraLocation;
	ViewState.CameraForward = CameraRotation.Vector();
	ViewState.FOV = FOV;
	ViewState.ViewportHeight = ViewportHeight;
	ViewState.ViewportWidth = ViewportHeight * AspectRatio;

	// Build frustum planes for culling
	BuildFrustumPlanes(CameraLocation, CameraRotation, FOV, AspectRatio);

	// Update last camera state for change detection
	LastCameraPosition = CameraLocation;
	LastCameraRotation = CameraRotation;

	UE_LOG(LogFragmentVisibility, VeryVerbose, TEXT("View updated: Pos=%s, FOV=%.1f, Viewport=%.0fx%.0f"),
	       *CameraLocation.ToString(), FOV, ViewState.ViewportWidth, ViewportHeight);
}

float UFragmentVisibility::GetPerspTrueDim(float FOV, float Distance) const
{
	// Port of engine_fragment getPerspTrueDim():
	// const radFactor = Math.PI / 180;
	// const tan = Math.tan(fov * 0.5 * radFactor);
	// return distance * tan;

	const float RadFactor = PI / 180.0f;
	const float Tan = FMath::Tan(FOV * 0.5f * RadFactor);
	return Distance * Tan;
}

float UFragmentVisibility::GetViewDimension(float Distance) const
{
	// Port of engine_fragment getViewDimension():
	// if (orthogonalDimension) return orthogonalDimension;
	// return distance * tan(fov/2);

	if (ViewState.OrthogonalDimension > 0.0f)
	{
		return ViewState.OrthogonalDimension;
	}

	// Cache tan(fov/2) to avoid recalculation
	if (ViewState.FOV != CachedFOV)
	{
		CachedTanHalfFOV = GetPerspTrueDim(ViewState.FOV, 1.0f);
		CachedFOV = ViewState.FOV;
	}

	return Distance * CachedTanHalfFOV;
}

float UFragmentVisibility::CalculateScreenSize(float Dimension, float Distance) const
{
	// Port of engine_fragment screenSize():
	// const viewDimension = this.getViewDimension(distance);
	// const screenDimension = dimension / viewDimension;
	// return screenDimension * this._virtualView.viewSize;

	// Avoid division by zero for very close objects
	if (Distance < 1.0f)
	{
		Distance = 1.0f;
	}

	const float ViewDimension = GetViewDimension(Distance);

	// Avoid division by zero
	if (ViewDimension < KINDA_SMALL_NUMBER)
	{
		return ViewState.ViewportHeight;  // Object fills screen
	}

	const float ScreenDimension = Dimension / ViewDimension;
	return ScreenDimension * ViewState.ViewportHeight;
}

float UFragmentVisibility::GetDistanceToBox(const FBox& Box) const
{
	// Port of Three.js Box3.distanceToPoint()
	// Returns 0 if point is inside box, otherwise shortest distance to box surface

	// Clamp camera position to box bounds to find closest point
	const FVector ClosestPoint(
		FMath::Clamp(ViewState.CameraPosition.X, Box.Min.X, Box.Max.X),
		FMath::Clamp(ViewState.CameraPosition.Y, Box.Min.Y, Box.Max.Y),
		FMath::Clamp(ViewState.CameraPosition.Z, Box.Min.Z, Box.Max.Z)
	);

	// Distance from camera to closest point
	return FVector::Dist(ViewState.CameraPosition, ClosestPoint);
}

bool UFragmentVisibility::HasViewChanged(const FVector& NewPosition, const FRotator& NewRotation) const
{
	// Check position change
	const float DistanceMoved = FVector::Dist(LastCameraPosition, NewPosition);
	if (DistanceMoved >= Params.UpdateViewPosition)
	{
		return true;
	}

	// Check rotation change (using max of pitch/yaw/roll delta)
	const FRotator RotationDelta = NewRotation - LastCameraRotation;
	const float RotationChange = FMath::Max3(
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Pitch)),
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Yaw)),
		FMath::Abs(FRotator::NormalizeAxis(RotationDelta.Roll))
	);

	return RotationChange >= Params.UpdateViewOrientation;
}

void UFragmentVisibility::BuildFrustumPlanes(const FVector& CameraLocation, const FRotator& CameraRotation,
                                              float FOV, float AspectRatio)
{
	// Build view matrix
	const FMatrix ViewMatrix = FInverseRotationMatrix(CameraRotation) * FTranslationMatrix(-CameraLocation);

	// Build perspective projection matrix
	const float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	const float NearPlane = 10.0f;     // 10cm (very close)
	const float FarPlane = 10000000.0f; // 100km (very far)

	// Use standard (non-reversed-Z) perspective matrix for frustum extraction
	// This ensures plane normals point inward correctly
	FMatrix ProjectionMatrix = FPerspectiveMatrix(
		HalfFOVRadians,
		AspectRatio,
		1.0f,
		NearPlane,
		FarPlane
	);

	// Combine into view-projection matrix
	const FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;

	// Extract 6 frustum planes from the view-projection matrix
	// Using the Gribb/Hartmann method
	ViewState.FrustumPlanes.Empty(6);

	// Row vectors of the matrix
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

	UE_LOG(LogFragmentVisibility, VeryVerbose, TEXT("Built %d frustum planes"), ViewState.FrustumPlanes.Num());
}

bool UFragmentVisibility::BoxIntersectsPlane(const FBox& Box, const FPlane& Plane)
{
	// Port of engine_fragment's PlanesUtils.collides() / getPointDistance()
	// This is the optimized AABB-plane intersection test.
	//
	// For each plane, we find the corner of the box that is CLOSEST to the plane
	// (in the direction of the plane normal). If this corner is behind the plane,
	// the entire box is culled.
	//
	// The "closest" corner depends on the sign of each component of the plane normal:
	// - If normal.x >= 0, use Box.Max.X (farthest in +X direction)
	// - If normal.x < 0, use Box.Min.X (farthest in -X direction)
	// etc.

	const FVector Normal(Plane.X, Plane.Y, Plane.Z);

	// Find the "positive vertex" (p-vertex) - the corner farthest in the direction of the normal
	FVector PVertex;
	PVertex.X = (Normal.X >= 0.0f) ? Box.Max.X : Box.Min.X;
	PVertex.Y = (Normal.Y >= 0.0f) ? Box.Max.Y : Box.Min.Y;
	PVertex.Z = (Normal.Z >= 0.0f) ? Box.Max.Z : Box.Min.Z;

	// Distance from p-vertex to plane (positive = in front of plane)
	const float Distance = Plane.PlaneDot(PVertex);

	// If p-vertex is behind plane, entire box is outside frustum
	return Distance >= 0.0f;
}

bool UFragmentVisibility::IsInFrustum(const FBox& Box) const
{
	// Port of engine_fragment's frustumCollide() / PlanesUtils.collides()
	// A box is inside the frustum if it's on the positive side of ALL planes

	for (const FPlane& Plane : ViewState.FrustumPlanes)
	{
		if (!BoxIntersectsPlane(Box, Plane))
		{
			return false;  // Box is completely outside this plane
		}
	}

	return true;  // Box intersects or is inside frustum
}

EFragmentLod UFragmentVisibility::FetchLodLevel(const UFragmentTile* Tile) const
{
	// Simplified visibility check - just frustum culling + minimum screen size

	if (!Tile)
	{
		return EFragmentLod::Invisible;
	}

	// Debug mode: show all fragments regardless of frustum
	if (bShowAllVisible)
	{
		return EFragmentLod::Visible;
	}

	// === FRUSTUM CULLING ===
	if (!IsInFrustum(Tile->Bounds))
	{
		UE_LOG(LogFragmentVisibility, VeryVerbose, TEXT("Tile culled: outside frustum"));
		return EFragmentLod::Invisible;
	}

	// === SCREEN SIZE CHECK (optional culling of tiny objects) ===
	const FVector Extent = Tile->Bounds.GetExtent();
	const float Dimension = FMath::Max3(Extent.X, Extent.Y, Extent.Z) * 2.0f;
	const float Distance = GetDistanceToBox(Tile->Bounds);
	const float ScreenSize = CalculateScreenSize(Dimension, Distance);

	// Apply graphics quality multiplier
	const float MinScreen = Params.MinScreenSize * ViewState.GraphicsQuality;

	// Cull if too small on screen
	if (ScreenSize < MinScreen)
	{
		UE_LOG(LogFragmentVisibility, VeryVerbose, TEXT("Tile culled: screen size %.2f < %.2f"), ScreenSize, MinScreen);
		return EFragmentLod::Invisible;
	}

	// Visible - render full geometry
	UE_LOG(LogFragmentVisibility, VeryVerbose, TEXT("Tile visible: screen size %.2f px"), ScreenSize);
	return EFragmentLod::Visible;
}
