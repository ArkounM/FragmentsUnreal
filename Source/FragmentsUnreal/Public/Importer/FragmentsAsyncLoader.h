#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "FragmentsAsyncLoader.generated.h"

class UFragmentsImporter;

/** Callback fired when async fragment loading completes. */
DECLARE_DYNAMIC_DELEGATE_ThreeParams(
	FOnFragmentLoadComplete,
	bool, bSuccess,
	const FString&, ErrorMessage,
	const FString&, ModelGuid
);

/** Background task: loads .frag file, decompresses (zlib), parses FlatBuffers, builds hierarchy. */
class FFragmentLoadTask : public FNonAbandonableTask
{
	friend class FAutoDeleteAsyncTask<FFragmentLoadTask>;

public:
	FString FragmentPath;
	TWeakObjectPtr<UFragmentsImporter> ImporterPtr;

	TArray<uint8> DecompressedBuffer;
	UFragmentModelWrapper* ModelWrapper;
	TArray<FFragmentItem> RootItems;
	FString ModelGuid;
	bool bSuccess;
	FString ErrorMessage;
	FString LoadingStage;

	FFragmentLoadTask(const FString& InPath, UFragmentsImporter* InImporter)
		: FragmentPath(InPath),
		ImporterPtr(InImporter),
		ModelWrapper(nullptr),
		bSuccess(false)
	{
	}

	void DoWork();

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FFragmentLoadTask, STATGROUP_ThreadPoolAsyncTasks);
	}
};

/** Async loader for fragment files. UObject managed by UE garbage collector. */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentsAsyncLoader : public UObject
{
	GENERATED_BODY()

public:
		UFUNCTION(BlueprintCallable, Category = "Fragments")
		void LoadFragmentAsync(const FString& FragmentPath, FOnFragmentLoadComplete OnComplete, UFragmentsImporter* InImporter);

		UFUNCTION(BlueprintPure, Category = "Fragments")
		bool IsLoading() const {
			return bIsLoading;
		}

		UFUNCTION(BlueprintPure, Category = "Fragments")
		float GetLoadProgress() {
			return LoadProgress;
		}

		UFUNCTION(BlueprintPure, Category = "Fragments")
		FString GetLoadingStage() const {
			return LoadingStage;
		}

		UFUNCTION(BlueprintCallable, Category = "Fragments")
		void CancelLoad();

protected:
	void CheckTaskCompletion();

private:
	FAsyncTask<FFragmentLoadTask>* CurrentTask;

	FOnFragmentLoadComplete CompletionCallback;

	UPROPERTY()
	bool bIsLoading;

	UPROPERTY()
	float LoadProgress;

	UPROPERTY()
	FString LoadingStage;

	FTimerHandle CheckCompletionTimer;

	UPROPERTY()
	UFragmentsImporter* Importer;

	FOnFragmentLoadComplete PendingCallback;
};
