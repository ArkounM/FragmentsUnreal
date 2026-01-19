


#include "Importer/FragmentModelWrapper.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentModelWrapper, Log, All);

void UFragmentModelWrapper::BuildFragmentRegistry(const FString& ModelGuid)
{
	if (!ParsedModel)
	{
		UE_LOG(LogFragmentModelWrapper, Error, TEXT("Cannot build fragment registry - model not loaded"));
		return;
	}

	FragmentRegistry = NewObject<UFragmentRegistry>(this);
	FragmentRegistry->BuildFromModel(this, ModelGuid);

	UE_LOG(LogFragmentModelWrapper, Log, TEXT("Fragment registry built for model: %s (%d fragments)"),
	       *ModelGuid, FragmentRegistry->GetFragmentCount());
}

