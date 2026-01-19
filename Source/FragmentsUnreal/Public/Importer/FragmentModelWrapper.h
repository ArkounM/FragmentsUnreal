#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Spatial/FragmentOctree.h"
#include "Spatial/FragmentRegistry.h"
#include "FragmentModelWrapper.generated.h"

/**
 * 
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentModelWrapper : public UObject
{
	GENERATED_BODY()


private:

	UPROPERTY()
	TArray<uint8> RawBuffer;

	const Model* ParsedModel = nullptr;

	FFragmentItem ModelItem;

	UPROPERTY()
	UFragmentOctree* SpatialIndex = nullptr;

	/** Fragment registry for per-sample visibility (Phase 1 optimization) */
	UPROPERTY()
	UFragmentRegistry* FragmentRegistry = nullptr;


public:
	void LoadModel(const TArray<uint8>& InBuffer)
	{
		RawBuffer = InBuffer;
		ParsedModel = GetModel(RawBuffer.GetData());
	}

	const Model* GetParsedModel() const { return ParsedModel; }

	void SetModelItem(FFragmentItem InModelItem) { ModelItem = InModelItem; }
	FFragmentItem GetModelItem() { return ModelItem; }
	const FFragmentItem& GetModelItemRef() const { return ModelItem; }

	/**
	 * Build spatial index for this model
	 * @param ModelGuid Model identifier
	 */
	void BuildSpatialIndex(const FString& ModelGuid);

	/**
	 * Get the spatial index
	 */
	UFragmentOctree* GetSpatialIndex() const { return SpatialIndex; }

	/**
	 * Build fragment registry for per-sample visibility
	 * @param ModelGuid Model identifier
	 */
	void BuildFragmentRegistry(const FString& ModelGuid);

	/**
	 * Get the fragment registry
	 */
	UFragmentRegistry* GetFragmentRegistry() const { return FragmentRegistry; }

};
