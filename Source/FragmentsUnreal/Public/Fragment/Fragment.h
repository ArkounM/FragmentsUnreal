

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Utils/FragmentsUtils.h"
#include "Fragment.generated.h"

// HISM bucket system structs (from HOK fork)

USTRUCT()
struct FFragLocalids
{
	GENERATED_BODY()

	UPROPERTY() TArray<int64> LocalIds;
};

USTRUCT()
struct FFragBucketKey
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<UStaticMesh> Mesh = nullptr;
	UPROPERTY() TObjectPtr<UMaterialInterface> Material = nullptr;
	UPROPERTY() int64 FloorId = INDEX_NONE;
	bool operator==(const FFragBucketKey& O) const { return Mesh == O.Mesh && Material == O.Material && FloorId == O.FloorId; }
};

USTRUCT()
struct FFragInstanceRef
{
	GENERATED_BODY()
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Comp = nullptr;
	UPROPERTY() int32 InstanceIndex = INDEX_NONE;
};

USTRUCT()
struct FFragInstanceArray
{
	GENERATED_BODY()
	UPROPERTY() TArray<FFragInstanceRef> Items;
};

FORCEINLINE uint32 GetTypeHash(const FFragBucketKey& K)
{
	uint32 H = HashCombine(::GetTypeHash(K.Mesh), ::GetTypeHash(K.Material));
	return HashCombine(H, ::GetTypeHash(K.FloorId));
}

UCLASS()
class FRAGMENTSUNREAL_API AFragment : public AActor
{
	GENERATED_BODY()

public:
	AFragment();

	void SetModelGuid(const FString& InModelGuid) { ModelGuid = InModelGuid; }
	void SetLocalId(const int64 InLocalId) { LocalId = InLocalId; }
	void SetCategory(const FString& InCategory) { Category = InCategory; }
	void SetGuid(const FString& InGuid) { Guid = InGuid; }
	void SetAttributes(TArray<struct FItemAttribute> InAttributes) { Attributes = InAttributes; }
	void SetChildren(TArray<AFragment*> InChildren) { FragmentChildren = InChildren; }
	void AddSampleInfo(const struct FFragmentSample& Sample) { Samples.Add(Sample); }
	void SetGlobalTransform(const FTransform& InGlobalTransform) { GlobalTransform = InGlobalTransform; }
	FTransform GetGlobalTransform() const { return GlobalTransform; }
	void SetData(FFragmentItem InFragmentItem);

	void AddChild(AFragment* InChild) {
		if (InChild) FragmentChildren.Add(InChild);
	}

	// HISM instance lookup
	bool GetLocalIdForInstance(const UHierarchicalInstancedStaticMeshComponent* HISM, int32 InstanceIndex, int64& OutLocalId) const;
	void GetAllLocalIdsForBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const;
	TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*> GetBuckets() { return Buckets; }

#if WITH_EDITOR
	void GetSelectedLocalIdsforBucket(const UHierarchicalInstancedStaticMeshComponent* HISM, TArray<int64>& OutLocalIds) const;
#endif

	// Blueprint-callable getters
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetModelGuid() const { return ModelGuid; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") int64 GetLocalId() const { return LocalId; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetCategory() const { return Category; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") FString GetGuid() const { return Guid; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") TArray<struct FItemAttribute> GetAttributes();
	UFUNCTION(BlueprintCallable, Category = "Fragments") TArray<AFragment*> GetChildren() const { return FragmentChildren; }
	UFUNCTION(BlueprintCallable, Category = "Fragments") AFragment* FindFragmentByLocalId(int64 InLocalId);
	UFUNCTION(BlueprintCallable, Category = "Fragments") const TArray<struct FFragmentSample>& GetSamples() const { return Samples; }

	// HISM API
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool SetHighlightedByLocalId(int64 InLocalId, bool bHighlighted);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ClearAllHISMHighlights();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool SetHiddenByLocalId(int64 InLocalId, bool bIsHidden);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ClearAllHISMHidden();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsLocalIdHidden(int64 InLocalId) const { return HiddenElementLocalIds.Contains(InLocalId); }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") int32 GetHiddenCount() const { return HiddenElementLocalIds.Num(); }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ShowOnlyFloor(int64 InFloorId);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetFloorVisible(int64 FloorKey, bool bVisible);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void ShowAllFloors();
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsFloorVisible(int64 InFloorId);
	UFUNCTION(BlueprintCallable, Category = "Fragments|Perf") void EnableProximityCulling(float StartFadeCm, float EndCullCm, bool bAlsoApplytoNonHISM = true);
	UFUNCTION(BlueprintCallable, Category = "Fragments|Perf") void EnableProximityCullingMeters(float StartFadeM, float EndCullM, bool bAlsoApplyToNonHISM = true)
	{
		EnableProximityCulling(StartFadeM * 100.0f, EndCullM * 100.0f, bAlsoApplyToNonHISM);
	}

	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetHISMEnabled(bool bEnable) { bUseHISM = bEnable; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool IsHISMEnabled() const { return bUseHISM; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") void SetAsBucketRoot(bool bRoot) { bBucketRoot = bRoot; }
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") AFragment* GetBucketRoot();
	int32 AddHISMInstance(UStaticMesh* Mesh, UMaterialInterface* Mat, const FTransform& WorldXd, int64 InLocalId, int64 InFloorId);
	UFUNCTION(BlueprintCallable, Category = "Fragments|HISM") bool ResolveHitToLocalId(const FHitResult& Hit, int64& OutLocalId) const;

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString ModelGuid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	int64 LocalId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString Category;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	FString Guid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Fragments|Attributes")
	TArray<struct FItemAttribute> Attributes;

private:

	FFragmentItem Data;
	UPROPERTY() TArray<AFragment*> FragmentChildren;
	UPROPERTY() TArray<struct FFragmentSample> Samples;
	UPROPERTY() FTransform GlobalTransform;

	// HISM bucket system
	UPROPERTY() bool bUseHISM = true;
	UPROPERTY() bool bBucketRoot = false;
	UPROPERTY() TMap<FFragBucketKey, UHierarchicalInstancedStaticMeshComponent*> Buckets;
	UPROPERTY() TMap<UHierarchicalInstancedStaticMeshComponent*, FFragLocalids> InstanceLocalIds;
	UPROPERTY() TMap<int64, FFragInstanceArray> LocalIdToInstance;
	UPROPERTY() TSet<int64> HighlightedLocalIds;
	UPROPERTY() TSet<int64> HiddenElementLocalIds;
	UPROPERTY() TSet<int64> HiddenLocalIds;

	UHierarchicalInstancedStaticMeshComponent* GetOrCreateBucket(UStaticMesh* Mesh, UMaterialInterface* Mat, int64 InFloorId);

};
