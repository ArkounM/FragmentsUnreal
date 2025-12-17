

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
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


private:
	// Cache of all fragment items by LocalId for fast lookup
	TMap<int32, TSharedPtr<FFragmentItem>> FragmentItemCache;

	// Build fragment cache recursively
	void BuildFragmentCache(FFragmentItem* Item)
	{
		if (!Item)
		{
			return;
		}

		// Add this item to cache
		TSharedPtr<FFragmentItem> SharedItem = MakeShared<FFragmentItem>(*Item);
		FragmentItemCache.Add(Item->LocalId, SharedItem);

		// Recursively cache children
		for (FFragmentItem* Child : Item->FragmentChildren)
		{
			BuildFragmentCache(Child);
		}
	}

public:
	void LoadModel(const TArray<uint8>& InBuffer)
	{
		RawBuffer = InBuffer;
		ParsedModel = GetModel(RawBuffer.GetData());
	}

	const Model* GetParsedModel() { return ParsedModel; }

	void SetModelItem(FFragmentItem InModelItem)
	{
		ModelItem = InModelItem;

		// Build cache for fast lookups
		FragmentItemCache.Empty();
		BuildFragmentCache(&ModelItem);
	}

	FFragmentItem GetModelItem() { return ModelItem; }

	/**
	 * Get all fragment items as a map (LocalId -> FragmentItem)
	 * This is used by the octree to iterate over all fragments
	 */
	const TMap<int32, TSharedPtr<FFragmentItem>>& GetFragmentItems() const
	{
		return FragmentItemCache;
	}

	/**
	 * Get the world transform for a fragment by local ID
	 * @param LocalID Fragment local ID
	 * @param OutTransform Output transform
	 * @return True if fragment found
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	bool GetFragmentTransform(int32 LocalID, FTransform& OutTransform) const
	{
		const TSharedPtr<FFragmentItem>* FoundItem = FragmentItemCache.Find(LocalID);
		if (FoundItem && FoundItem->IsValid())
		{
			OutTransform = (*FoundItem)->GlobalTransform;
			return true;
		}
		return false;
	}

};
