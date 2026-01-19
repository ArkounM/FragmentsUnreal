#pragma once

#include "CoreMinimal.h"
#include "FragmentVisibility.generated.h"

// Forward declarations
class UFragmentTile;

/**
 * LOD levels matching engine_fragment's CurrentLod enum.
 * Used to determine rendering detail level based on screen size.
 */
UENUM(BlueprintType)
enum class EFragmentLod : uint8
{
	/** Full geometry detail */
	Geometry = 0  UMETA(DisplayName = "Geometry"),

	/** Simplified wireframe/lines representation */
	Wires = 1     UMETA(DisplayName = "Wires"),

	/** Not rendered at all */
	Invisible = 2 UMETA(DisplayName = "Invisible")
};

/**
 * LOD mode matching engine_fragment's LodMode enum.
 */
UENUM(BlueprintType)
enum class EFragmentLodMode : uint8
{
	/** Hides invisible items, displays far away items as LOD geometry, close items as full geometry */
	Default = 0   UMETA(DisplayName = "Default"),

	/** Displays all items as full geometry (no culling) */
	AllVisible = 1 UMETA(DisplayName = "All Visible"),

	/** Hides invisible items, displays the rest as full geometry */
	AllGeometry = 2 UMETA(DisplayName = "All Geometry")
};

/**
 * Camera view state for visibility calculations.
 * Matches engine_fragment's VirtualView structure.
 */
USTRUCT(BlueprintType)
struct FFragmentViewState
{
	GENERATED_BODY()

	/** Camera world position */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	FVector CameraPosition = FVector::ZeroVector;

	/** Camera forward direction (normalized) */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	FVector CameraForward = FVector::ForwardVector;

	/** Field of view in degrees */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	float FOV = 90.0f;

	/** Viewport height in pixels */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	float ViewportHeight = 1080.0f;

	/** Viewport width in pixels */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	float ViewportWidth = 1920.0f;

	/** Graphics quality multiplier (0.5 = half, 1.0 = normal, 2.0 = double) */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	float GraphicsQuality = 1.0f;

	/** Orthographic projection dimension (0 = perspective) */
	UPROPERTY(BlueprintReadWrite, Category = "View")
	float OrthogonalDimension = 0.0f;

	/** Frustum planes in world space (6 planes: near, far, left, right, top, bottom) */
	TArray<FPlane> FrustumPlanes;
};

/**
 * Screen size thresholds matching engine_fragment.
 * Values are in pixels.
 */
USTRUCT(BlueprintType)
struct FFragmentVisibilityParams
{
	GENERATED_BODY()

	/** Objects smaller than this world size are considered "small" (in cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds")
	float SmallObjectSize = 200.0f;  // 2 meters in engine_fragment units * 100

	/** Screen size threshold for hiding tiny objects (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds")
	float SmallScreenSize = 2.0f;

	/** Screen size threshold for wireframe/simplified LOD (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds")
	float MediumScreenSize = 4.0f;

	/** Screen size threshold for full geometry (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds")
	float LargeScreenSize = 16.0f;

	/** Minimum camera movement to trigger update (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Update")
	float UpdateViewPosition = 25600.0f;  // 256 meters * 100

	/** Minimum camera rotation to trigger update (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Update")
	float UpdateViewOrientation = 8.0f;

	/** Frame time budget for updates (ms) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Update")
	float UpdateTime = 16.0f;
};

/**
 * Fragment visibility manager - direct port of engine_fragment's visibility system.
 *
 * This class implements the screen-size based LOD and frustum culling system
 * from engine_fragment (virtual-tiles-controller.ts).
 *
 * Key difference from Cesium SSE: Uses actual tile dimension for screen size
 * calculation rather than a separate "geometric error" value.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentVisibility : public UObject
{
	GENERATED_BODY()

public:
	UFragmentVisibility();

	/**
	 * Initialize with visibility parameters.
	 */
	void Initialize(const FFragmentVisibilityParams& InParams);

