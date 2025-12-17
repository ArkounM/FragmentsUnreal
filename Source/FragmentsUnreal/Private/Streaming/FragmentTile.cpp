#include "Streaming/FragmentTile.h"
#include "Math/UnrealMathUtility.h"

float FFragmentTile::CalculateScreenSpaceError(const FVector& CameraLocation, float VerticalFOV, float ViewportHeight) const
{
	if (GeometricError <= 0.0f)
	{
		return 0.0f;
	}

	// Get distance from camera to tile bounding box
	const FVector TileCenter = BoundingBox.GetCenter();
	const float Distance = FMath::Max(1.0f, FVector::Dist(CameraLocation, TileCenter));

	// Screen space error formula (from 3D Tiles spec):
	// SSE = (GeometricError * ViewportHeight) / (Distance * 2 * tan(FOV/2))
	const float HalfFOVRadians = FMath::DegreesToRadians(VerticalFOV * 0.5f);
	const float Denominator = Distance * 2.0f * FMath::Tan(HalfFOVRadians);

	if (Denominator <= 0.0f)
	{
		return 0.0f;
	}

	const float SSE = (GeometricError * ViewportHeight) / Denominator;
	return SSE;
}

bool FFragmentTile::IntersectsFrustum(const FConvexVolume& Frustum) const
{
	// Check if tile bounding box intersects camera frustum
	return Frustum.IntersectBox(BoundingBox.GetCenter(), BoundingBox.GetExtent());
}
