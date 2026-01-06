

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Spatial/FragmentOctree.h"
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


public:
	void LoadModel(const TArray<uint8>& InBuffer)
	{
		RawBuffer = InBuffer;
		ParsedModel = GetModel(RawBuffer.GetData());
	}

	const Model* GetParsedModel() { return ParsedModel; }

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

};