	/**
	 * Update view state from camera.
	 * @param CameraLocation Camera world position
	 * @param CameraRotation Camera rotation
	 * @param FOV Field of view in degrees
	 * @param AspectRatio Viewport aspect ratio (width/height)
	 * @param ViewportHeight Viewport height in pixels
	 */
	void UpdateView(const FVector& CameraLocation, const FRotator& CameraRotation,
	                float FOV, float AspectRatio, float ViewportHeight);

	/**
	 * Calculate LOD level for a tile.
	 * Direct port of engine_fragment's fetchLodLevel() function.
	 *
	 * @param Tile Tile to evaluate
	 * @return LOD level to use for this tile
	 */
	EFragmentLod FetchLodLevel(const UFragmentTile* Tile) const;

	/**
	 * Check if a box intersects the view frustum.
	 * @param Box World-space bounding box
	 * @return true if box is (partially) inside frustum
	 */
	bool IsInFrustum(const FBox& Box) const;

	/**
	 * Calculate screen size in pixels for an object.
	 * Direct port of engine_fragment's screenSize() function.
	 *
	 * @param Dimension Object world-space dimension (max extent)
	 * @param Distance Distance from camera to closest point on object
	 * @return Projected screen size in pixels
	 */
	float CalculateScreenSize(float Dimension, float Distance) const;

	/**
	 * Get distance from camera to closest point on a box.
	 * Matches Three.js Box3.distanceToPoint() behavior.
	 *
	 * @param Box World-space bounding box
	 * @return Distance in world units (0 if camera is inside box)
	 */
	float GetDistanceToBox(const FBox& Box) const;

	/**
	 * Check if camera has moved significantly since last update.
	 * @param NewPosition New camera position
	 * @param NewRotation New camera rotation
	 * @return true if camera moved enough to warrant visibility recalculation
	 */
	bool HasViewChanged(const FVector& NewPosition, const FRotator& NewRotation) const;

	// --- Configuration ---

	/** Current LOD mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility")
	EFragmentLodMode LodMode = EFragmentLodMode::Default;

	/** Visibility parameters (screen size thresholds, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility")
	FFragmentVisibilityParams Params;

	/** Current view state */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	FFragmentViewState ViewState;

private:
	/**
	 * Calculate view dimension for perspective projection.
	 * Port of engine_fragment's getViewDimension().
	 *
	 * @param Distance Distance from camera
	 * @return View dimension at that distance
	 */
	float GetViewDimension(float Distance) const;

	/**
	 * Calculate true view dimension from FOV.
	 * Port of engine_fragment's getPerspTrueDim().
	 *
	 * @param FOV Field of view in degrees
	 * @param Distance Distance (typically 1.0 for normalization)
	 * @return View dimension factor
	 */
	float GetPerspTrueDim(float FOV, float Distance) const;

	/**
	 * Build frustum planes from view matrix.
	 */
	void BuildFrustumPlanes(const FVector& CameraLocation, const FRotator& CameraRotation,
	                         float FOV, float AspectRatio);

	/**
	 * Test box against frustum plane.
	 * Port of engine_fragment's PlanesUtils.collides().
	 *
	 * Uses optimized AABB-plane test that checks the corner
	 * of the box closest to the plane normal direction.
	 *
	 * @param Box World-space bounding box
	 * @param Plane Frustum plane
	 * @return true if box is on positive side of plane (inside frustum)
	 */
	static bool BoxIntersectsPlane(const FBox& Box, const FPlane& Plane);

	// Cached tan(fov/2) to avoid recalculation
	mutable float CachedTanHalfFOV = 0.0f;
	mutable float CachedFOV = 0.0f;

	// Last camera state for change detection
	FVector LastCameraPosition = FVector::ZeroVector;
	FRotator LastCameraRotation = FRotator::ZeroRotator;
};
