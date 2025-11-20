#include "Importer/FragmentsAsyncLoader.h"
#include "Importer/FragmentsImporter.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "TimerManager.h"

// for zlib decompression
#include "zlib.h"

// FlatBuffers 
#include "Index/index_generated.h"

// DoWork implementation - runs on background thread
void FFragmentLoadTask::DoWork()
{
	// Update stage (thread-safe since we're only writing)
	LoadingStage = TEXT("Loading file from disk...");

	// Step 1: Load file into memory
	TArray<uint8> RawFileData;
	if (!FFileHelper::LoadFileToArray(RawFileData, *FragmentPath))
	{
		// File load failed
		bSuccess = false;
		ErrorMessage = FString::Printf(TEXT("Failed to load file: %s"), *FragmentPath);
		return;
	}
	
	LoadingStage = TEXT("Decompressing data...");

	// Step 2: Check if compressed (zlib magic number: 0x78)
	bool bIsCompressed = (RawFileData.Num() > 0 && RawFileData[0] == 0x78);

	if (bIsCompressed)
	{
		// Use raw zlib for dynamic decompression
		TArray<uint8> DecompBuffer;
		DecompBuffer.Reserve(RawFileData.Num() * 10); // Estimate

		const int32 ChunkSize = 1024 * 1024; // 1 MB chunks
		TArray<uint8> TempBuffer;
		TempBuffer.SetNumUninitialized(ChunkSize);

		z_stream Stream;
		FMemory::Memzero(&Stream, sizeof(z_stream));

		Stream.zalloc = Z_NULL;
		Stream.zfree = Z_NULL;
		Stream.opaque = Z_NULL;
		Stream.avail_in = RawFileData.Num();
		Stream.next_in = RawFileData.GetData();

		int Ret = inflateInit(&Stream);
		if (Ret != Z_OK)
		{
			bSuccess = false;
			ErrorMessage = TEXT("Failed to initialize zlib decompression");
			return;
		}

		// Decompress in chunks
		do
		{
			Stream.avail_out = ChunkSize;
			Stream.next_out = TempBuffer.GetData();

			Ret = inflate(&Stream, Z_NO_FLUSH);

			if (Ret != Z_OK && Ret != Z_STREAM_END)
			{
				inflateEnd(&Stream);
				bSuccess = false;
				ErrorMessage = TEXT("Decompression error");
				return;
			}

			int32 DecompressedSize = ChunkSize - Stream.avail_out;
			DecompBuffer.Append(TempBuffer.GetData(), DecompressedSize);

		} while (Ret != Z_STREAM_END);

		inflateEnd(&Stream);

		// Use decompressed data
		DecompressedBuffer = MoveTemp(DecompBuffer);
	}
	else
	{
		// Not compressed, use as-is
		DecompressedBuffer = MoveTemp(RawFileData);
	}

	LoadingStage = TEXT("Parsing FlatBuffers...");

	// Step 3: Parse FlatBuffers
	// Note: GetModel returns a pointer into our buffer (zero-copy)
	const Model* ParsedModel = GetModel(DecompressedBuffer.GetData());
	if (!ParsedModel)
	{
		bSuccess = false;
		ErrorMessage = TEXT("Failed to parse FlatBuffers data");
		return;
	}

	// Extract GUID
	if (ParsedModel->guid())
	{
		ModelGuid = FString(UTF8_TO_TCHAR(ParsedModel->guid()->c_str()));
	}
	else
	{
		ModelGuid = TEXT("Unknown");
	}
	
	LoadingStage = TEXT("Building hierarchy...");

	// Step 4: Build Hierarchy tree by first storing structure async and then do the full build on game thread
	// Get spatial structure root
	const SpatialStructure * SpatialRoot = ParsedModel->spatial_structure();
	if (SpatialRoot)
	{
		// Will parse on game thread
		// For now just mark success
	}

	LoadingStage = TEXT("Complete!");
	bSuccess = true;
}

// LoadFragmentAsync -> Starts async task
void UFragmentsAsyncLoader::LoadFragmentAsync(const FString& FragmentPath, FOnFragmentLoadComplete OnComplete)
{
		// Don't start if already loading
	if (bIsLoading)
	{
		UE_LOG(LogTemp, Warning, TEXT("Already loading a fragment, ignoring new request"));
		OnComplete.ExecuteIfBound(false, TEXT("Already loading"), TEXT(""));
			return;
		}
	// Validate Path
	if (!FPaths::FileExists(FragmentPath))
	{
		OnComplete.ExecuteIfBound(false, TEXT("File does not exist"), TEXT(""));
		return;
	}

	// Store Callback
	CompletionCallback = OnComplete;

	// Get Importer Reference 
	if (!Importer)
	{
		// Find or create importer
		Importer = NewObject<UFragmentsImporter>(this);
	}

	// Create and start async task
	CurrentTask = new FAsyncTask<FFragmentLoadTask>(FragmentPath, Importer);
	CurrentTask->StartBackgroundTask();

	// Set Loading State
	bIsLoading = true;
	LoadProgress = 0.0f;
	LoadingStage = TEXT("Starting...");

	// Start timer to check completion (every 0.1 seconds)
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			CheckCompletionTimer,
			this,
			&UFragmentsAsyncLoader::CheckTaskCompletion,
			0.1f, // Check every 100 ms
			true // Repeat
		);
	}
}

// Check task completion periodically to track when task is done
void UFragmentsAsyncLoader::CheckTaskCompletion()
{
	if (!CurrentTask)
	{
		return;
	}

	// Check if done
	if (CurrentTask->IsDone())
	{
		// Ensure internal task cleanup is finished before accessing results
		CurrentTask->EnsureCompletion();

		// Stop Timer
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(CheckCompletionTimer);
		}

		// Get Results
		FFragmentLoadTask& Task = CurrentTask->GetTask();

		if (Task.bSuccess)
		{
			// For now, just log success - we'll implement model storage later
			UE_LOG(LogTemp, Log, TEXT("Model loaded successfully: %s"), *Task.ModelGuid);

			// Notify completion
			LoadProgress = 1.0f;
			LoadingStage = TEXT("Complete!");
			CompletionCallback.ExecuteIfBound(true, TEXT(""), Task.ModelGuid);

		}
		else
		{
			// Failed
			CompletionCallback.ExecuteIfBound(false, Task.ErrorMessage, TEXT(""));
		}

		// Cleanup
		delete CurrentTask;
		CurrentTask = nullptr;
			bIsLoading = false;
	}
}

// Cancel load to stop current operation
void UFragmentsAsyncLoader::CancelLoad()
{
	if (CurrentTask)
	{
		// While we can't cancel FNonAbandonableTask mid-execution we can stop tracking to ignore the results. 
		// Might require future modification to ensure proper unloading/cleanup of cancelled tasks
		
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(CheckCompletionTimer);
		}

		// Task completes in background but we ignore results
		delete CurrentTask;
		CurrentTask = nullptr;
		bIsLoading = false;

		CompletionCallback.ExecuteIfBound(false, TEXT("Cancelled by user"), TEXT(""));
	}
}