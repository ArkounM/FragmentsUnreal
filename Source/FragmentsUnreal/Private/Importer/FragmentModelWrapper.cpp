


#include "Importer/FragmentModelWrapper.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentModelWrapper, Log, All);

void UFragmentModelWrapper::BuildSpatialIndex(const FString& ModelGuid)
{
	if (!ParsedModel)
	{
		UE_LOG(LogFragmentModelWrapper, Error, TEXT("Cannot build spatial index - model not loaded"));
		return;
	}

	SpatialIndex = NewObject<UFragmentOctree>(this);
	SpatialIndex->BuildFromModel(this, ModelGuid);

	UE_LOG(LogFragmentModelWrapper, Log, TEXT("Spatial index built for model: %s"), *ModelGuid);
}

