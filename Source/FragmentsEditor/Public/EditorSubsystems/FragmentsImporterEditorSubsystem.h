

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Utils/FragmentsUtils.h"
#include "FragmentsImporterEditorSubsystem.generated.h"

/**
 * Editor-only subsystem for fragment import/query during editor sessions.
 * Handles world cleanup and map-open lifecycle automatically.
 */
UCLASS()
class FRAGMENTSEDITOR_API UFragmentsImporterEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintCallable)
	FString LoadFragment(const FString& FragPath);

	UFUNCTION(BlueprintCallable)
	void UnloadFragment(const FString& ModelGuid);

	UFUNCTION(BlueprintCallable)
	FString ProcessFragment(AActor* OwnerActor, const FString& FragPath, TArray<class AFragment*>& OutFragments, bool bSaveMeshes,
		bool bUseDynamicMesh = false, bool bUseHISM = false, AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		bool bUseDynamicMesh = false, bool bUseHISM = false, AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	void ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
		bool bUseDynamicMesh = false, bool bUseHISM = false, AFragment* BucketRoot = nullptr);

	UFUNCTION(BlueprintCallable)
	TArray<int64> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);

	UFUNCTION(BlueprintCallable)
	AFragment* GetItemByLocalId(int64 InLocalId, const FString& InModelGuid);

	FFragmentItem* GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid);
	void GetItemData(FFragmentItem* InFragmentItem);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemPropertySets(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemAttributes(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	AFragment* GetModelFragment(const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	FTransform GetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	void ResetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	bool IsFragmentLoaded(const FString& InModelGuid);

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}

protected:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:

	void OnWorldCleanup(UWorld* World, bool, bool);
	void OnMapOpened(const FString&, bool);

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	class UFragmentsImporter* Importer = nullptr;

};
