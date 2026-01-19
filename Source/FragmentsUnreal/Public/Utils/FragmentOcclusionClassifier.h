#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Utils/FragmentOcclusionTypes.h"
#include "FragmentOcclusionClassifier.generated.h"

/**
 * Utility class for classifying fragments based on their BIM category and material properties.
 * Used to configure Unreal Engine's built-in hardware occlusion system.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentOcclusionClassifier : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Classify a fragment based on its category and material alpha.
	 * @param Category IFC category string (e.g., "IfcWall", "IfcWindow")
	 * @param MaterialAlpha Material alpha value (0-255, 255 = fully opaque)
	 * @return Occlusion role for the fragment
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static EOcclusionRole ClassifyFragment(const FString& Category, uint8 MaterialAlpha);

	/**
	 * Check if a category represents a large structural element that should occlude.
	 * @param Category IFC category string
	 * @return true if category is an occluder type
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static bool IsOccluderCategory(const FString& Category);

	/**
	 * Check if a material is transparent based on alpha value.
	 * @param MaterialAlpha Material alpha value (0-255)
	 * @return true if material is transparent (alpha < TransparencyThreshold)
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static bool IsTransparentMaterial(uint8 MaterialAlpha);

	/**
	 * Get the occlusion role as a display string.
	 * @param Role Occlusion role enum value
	 * @return Human-readable string for the role
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Occlusion")
	static FString GetOcclusionRoleString(EOcclusionRole Role);

	/**
	 * Transparency threshold for material classification.
	 * Materials with alpha >= this value are considered opaque.
	 * 245 = ~96% opaque threshold.
	 */
	static constexpr uint8 TransparencyThreshold = 245;

private:
	/** Set of IFC categories that represent large structural occluders */
	static const TSet<FString>& GetOccluderCategories();
};
