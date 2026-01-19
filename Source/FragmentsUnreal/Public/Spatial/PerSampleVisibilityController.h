#pragma once

#include "CoreMinimal.h"
#include "Spatial/FragmentVisibility.h"
#include "Spatial/FragmentRegistry.h"
#include "PerSampleVisibilityController.generated.h"

// Forward declarations
class UFragmentRegistry;

/**
 * Result of per-sample visibility evaluation.
 * Contains LOD level and screen metrics for a single visible fragment.
 */
USTRUCT(BlueprintType)
struct FFragmentVisibilityResult
{
	GENERATED_BODY()

	/** Fragment local ID */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	int32 LocalId = -1;

	/** Determined LOD level for this fragment */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	EFragmentLod LodLevel = EFragmentLod::Invisible;

	/** Screen size in pixels */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	float ScreenSize = 0.0f;

	/** Distance from camera to closest point on bounds */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	float Distance = 0.0f;

	/** Material index (for dynamic tile grouping) */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	int32 MaterialIndex = 0;

	/** Whether this is a small object */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	bool bIsSmallObject = false;

	/** Bounding box center (for CRC tile grouping) */
	UPROPERTY(BlueprintReadOnly, Category = "Visibility")
	FVector BoundsCenter = FVector::ZeroVector;

	FFragmentVisibilityResult() = default;

	FFragmentVisibilityResult(int32 InLocalId, EFragmentLod InLod, float InScreenSize, float InDistance)
		: LocalId(InLocalId)
		, LodLevel(InLod)
		, ScreenSize(InScreenSize)
		, Distance(InDistance)
	{
	}
};

/**
 * Per-Sample Visibility Controller - engine_fragment style visibility evaluation.
 *
 * This controller evaluates visibility for EACH fragment individually, rather than
 * testing entire tiles. This prevents the bug where camera inside a tile causes
 * all fragments in that tile to be culled.
 *
 * Algorithm (per frame):
 * 1. Iterate all fragments in registry
 * 2. Test EACH fragment's bounds against frustum
 * 3. Calculate screen size for fragments in frustum
 * 4. Determine LOD level (Geometry, Wires, Invisible)
 * 5. Output visible samples with LOD info for tile grouping
 *
 * Performance notes:
 * - 10,000 fragments × 64 bytes = 640 KB (cache-friendly)
 * - Early frustum rejection before full LOD calculation
 * - Optional: Frame spreading (process 1/4 per frame)
 */
UCLASS()
class FRAGMENTSUNREAL_API UPerSampleVisibilityController : public UObject
{
	GENERATED_BODY()

public:
	UPerSampleVisibilityController();

	/**
	 * Initialize with fragment registry.
	 * @param InRegistry Registry containing all fragment visibility data
	 */
	void Initialize(UFragmentRegistry* InRegistry);

	/**
	 * Update visibility for all samples based on camera state.
	 * This is the main per-frame update that evaluates each fragment.
	 *
	 * @param CameraPos Camera world position
	 * @param CameraRot Camera rotation
	 * @param FOV Field of view in degrees
	 * @param AspectRatio Viewport width / height
	 * @param ViewportHeight Viewport height in pixels
	 */
	void UpdateVisibility(const FVector& CameraPos, const FRotator& CameraRot,
	                      float FOV, float AspectRatio, float ViewportHeight);

	/**
	 * Get current visible samples (const reference).
	 * @return Array of visibility results for fragments that passed culling
	 */
	const TArray<FFragmentVisibilityResult>& GetVisibleSamples() const { return VisibleSamples; }

	/**
	 * Get count of currently visible samples.
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	int32 GetVisibleCount() const { return VisibleSamples.Num(); }

	/**
	 * Get count of samples by LOD level.
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Visibility")
	int32 GetCountByLod(EFragmentLod LodLevel) const;

	/**
	 * Check if visibility needs update based on camera movement.
	 * @param NewPosition New camera position
	 * @param NewRotation New camera rotation
	 * @return true if camera moved enough to warrant re-evaluation
	 */
	bool NeedsUpdate(const FVector& NewPosition, const FRotator& NewRotation) const;

	// --- Configuration ---

	/** Show all fragments regardless of frustum (debug mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility")
	bool bShowAllVisible = false;

	/** Graphics quality multiplier (affects screen size thresholds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility", meta = (ClampMin = "0.5", ClampMax = "2.0"))
	float GraphicsQuality = 1.0f;

	/** Enable frame spreading to distribute visibility updates */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility")
	bool bEnableFrameSpreading = false;

	/** Number of frames to spread visibility updates across */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility", meta = (ClampMin = "1", ClampMax = "8"))
	int32 FrameSpreadCount = 4;

	// --- Screen Size Thresholds ---

	/** Minimum screen size to show a fragment (pixels) - fragments smaller than this are culled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility|Thresholds")
	float MinScreenSize = 2.0f;

	/** Minimum camera movement to trigger update (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility|Update")
	float MinCameraMovement = 2500.0f;

	/** Minimum camera rotation to trigger update (degrees) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visibility|Update")
	float MinCameraRotation = 5.0f;

private:
	/** Reference to fragment registry */
	UPROPERTY()
	UFragmentRegistry* Registry = nullptr;

	/** Current visible samples (output of UpdateVisibility) */
	TArray<FFragmentVisibilityResult> VisibleSamples;

	/** Current frame index for frame spreading */
	int32 CurrentFrameIndex = 0;

	/** Cached view state */
	FFragmentViewState ViewState;

	/** Cached tan(fov/2) to avoid recalculation */
	mutable float CachedTanHalfFOV = 0.0f;
	mutable float CachedFOV = 0.0f;

	/** Last camera position for change detection */
	FVector LastCameraPosition = FVector::ZeroVector;

	/** Last camera rotation for change detection */
	FRotator LastCameraRotation = FRotator::ZeroRotator;

	// --- Helper Methods ---

	/**
	 * Test if box is inside frustum.
	 * Port of engine_fragment's frustumCollide().
	 * @param Box World-space bounding box
	 * @return true if box intersects or is inside frustum
	 */
	bool IsInFrustum(const FBox& Box) const;

	/**
	 * Calculate screen size in pixels.
	 * Port of engine_fragment's screenSize().
	 * @param Dimension Object world-space dimension
	 * @param Distance Distance from camera
	 * @return Projected screen size in pixels
	 */
	float CalculateScreenSize(float Dimension, float Distance) const;

	/**
	 * Get distance to closest point on box.
	 * Port of Three.js Box3.distanceToPoint().
	 * @param Box World-space bounding box
	 * @return Distance (0 if camera inside box)
	 */
	float GetDistanceToBox(const FBox& Box) const;

	/**
	 * Build frustum planes from camera parameters.
	 */
	void BuildFrustumPlanes(const FVector& CameraLocation, const FRotator& CameraRotation,
	                        float FOV, float AspectRatio);

	/**
	 * Test box against a single frustum plane.
	 * Optimized AABB-plane test using p-vertex.
	 */
	static bool BoxIntersectsPlane(const FBox& Box, const FPlane& Plane);

	/**
	 * Get view dimension for perspective projection.
	 */
	float GetViewDimension(float Distance) const;
};
