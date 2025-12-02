#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "FragmentModelWrapper.h"  // For UFragmentModelWrapper
#include "Utils/FragmentsUtils.h"        // For FFragmentItem
#include "Importer/FragmentModelWrapper.h"
#include "FragmentsAsyncLoader.generated.h"

// Forward declarations (tells compiler these types exist)
class UFragmentsImporter;

/** Delegate (callback) for when loading is complete. 
* Serves as a function point that can be assigned and called later for when the async loading finishes/
*/
DECLARE_DYNAMIC_DELEGATE_ThreeParams(
	FOnFragmentLoadComplete,				// Delegate Name
	bool, bSuccess,							// Param 1: Did it Succeed?
	const FString&, ErrorMessage,			// Param 2: Error message if failed
	const FString&, ModelGuid				// Param 3: Loaded model's GUID
);

/**
* This is the Async task for loading and parsing Fragment files in the background
* 
* 1. Loads the .frag file from disk
* 2. Decompresses it (zlib)
* 3. Parses the FlatBuffers data
* 4. Builds the Hierarchy tree (FFragmentItem)
* 
* The goal is to have these steps happen in the background thread so game/ui stays responsive
*/
class FFragmentLoadTask : public FNonAbandonableTask
{
	friend class FAutoDeleteAsyncTask<FFragmentLoadTask>;

public:
	// Input data (set in constructor)
	FString FragmentPath;
	TWeakObjectPtr<UFragmentsImporter> ImporterPtr; // Weak pointer to ensure garbage collection doesn't get blocked

	// Output data (read after DoWork completes)
	TArray<uint8> DecompressedBuffer;
	UFragmentModelWrapper* ModelWrapper; // To be created on game thread
	TArray<FFragmentItem> RootItems;
	FString ModelGuid;
	bool bSuccess;
	FString ErrorMessage;
	FString LoadingStage;

	/**
	*Constructor - called on game thread
	*
	* @param InPath - Path to .frag file
	* @param InImporter - The importer that started this task
	*/

	FFragmentLoadTask(const FString& InPath, UFragmentsImporter* InImporter)
		: FragmentPath(InPath),
		ImporterPtr(InImporter), // Store as weak pointer
		ModelWrapper(nullptr),
		bSuccess(false)
	{
	}
	// DoWork runs on background thread. Allowed to Read/Write files + computation but cannot create UObjects or access game world
	void DoWork();

	// Start stat tracking with UE Task System
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FFragmentLoadTask, STATGROUP_ThreadPoolAsyncTasks);
	}
};

// Main Async loader class. UObject managed by UE Garabage collector. 
UCLASS()
class FRAGMENTSUNREAL_API UFragmentsAsyncLoader : public UObject
{
	GENERATED_BODY()

public:
		// Start loading fragment file async
		UFUNCTION(BlueprintCallable, Category = "Fragments")
		void LoadFragmentAsync(const FString& FragmentPath, FOnFragmentLoadComplete OnComplete, UFragmentsImporter* InImporter);

		// Check if load is currently in progress
		UFUNCTION(BlueprintPure, Category = "Fragments")
		bool IsLoading() const {
			return bIsLoading;
		}
		// Check Current load progress (0.0 to 1.0)
		UFUNCTION(BlueprintPure, Category = "Fragments")
		float GetLoadProgress() {
			return LoadProgress;
		}
		// Get current loading stage description
		UFUNCTION(BlueprintPure, Category = "Fragments")
		FString GetLoadingStage() const {
			return LoadingStage;
		}
		// Cancel current loading operation
		UFUNCTION(BlueprintCallable, Category = "Fragments")
		void CancelLoad();

protected:
		// Called each frame to check if async task completed using timer instead of blocking
	void CheckTaskCompletion();
	
private:
	// The async task (nullptr when not loading)
	FAsyncTask<FFragmentLoadTask>* CurrentTask;

	// Completion callback
	FOnFragmentLoadComplete CompletionCallback;

	// Loading State
	UPROPERTY()
	bool bIsLoading;

	UPROPERTY()
	float LoadProgress;

	UPROPERTY()
	FString LoadingStage;

	// Timer handle for checking completion
	FTimerHandle CheckCompletionTimer;

	// Reference to importer
	UPROPERTY()
	UFragmentsImporter* Importer;
	
	//Pending Completion Callback
	FOnFragmentLoadComplete PendingCallback;
};



