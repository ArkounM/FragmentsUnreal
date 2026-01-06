#include "Importer/FragmentsAsyncLoader.h"
#include "Importer/FragmentsImporter.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "TimerManager.h"
#include "Utils/FragmentsUtils.h"

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
void UFragmentsAsyncLoader::LoadFragmentAsync(const FString& FragmentPath, FOnFragmentLoadComplete OnComplete, UFragmentsImporter* InImporter)
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

	// Store the importer reference (passed from FragmentsImporter.cpp)
	Importer = InImporter;

	if (!Importer)
	{
		UE_LOG(LogTemp, Error, TEXT("No importer provided to LoadFragmentAsync"));
		OnComplete.ExecuteIfBound(false, TEXT("No Importer"), TEXT(""));
		return;
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
			// Create Model Wrapper on Game Thread
			UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(Importer);

			// Load the decompressed buffer into the wrapper
			Wrapper->LoadModel(Task.DecompressedBuffer);

			// Get the parsed models
			const Model* ParsedModel = Wrapper->GetParsedModel();

			if (ParsedModel && ParsedModel->spatial_structure())

			{
				// Create root FFragmentItem
				FFragmentItem RootItem;
				RootItem.ModelGuid = Task.ModelGuid;
				RootItem.LocalId = -1; // Root should not have localID

				// Build Hierarchy from spatial structure
				UFragmentsUtils::MapModelStructureToData(
					ParsedModel->spatial_structure(),
					RootItem,
					TEXT("") // No inherited category for root
				);

				// Store the hierarchy in the wrapper
				Wrapper->SetModelItem(RootItem);

				// Populate Samples
				const Meshes* _meshes = ParsedModel->meshes();
				const auto* local_ids = ParsedModel->local_ids();

				if (_meshes && local_ids)
				{
					const auto* samples = _meshes->samples();
					const auto* meshes_items = _meshes->meshes_items();
					const auto* global_transforms = _meshes->global_transforms();

					if (samples && meshes_items && global_transforms)
					{
						// Group samples by Item ID
						TMap<int32, TArray<const Sample*>> SamplesByItem;
						for (flatbuffers::uoffset_t i = 0; i < samples->size(); i++)
						{
							const auto* sample = samples->Get(i);
							SamplesByItem.FindOrAdd(sample->item()).Add(sample);
						}

						UE_LOG(LogTemp, Log, TEXT("Found %d sample groups"), SamplesByItem.Num());

						// Assign samples to corresponding fragment items
						for (const auto& Item : SamplesByItem)
						{
							int32 ItemId = Item.Key;
							const TArray<const Sample*> ItemSamples = Item.Value;

							const auto mesh = meshes_items->Get(ItemId);
							const auto local_id = local_ids->Get(ItemId);

							// Find the fragment item with this local_id
							FFragmentItem* FoundFragmentItem = nullptr;
							if (Wrapper->GetModelItem().FindFragmentByLocalId(local_id, FoundFragmentItem))
							{
								// Populate item data (category, guid, attributes)
								if (Importer)
								{
									Importer->GetItemData(FoundFragmentItem);
								}

								// Set global transform
								const auto* global_transform = global_transforms->Get(mesh);
								FTransform GlobalTransform = UFragmentsUtils::MakeTransform(global_transform);
								FoundFragmentItem->GlobalTransform = GlobalTransform;

								// Add all samples for this item
								for (int32 i = 0; i < ItemSamples.Num(); i++)
								{
									const Sample* sample = ItemSamples[i];

									FFragmentSample SampleInfo;
									SampleInfo.SampleIndex = i;
									SampleInfo.LocalTransformIndex = sample->local_transform();
									SampleInfo.RepresentationIndex = sample->representation();
									SampleInfo.MaterialIndex = sample->material();

									FoundFragmentItem->Samples.Add(SampleInfo);
								}

								UE_LOG(LogTemp, Verbose, TEXT("Populated %d samples for LocalId %d"), ItemSamples.Num(), local_id);
							}
							else
							{
								UE_LOG(LogTemp, Warning, TEXT("Could not find FragmentItem for LocalId: %d"), local_id);
							}
						}
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("Model has no meshes or local_ids"));
				}

				// Store wrapper in importer's FragmentModels map
				if (Importer)
				{
					Importer->GetFragmentModels_Mutable().Add(Task.ModelGuid, Wrapper);
				}

				// Build spatial index for tile-based streaming
				Wrapper->BuildSpatialIndex(Task.ModelGuid);

				UE_LOG(LogTemp, Log, TEXT("Model stored successfully: %s"), *Task.ModelGuid);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("Failed to parse spatial structure"));
				CompletionCallback.ExecuteIfBound(false, TEXT("Invalid spatial structure"), TEXT(""));
				delete CurrentTask;
				CurrentTask = nullptr;
				bIsLoading = false;
				return;
			}

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