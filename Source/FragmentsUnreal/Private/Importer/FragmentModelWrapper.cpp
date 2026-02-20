


#include "Importer/FragmentModelWrapper.h"
#include "Fragment/Fragment.h"

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

void UFragmentModelWrapper::ResetWrapper()
{
	SpawnedFragment = nullptr;
	MaterialsMap.Empty();
}

bool UFragmentModelWrapper::ReferencesWorld(const UWorld* World) const
{
	if (!World) return false;

	if (const AFragment* Frag = SpawnedFragment)
	{
		if (Frag->GetWorld() == World)
		{
			return true;
		}
	}

	for (const TPair<int32, UMaterialInstanceDynamic*>& KV : MaterialsMap)
	{
		if (const UMaterialInstanceDynamic* MID = KV.Value)
		{
			if (const UObject* Outer = MID->GetOuter())
			{
				if (const UWorld* MIDWorld = Outer->GetWorld())
				{
					if (MIDWorld == World) return true;
				}
			}
		}
	}

	return false;
}

