

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/CollisionProfile.h"
#include "Utils/FragmentsUtils.h"
#include "FragmentsImporterSubsystem.generated.h"

// Cached component state for visibility toggling (from HOK fork)
struct FComponentSavedState
{
    FName CollisionProfile;
    ECollisionEnabled::Type CollisionEnabled;
    bool bGenerateOverlapEvents = false;
    bool bTickEnabled = false;
    bool bHiddenInGame = false;
};

/**
 *
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentsImporterSubsystem : public UGameInstanceSubsystem
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

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemPropertySets(int64 LocalId, const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	TArray<FItemAttribute> GetItemAttributes(int64 LocalId, const FString& InModelGuid);

	FFragmentItem* GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid);
	void GetItemData(FFragmentItem* InFragmentItem);

	UFUNCTION(BlueprintCallable)
	AFragment* GetModelFragment(const FString& InModelGuid);

	UFUNCTION(BlueprintCallable)
	FTransform GetBaseCoordinates();

	UFUNCTION(BlueprintCallable)
	void ResetBaseCoordinates();

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}

	// ==========================================
	// Visibility Management (from HOK fork)
	// ==========================================

	static void SetHierarchyVisible(AActor* Root, bool bVisible)
	{
		if (!IsValid(Root)) return;

		if (bVisible)
		{
			RestoreActor(Root);
		}
		else
		{
			SaveAndHideActor(Root);
		}

		TArray<AActor*> Attached;
		Root->GetAttachedActors(Attached);
		for (AActor* Child : Attached)
		{
			SetHierarchyVisible(Child, bVisible);
		}
	}

	static bool IsHierarchyVisible(const AActor* Root)
	{
		return IsValid(Root) ? !Root->IsHidden() : false;
	}

protected:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	class UFragmentsImporter* Importer = nullptr;

	// Visibility state caches (static for global access)

	static TMap<TWeakObjectPtr<AActor>, bool>& ActorTickCache()
	{
		static TMap<TWeakObjectPtr<AActor>, bool> S;
		return S;
	}

	static TMap<TWeakObjectPtr<UPrimitiveComponent>, FComponentSavedState>& Cache()
	{
		static TMap<TWeakObjectPtr<UPrimitiveComponent>, FComponentSavedState> S;
		return S;
	}

	static void SaveAndHideActor(AActor* A)
	{
		if (!IsValid(A)) return;

		if (!ActorTickCache().Contains(A))
		{
			ActorTickCache().Add(A, A->IsActorTickEnabled());
		}

		A->SetActorTickEnabled(false);
		A->SetActorHiddenInGame(true);

		TInlineComponentArray<UPrimitiveComponent*> PrimComps;
		A->GetComponents(PrimComps);
		for (UPrimitiveComponent* C : PrimComps)
		{
			if (!IsValid(C)) continue;

			if (!Cache().Contains(C))
			{
				FComponentSavedState State;
				State.CollisionProfile = C->GetCollisionProfileName();
				State.CollisionEnabled = C->GetCollisionEnabled();
				State.bGenerateOverlapEvents = C->GetGenerateOverlapEvents();
				State.bTickEnabled = C->IsComponentTickEnabled();
				State.bHiddenInGame = C->bHiddenInGame;
				Cache().Add(C, State);
			}

			C->SetHiddenInGame(true);
			C->SetGenerateOverlapEvents(false);
			C->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetComponentTickEnabled(false);
		}
	}

	static void RestoreActor(AActor* A)
	{
		if (!IsValid(A)) return;

		TInlineComponentArray<UPrimitiveComponent*> PrimComps;
		A->GetComponents(PrimComps);
		for (UPrimitiveComponent* C : PrimComps)
		{
			if (!IsValid(C)) continue;

			if (FComponentSavedState* Saved = Cache().Find(C))
			{
				C->SetHiddenInGame(Saved->bHiddenInGame);
				C->SetCollisionProfileName(Saved->CollisionProfile);
				C->SetCollisionEnabled(Saved->CollisionEnabled);
				C->SetGenerateOverlapEvents(Saved->bGenerateOverlapEvents);
				C->SetComponentTickEnabled(Saved->bTickEnabled);
				Cache().Remove(C);
			}
			else
			{
				C->SetHiddenInGame(false);
				C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				C->SetGenerateOverlapEvents(true);
				C->SetComponentTickEnabled(C->PrimaryComponentTick.bCanEverTick);
			}
		}

		const bool* bTick = ActorTickCache().Find(A);
		if (bTick) { A->SetActorTickEnabled(*bTick); ActorTickCache().Remove(A); }
		else { A->SetActorTickEnabled(A->PrimaryActorTick.bCanEverTick); }

		A->SetActorHiddenInGame(false);
	}

};
