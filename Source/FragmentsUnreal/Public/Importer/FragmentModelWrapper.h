#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Spatial/FragmentRegistry.h"
#include "FragmentModelWrapper.generated.h"

// Forward declarations
class AFragment;

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

	/** Fragment registry for per-sample visibility (Phase 1 optimization) */
	UPROPERTY()
	UFragmentRegistry* FragmentRegistry = nullptr;

	/** Root fragment actor spawned from this model (HOK lifecycle) */
	UPROPERTY()
	AFragment* SpawnedFragment = nullptr;

	/** Material instances by representation index (HOK material caching) */
	UPROPERTY()
	TMap<int32, UMaterialInstanceDynamic*> MaterialsMap;


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
	FFragmentItem& GetModelItemRef() { return ModelItem; }  // Non-const for geometry extraction

	/**
	 * Build fragment registry for per-sample visibility
	 * @param ModelGuid Model identifier
	 */
	void BuildFragmentRegistry(const FString& ModelGuid);

	/**
	 * Get the fragment registry
	 */
	UFragmentRegistry* GetFragmentRegistry() const { return FragmentRegistry; }

	/** Reset wrapper state when changing worlds (HOK lifecycle) */
	void ResetWrapper();

	/** Check if this wrapper references actors/materials in the given world */
	bool ReferencesWorld(const UWorld* World) const;

	/** Set/Get the root fragment actor spawned from this model */
	void SetSpawnedFragment(AFragment* InSpawnedFragment) { SpawnedFragment = InSpawnedFragment; }
	AFragment* GetSpawnedFragment() const { return SpawnedFragment; }

	/** Get materials map */
	TMap<int32, UMaterialInstanceDynamic*>& GetMaterialsMap() { return MaterialsMap; }

};
