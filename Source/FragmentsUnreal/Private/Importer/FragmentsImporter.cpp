


#include "Importer/FragmentsImporter.h"
#include "flatbuffers/flatbuffers.h"
#include "Serialization/ArchiveLoadCompressedProxy.h"
#include "zlib.h"
#include "ProceduralMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StaticMeshDescription.h"
#include <StaticMeshOperations.h>
#include "Async/Async.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Curve/PolygonIntersectionUtils.h"
#include "CompGeom/PolygonTriangulation.h"
#include "tesselator.h"
#include "Algo/Reverse.h"
#include "Fragment/Fragment.h"
#include "Importer/FragmentModelWrapper.h"
#include "UObject/SavePackage.h"
#include "Misc/ScopedSlowTask.h"
#include "Importer/FragmentsAsyncLoader.h"
#include "Spatial/FragmentTileManager.h"
#include "Utils/FragmentOcclusionClassifier.h"
#include "Utils/FragmentGeometryWorker.h"
#include "Components/InstancedStaticMeshComponent.h"


void UFragmentsImporter::ProcessFragmentAsync(const FString& FragmentPath, AActor* Owner, FOnFragmentLoadComplete OnComplete)
{
	// Create async loader if needed
	if (!AsyncLoader)
	{
		AsyncLoader = NewObject<UFragmentsAsyncLoader>(this);
	}

	// Store callback
	PendingCallback = OnComplete;
	PendingOwner = Owner;

	// Start Async Load
	FOnFragmentLoadComplete LoadCallback;
	LoadCallback.BindUFunction(this, FName("OnAsyncLoadComplete"));
	AsyncLoader->LoadFragmentAsync(FragmentPath, LoadCallback, this);
}

void UFragmentsImporter::OnAsyncLoadComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid)
{
	if (!bSuccess)
	{
		UE_LOG(LogFragments, Error, TEXT("Async load failed: %s"), *ErrorMessage);
		PendingCallback.ExecuteIfBound(false, ErrorMessage, TEXT(""));
		return;
	}

	UE_LOG(LogFragments, Log, TEXT("Async load complete: %s"), *ModelGuid);

	// Verify model was stored
	if (!FragmentModels.Contains(ModelGuid))
	{
		UE_LOG(LogFragments, Error, TEXT("Model not found in FragmentsModel after async load"));
		PendingCallback.ExecuteIfBound(false, TEXT("Model not stored"), TEXT(""));
		return;	
	}

	UE_LOG(LogFragments, Error, TEXT("About to call ProcessLoadedFragment for: %p"), PendingOwner);

	// Set Owner reference before spawning
	if (PendingOwner)
	{
		SetOwnerRef(PendingOwner);
	}
	else
	{
		UE_LOG(LogFragments, Warning, TEXT("No owner provided for async spawn"));
	}

	// Leverage existing ProcessLoadedFragment to spawn actors
	// TEMP: Use nullptr owner and save meshes -> in the future this will be a passed obj and bool respectively
	ProcessLoadedFragment(ModelGuid, PendingOwner, true);

	UE_LOG(LogFragments, Error, TEXT("ProcessLoadedFragment returned for: %s"), *ModelGuid);

	// Just notify success for now
	PendingCallback.ExecuteIfBound(true, TEXT(""), ModelGuid);
}

DEFINE_LOG_CATEGORY(LogFragments);

UFragmentsImporter::UFragmentsImporter()
{
	DeduplicationManager = CreateDefaultSubobject<UGeometryDeduplicationManager>(TEXT("DeduplicationManager"));
}

FString UFragmentsImporter::Process(AActor* OwnerA, const FString& FragPath, TArray<AFragment*>& OutFragments, bool bSaveMeshes)
{
	SetOwnerRef(OwnerA);
	FString ModelGuidStr = LoadFragment(FragPath);

	if (ModelGuidStr.IsEmpty())	return FString();
	
	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(ModelGuidStr);
	const Model* ModelRef = Wrapper->GetParsedModel();

	BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
	BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));

	FDateTime StartTime = FDateTime::Now();
	SpawnFragmentModel(Wrapper->GetModelItem(), OwnerRef, ModelRef->meshes(), bSaveMeshes);
	UE_LOG(LogFragments, Warning, TEXT("Loaded model in [%s]s -> %s"), *(FDateTime::Now() - StartTime).ToString(), *ModelGuidStr);
	if (PackagesToSave.Num() > 0)
	{
		DeferredSaveManager.AddPackagesToSave(PackagesToSave);
		PackagesToSave.Empty();
	}
	
	return ModelGuidStr;
}

void UFragmentsImporter::GetItemData(AFragment*& InFragment)
{
	if (!InFragment || InFragment->GetModelGuid().IsEmpty()) return;

	if (FragmentModels.Contains(InFragment->GetModelGuid()))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragment->GetModelGuid());
		const Model* InModel = Wrapper->GetParsedModel();
		
		int32 ItemIndex = UFragmentsUtils::GetIndexForLocalId(InModel, InFragment->GetLocalId());
		if (ItemIndex == INDEX_NONE) return;
	
		// Attributes
		const auto* attribute = InModel->attributes()->Get(ItemIndex);
		TArray<FItemAttribute> ItemAttributes = UFragmentsUtils::ParseItemAttribute(attribute);
		InFragment->SetAttributes(ItemAttributes);

		// Category
		const auto* category = InModel->categories()->Get(ItemIndex);
		const char* RawCategory = category->c_str();
		FString CategorySty = UTF8_TO_TCHAR(RawCategory);
		InFragment->SetCategory(CategorySty);
		//ItemActor->Tags.Add(FName(CategorySty));

		// Guids
		const auto* item_guid = InModel->guids()->Get(ItemIndex);
		const char* RawGuid = item_guid->c_str();
		FString GuidStr = UTF8_TO_TCHAR(RawGuid);
		InFragment->SetGuid(GuidStr);
	}
}

void UFragmentsImporter::GetItemData(FFragmentItem* InFragmentItem)
{
	if (InFragmentItem->ModelGuid.IsEmpty()) return;

	if (FragmentModels.Contains(InFragmentItem->ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragmentItem->ModelGuid);
		const Model* InModel = Wrapper->GetParsedModel();

		int32 ItemIndex = UFragmentsUtils::GetIndexForLocalId(InModel, InFragmentItem->LocalId);
		flatbuffers::uoffset_t ii = ItemIndex;
		if (ItemIndex == INDEX_NONE) return;

		// Attributes
		if (ii < InModel->attributes()->size())
		{
			const auto* attribute = InModel->attributes()->Get(ItemIndex);
			TArray<FItemAttribute> ItemAttributes = UFragmentsUtils::ParseItemAttribute(attribute);
			InFragmentItem->Attributes = ItemAttributes;
		}

		// Category
		if (ii < InModel->categories()->size())
		{
			const auto* category = InModel->categories()->Get(ItemIndex);
			if (category)  // Null check for category
			{
				const char* RawCategory = category->c_str();
				FString CategorySty = UTF8_TO_TCHAR(RawCategory);
				InFragmentItem->Category = CategorySty;
			}
		}

		// Guids
		if (ii < InModel->guids()->size())
		{
			const auto* item_guid = InModel->guids()->Get(ItemIndex);
			if (item_guid)  
			{
				const char* RawGuid = item_guid->c_str();
				FString GuidStr = UTF8_TO_TCHAR(RawGuid);
				InFragmentItem->Guid = GuidStr;
			}
		}
	}
}

TArray<FItemAttribute> UFragmentsImporter::GetItemPropertySets(AFragment* InFragment)
{
	TArray<FItemAttribute> CollectedAttributes;
	if (!InFragment || InFragment->GetModelGuid().IsEmpty()) return CollectedAttributes;
	if (!FragmentModels.Contains(InFragment->GetModelGuid())) return CollectedAttributes;

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InFragment->GetModelGuid());
	const Model* InModel = Wrapper->GetParsedModel();
	if (!InModel) return CollectedAttributes;

	TSet<int32> Visited;
	CollectPropertiesRecursive(InModel, InFragment->GetLocalId(), Visited, CollectedAttributes);

	return CollectedAttributes;
}


AFragment* UFragmentsImporter::GetItemByLocalId(int32 LocalId, const FString& ModelGuid)
{
	if (ModelFragmentsMap.Contains(ModelGuid))
	{
		FFragmentLookup Lookup = *ModelFragmentsMap.Find(ModelGuid);

		if (Lookup.Fragments.Contains(LocalId))
		{
			return *Lookup.Fragments.Find(LocalId);
		}
	}
	return nullptr;
}

FFragmentItem* UFragmentsImporter::GetFragmentItemByLocalId(int32 LocalId, const FString& InModelGuid)
{
	if (FragmentModels.Contains(InModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
		FFragmentItem* FoundItem;
		if (Wrapper->GetModelItem().FindFragmentByLocalId(LocalId, FoundItem))
		{
			return FoundItem;
		}
	}
	return nullptr;
}

FString UFragmentsImporter::LoadFragment(const FString& FragPath)
{
	TArray<uint8> CompressedData;
	bool bIsCompressed = false;
	TArray<uint8> Decompressed;


	if (!FFileHelper::LoadFileToArray(CompressedData, *FragPath))
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to load the compressed file"));
		return FString();
	}

	if (CompressedData.Num() >= 2 && CompressedData[0] == 0x78)
	{
		bIsCompressed = true;
		UE_LOG(LogFragments, Log, TEXT("Zlib header detected. Starting decompression..."));
	}

	if (bIsCompressed)
	{
		// Use zlib directly (Unreal's zlib.h)
		z_stream stream = {};
		stream.next_in = CompressedData.GetData();
		stream.avail_in = CompressedData.Num();

		int32 ret = inflateInit(&stream);
		if (ret != Z_OK)
		{
			UE_LOG(LogFragments, Error, TEXT("zlib initialization failed: %d"), ret);
			return FString();
		}

		const int32 ChunkSize = 1024 * 1024;
		int32 TotalOut = 0;

		for (int32 i = 0; i < 100; ++i)
		{
			int32 OldSize = Decompressed.Num();
			Decompressed.AddUninitialized(ChunkSize);
			stream.next_out = Decompressed.GetData() + OldSize;
			stream.avail_out = ChunkSize;

			ret = inflate(&stream, Z_NO_FLUSH);

			if (ret == Z_STREAM_END)
			{
				TotalOut = OldSize + ChunkSize - stream.avail_out;
				break;
			}
			else if (ret != Z_OK)
			{
				UE_LOG(LogFragments, Error, TEXT("Decompression failed with error code: %d"), ret);
				break;
			}
		}

		ret = inflateEnd(&stream);
		if (ret != Z_OK)
		{
			UE_LOG(LogFragments, Error, TEXT("zlib end stream failed: %d"), ret);
			return FString();
		}

		Decompressed.SetNum(TotalOut);
		//UE_LOG(LogFragments, Log, TEXT("Decompression complete. Total bytes: %d"), TotalOut);
	}
	else
	{
		Decompressed = CompressedData;
		UE_LOG(LogFragments, Log, TEXT("Data appears uncompressed, using raw data"));
	}

	UFragmentModelWrapper* Wrapper = NewObject<UFragmentModelWrapper>(this);
	Wrapper->LoadModel(Decompressed);
	const Model* ModelRef = Wrapper->GetParsedModel();

	if (!ModelRef)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to parse Fragments model"));
		return FString();
	}

	const auto* guid = ModelRef->guid();
	const char* RawModelGuid = guid->c_str();
	FString ModelGuidStr = UTF8_TO_TCHAR(RawModelGuid);

	const auto* spatial_structure = ModelRef->spatial_structure();
	FTransform RootTransform = FTransform::Identity;
	FFragmentItem FragmentItem;
	FragmentItem.Guid = ModelGuidStr;
	FragmentItem.ModelGuid = ModelGuidStr;
	FragmentItem.GlobalTransform = RootTransform;
	UFragmentsUtils::MapModelStructureToData(spatial_structure, FragmentItem, TEXT(""));

	Wrapper->SetModelItem(FragmentItem);
	FragmentModels.Add(ModelGuidStr, Wrapper);

	const auto* local_ids = ModelRef->local_ids();
	const auto* _meshes = ModelRef->meshes();

	// Loop through samples and spawn meshes
	if (_meshes)
	{
		const auto* samples = _meshes->samples();
		const auto* representations = _meshes->representations();
		const auto* coordinates = _meshes->coordinates();
		const auto* meshes_items = _meshes->meshes_items();
		const auto* materials = _meshes->materials();
		const auto* cirle_extrusions = _meshes->circle_extrusions();
		const auto* shells = _meshes->shells();
		const auto* local_tranforms = _meshes->local_transforms();
		const auto* global_transforms = _meshes->global_transforms();

		// Grouping samples by Item ID
		TMap<int32, TArray<const Sample*>> SamplesByItem;
		for (flatbuffers::uoffset_t i = 0; i < samples->size(); i++)
		{
			const auto* sample = samples->Get(i);
			SamplesByItem.FindOrAdd(sample->item()).Add(sample);
		}

		for (const auto& Item : SamplesByItem)
		{
			int32 ItemId = Item.Key;

			const TArray<const Sample*> ItemSamples = Item.Value;

			const auto mesh = meshes_items->Get(ItemId);
			const auto local_id = local_ids->Get(ItemId);

			FFragmentItem* FoundFragmentItem = nullptr;
			if (!Wrapper->GetModelItem().FindFragmentByLocalId(local_id, FoundFragmentItem))
			{
				return FString();
			}

			GetItemData(FoundFragmentItem);

			const auto* global_transform = global_transforms->Get(mesh);
			FTransform GlobalTransform = UFragmentsUtils::MakeTransform(global_transform);
			FoundFragmentItem->GlobalTransform = GlobalTransform;

			for (int32 i = 0; i < ItemSamples.Num(); i++)
			{
				const Sample* sample = ItemSamples[i];
				const auto* material = materials->Get(sample->material());
				const auto* representation = representations->Get(sample->representation());
				const auto* local_transform = local_tranforms->Get(sample->local_transform());

				FFragmentSample SampleInfo;
				SampleInfo.SampleIndex = i;
				SampleInfo.LocalTransformIndex = sample->local_transform();
				SampleInfo.RepresentationIndex = sample->representation();
				SampleInfo.MaterialIndex = sample->material();

				FoundFragmentItem->Samples.Add(SampleInfo);
			}

		}

		// Pre-extract all geometry data from FlatBuffers at load time
		// This eliminates FlatBuffer access during spawn phase and prevents crashes
		// when FlatBuffer pointers become invalid in the async/TileManager path
		PreExtractAllGeometry(Wrapper->GetModelItemRef(), _meshes);
	}
	ModelFragmentsMap.Add(ModelGuidStr, FFragmentLookup());

	return ModelGuidStr;
}

void UFragmentsImporter::ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh)
{
	UE_LOG(LogFragments, Log, TEXT("ProcessLoadedFragment START - ModelGuid: %s, Owner: %p"), *InModelGuid, InOwnerRef);

	// Check if model exists
	if (!FragmentModels.Contains(InModelGuid))
	{
		UE_LOG(LogFragments, Error, TEXT("ProcessLoadedFragment: Model not in FragmentModels!"));
		return;
	}

	if (!InOwnerRef) return;

	SetOwnerRef(InOwnerRef);

	UFragmentModelWrapper* Wrapper = *FragmentModels.Find(InModelGuid);
	const Model* ModelRef = Wrapper->GetParsedModel();

	BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
	BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));

	// Build fragment registry for per-sample visibility
	Wrapper->BuildFragmentRegistry(InModelGuid);
	UFragmentRegistry* Registry = Wrapper->GetFragmentRegistry();

	if (!Registry || !Registry->IsBuilt())
	{
		UE_LOG(LogFragments, Error, TEXT("ProcessLoadedFragment: Failed to build fragment registry for model %s"), *InModelGuid);
		return;
	}

	// Create tile manager for per-sample visibility streaming
	UFragmentTileManager* TileManager = NewObject<UFragmentTileManager>(this);
	TileManager->Initialize(InModelGuid, this);
	TileManager->InitializePerSampleVisibility(Registry);
	TileManagers.Add(InModelGuid, TileManager);

	UE_LOG(LogFragments, Log, TEXT("Per-sample visibility initialized for model: %s (%d fragments)"),
	       *InModelGuid, Registry->GetFragmentCount());

	// Start spawn processing timer if not already running
	UWorld* World = GetWorld();
	if (World && !World->GetTimerManager().IsTimerActive(SpawnChunkTimerHandle))
	{
		World->GetTimerManager().SetTimer(
			SpawnChunkTimerHandle,
			this,
			&UFragmentsImporter::ProcessAllTileManagerChunks,
			0.016f, // ~60 FPS
			true // Repeat
		);
	}

	UE_LOG(LogFragments, Log, TEXT("Tile-based streaming started for model: %s"), *InModelGuid);
}


TArray<int32> UFragmentsImporter::GetElementsByCategory(const FString& InCategory, const FString& ModelGuid)
{
	TArray<int32> LocalIds;

	if (FragmentModels.Contains(ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *FragmentModels.Find(ModelGuid);
		const Model* InModel = Wrapper->GetParsedModel();

		if (!InModel)
			return LocalIds;

		const auto* categories = InModel->categories();
		const auto* local_ids = InModel->local_ids();

		if (!categories || !local_ids)
			return LocalIds;

		for (flatbuffers::uoffset_t i = 0; i < categories->size(); i++)
		{
			const auto* category = categories->Get(i);
			FString CategoryName = UTF8_TO_TCHAR(category->c_str());

			if (CategoryName.Equals(InCategory, ESearchCase::IgnoreCase))
			{
				int32 LocalId = local_ids->Get(i);
				LocalIds.Add(LocalId);
			}
		}
	}
	return LocalIds;
}

void UFragmentsImporter::UnloadFragment(const FString& ModelGuid)
{
	if (FFragmentLookup* Lookup = ModelFragmentsMap.Find(ModelGuid))
	{
		for (TPair<int32, AFragment*> Obj : Lookup->Fragments)
		{
			Obj.Value->Destroy();
		}
		ModelFragmentsMap.Remove(ModelGuid);
	}

	if (UFragmentModelWrapper** WrapperPtr = FragmentModels.Find(ModelGuid))
	{
		UFragmentModelWrapper* Wrapper = *WrapperPtr;

		Wrapper = nullptr;
		FragmentModels.Remove(ModelGuid);
	}
}

void UFragmentsImporter::CollectPropertiesRecursive(
	const Model* InModel,
	int32 StartLocalId,
	TSet<int32>& Visited,
	TArray<FItemAttribute>& OutAttributes)
{
	if (!InModel || Visited.Contains(StartLocalId)) return;
	Visited.Add(StartLocalId);

	const auto* relations = InModel->relations();
	const auto* attributes = InModel->attributes();
	const auto* relations_items = InModel->relations_items();

	for (flatbuffers::uoffset_t i = 0; i < relations_items->size(); i++)
	{
		if (relations_items->Get(i) != StartLocalId) continue;

		const auto* Relation = relations->Get(i);
		if (!Relation || !Relation->data()) continue;

		for (flatbuffers::uoffset_t j = 0; j < Relation->data()->size(); j++)
		{
			const char* RawStr = Relation->data()->Get(j)->c_str();
			FString Cleaned = UTF8_TO_TCHAR(RawStr);

			TArray<FString> Tokens;
			Cleaned.Replace(TEXT("["), TEXT(""))
				.Replace(TEXT("]"), TEXT(""))
				.ParseIntoArray(Tokens, TEXT(","), true);

			if (Tokens.Num() < 2) continue;

			FString RelationName = Tokens[0].TrimStartAndEnd().Replace(TEXT("\""), TEXT(""));

			// Only allow property-related relations
			if (!(RelationName.Equals(TEXT("IsDefinedBy")) || RelationName.Equals(TEXT("HasProperties")) || RelationName.Equals(TEXT("DefinesType"))))
				continue;

			for (int32 k = 1; k < Tokens.Num(); ++k)
			{
				int32 RelatedLocalId = FCString::Atoi(*Tokens[k].TrimStartAndEnd());
				if (Visited.Contains(RelatedLocalId)) continue;

				// Try resolving RelatedLocalId to attribute
				flatbuffers::uoffset_t AttrIndex = UFragmentsUtils::GetIndexForLocalId(InModel, RelatedLocalId);
				if (AttrIndex != INDEX_NONE && attributes && AttrIndex < attributes->size())
				{
					const auto* Attr = attributes->Get(AttrIndex);
					if (Attr)
					{
						TArray<FItemAttribute> Props = UFragmentsUtils::ParseItemAttribute(Attr);
						OutAttributes.Append(Props);
					}
				}

				// Recurse
				CollectPropertiesRecursive(InModel, RelatedLocalId, Visited, OutAttributes);
			}
		}
	}
}


void UFragmentsImporter::SpawnStaticMesh(UStaticMesh* StaticMesh,const Transform* LocalTransform, const Transform* GlobalTransform, AActor* Owner, FName OptionalTag)
{
	if (!StaticMesh || !LocalTransform || !GlobalTransform || !Owner) return;


	// Convert Fragments transform → Unreal FTransform
	FVector Local(LocalTransform->position().x() * 100, LocalTransform->position().z() * 100, LocalTransform->position().y() * 100); // Fix z-up and Unreal units
	FVector XLocal(LocalTransform->x_direction().x(), LocalTransform->x_direction().z(), LocalTransform->x_direction().y());
	FVector YLocal(LocalTransform->y_direction().x(), LocalTransform->y_direction().z(), LocalTransform->y_direction().y());
	FVector ZLocal = FVector::CrossProduct(XLocal, YLocal);

	FVector Global(GlobalTransform->position().x() * 100, GlobalTransform->position().z() * 100, GlobalTransform->position().y() * 100); // Fix z-up and Unreal units
	FVector XGlobal(GlobalTransform->x_direction().x(), GlobalTransform->x_direction().z(), GlobalTransform->x_direction().y());
	FVector YGlobal(GlobalTransform->y_direction().x(), GlobalTransform->y_direction().z(), GlobalTransform->y_direction().y());
	FVector ZGlobal = FVector::CrossProduct(XGlobal, YGlobal);

	FVector Pos = Global + Local;

	FMatrix GlobalMatrix(XGlobal, YGlobal, ZGlobal, FVector::ZeroVector);  // Global rotation matrix
	FMatrix LocalMatrix(XLocal, YLocal, ZLocal, FVector::ZeroVector);  // Local rotation matrix

	// Debug log the rotation matrices
	//UE_LOG(LogTemp, Log, TEXT("GlobalMatrix: X(%s) Y(%s) Z(%s)"), *XGlobal.ToString(), *YGlobal.ToString(), *ZGlobal.ToString());
	//UE_LOG(LogTemp, Log, TEXT("LocalMatrix: X(%s) Y(%s) Z(%s)"), *XLocal.ToString(), *YLocal.ToString(), *ZLocal.ToString());


	FMatrix FinalRotationMatrix = LocalMatrix.Inverse() * GlobalMatrix;
	//FMatrix FinalRotationMatrix = LocalMatrix.GetTransposed() * GlobalMatrix;

	//UE_LOG(LogTemp, Log, TEXT("FinalRotationMatrix: %s"), *FinalRotationMatrix.ToString());


	FRotator Rot = FinalRotationMatrix.Rotator();
	FVector FinalScale = FVector(1.0f, 1.0f, 1.0f);

	FTransform Transform(Rot, Pos, FinalScale);

	// Step 1: Spawn StaticMeshActor
	AStaticMeshActor* MeshActor = Owner->GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform);

	if (MeshActor == nullptr)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to spawn StaticMeshActor."));
		return;
	}

	// Step 2: Set Mesh Actor's mobility to movable (so it can be adjusted in runtime)
	MeshActor->GetRootComponent()->SetMobility(EComponentMobility::Movable);

	// Step 3: Ensure the mesh component is available (create one if needed)
	UStaticMeshComponent* MeshComponent = MeshActor->GetStaticMeshComponent();
	if (!MeshComponent)
	{
		// If no mesh component exists, create and register one
		MeshComponent = NewObject<UStaticMeshComponent>(MeshActor);
		MeshComponent->SetupAttachment(MeshActor->GetRootComponent());
		MeshComponent->RegisterComponent();
	}

	// Step 4: Set the static mesh
	MeshComponent->SetStaticMesh(StaticMesh);

	// Step 5: Mark the actor as needing an update to refresh its mesh
	MeshActor->MarkComponentsRenderStateDirty();
	MeshActor->SetActorTransform(Transform); // Apply the final transform

	// Step 6: Save the StaticMesh used


	// Ensure mesh is added and visible
	MeshComponent->SetVisibility(true);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	// Add Optional tag
	MeshActor->Tags.Add(OptionalTag);
}

void UFragmentsImporter::SpawnFragmentModel(AFragment* InFragmentModel, AActor* InParent, const Meshes* MeshesRef, bool bSaveMeshes)
{
	if (!InFragmentModel || !InParent || !MeshesRef) return;

	// 1. Root Component
	USceneComponent* RootSceneComponent = NewObject<USceneComponent>(InFragmentModel);
	RootSceneComponent->RegisterComponent();
	InFragmentModel->SetRootComponent(RootSceneComponent);
	RootSceneComponent->SetMobility(EComponentMobility::Movable);

	// 2. Set Transform and info
	InFragmentModel->SetActorTransform(InFragmentModel->GetGlobalTransform());
	InFragmentModel->AttachToActor(InParent, FAttachmentTransformRules::KeepWorldTransform);

#if WITH_EDITOR
	if (!InFragmentModel->GetCategory().IsEmpty())
		InFragmentModel->SetActorLabel(InFragmentModel->GetCategory());
#endif

	// 3. Create Meshes If Sample Exists
	const TArray<FFragmentSample>& Samples = InFragmentModel->GetSamples();
	if (Samples.Num() > 0)
	{
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			const FFragmentSample& Sample = Samples[i];

			//FString PackagePath = FString::Printf(TEXT("/Game/Buildings/%s"), *InFragmentModel->GetModelGuid());
			FString MeshName = FString::Printf(TEXT("%d_%d"), InFragmentModel->GetLocalId(), i);
			FString PackagePath = TEXT("/Game/Buildings") / InFragmentModel->GetModelGuid()/ MeshName;
			const FString SamplePath = PackagePath + TEXT(".") + MeshName;

			FString UniquePackageName = FPackageName::ObjectPathToPackageName(PackagePath);
			FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());

			const Material* material = MeshesRef->materials()->Get(Sample.MaterialIndex);
			const Representation* representation = MeshesRef->representations()->Get(Sample.RepresentationIndex);
			const Transform* local_transform = MeshesRef->local_transforms()->Get(Sample.LocalTransformIndex);

			FTransform LocalTransform = UFragmentsUtils::MakeTransform(local_transform);

			UStaticMesh* Mesh = nullptr;
			//FString MeshName = FString::Printf(TEXT("sample_%d_%d"), InFragmentModel->GetLocalId(), i);
			if (MeshCache.Contains(SamplePath))
			{
				Mesh = MeshCache[SamplePath];
			}
			else if (FPaths::FileExists(PackageFileName))
			{
				UPackage* ExistingPackage = LoadPackage(nullptr, *PackagePath, LOAD_None);
				//UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshObjectPath));
				if (ExistingPackage)
				{
					Mesh = FindObject<UStaticMesh>(ExistingPackage, *MeshName);
				}
			}
			else
			{
				UPackage* MeshPackage = CreatePackage(*PackagePath);
				if (representation->representation_class() == RepresentationClass::RepresentationClass_SHELL)
				{
					const auto* shell = MeshesRef->shells()->Get(representation->id());
					Mesh = CreateStaticMeshFromShell(shell, material, *MeshName, MeshPackage);

				}
				else if (representation->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
				{
					const auto* circleExtrusion = MeshesRef->circle_extrusions()->Get(representation->id());
					Mesh = CreateStaticMeshFromCircleExtrusion(circleExtrusion, material, *MeshName, MeshPackage);
				}

				if (Mesh)
				{
					if (!FPaths::FileExists(PackageFileName) && bSaveMeshes)
					{
#if WITH_EDITOR
						MeshPackage->FullyLoad();

						Mesh->Rename(*MeshName, MeshPackage);
						Mesh->SetFlags(RF_Public | RF_Standalone);
						//Mesh->Build();
						MeshPackage->MarkPackageDirty();
						FAssetRegistryModule::AssetCreated(Mesh);

						FSavePackageArgs SaveArgs;
						SaveArgs.SaveFlags = RF_Public | RF_Standalone;

						PackagesToSave.Add(MeshPackage);
#endif
						//UPackage::SavePackage(MeshPackage, Mesh, *PackageFileName, SaveArgs);
					}
				}

				MeshCache.Add(SamplePath, Mesh);
			}

			if (Mesh)
			{

				// Add StaticMeshComponent to parent actor
				UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(InFragmentModel);
				MeshComp->SetStaticMesh(Mesh);
				MeshComp->SetRelativeTransform(LocalTransform); // local to parent
				MeshComp->AttachToComponent(RootSceneComponent, FAttachmentTransformRules::KeepRelativeTransform);
				MeshComp->RegisterComponent();
				InFragmentModel->AddInstanceComponent(MeshComp);
			}
		}
	}

	// 4. Recursively spawn child fragments
	for (AFragment* Child : InFragmentModel->GetChildren())
	{
		SpawnFragmentModel(Child, InFragmentModel, MeshesRef, bSaveMeshes);
	}
}

void UFragmentsImporter::ExtractShellGeometry(
	const Shell* ShellRef,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs)
{
	if (!ShellRef || !ShellRef->points())
	{
		return;
	}

	const auto* Points = ShellRef->points();

	// Extract vertices (convert from FlatBuffers FloatVector to FVector)
	for (flatbuffers::uoffset_t i = 0; i < Points->size(); i++)
	{
		const auto& P = *Points->Get(i);
		// Convert: m to cm (×100), swap Y and Z for Unreal coordinate system
		FVector Vertex(P.x() * 100.0f, P.z() * 100.0f, P.y() * 100.0f);
		OutVertices.Add(Vertex);
	}

	// Build hole map (profile_id → hole indices)
	const auto* Holes = ShellRef->holes();
	TMap<int32, TArray<TArray<int32>>> ProfileHolesMap;

	if (Holes)
	{
		for (flatbuffers::uoffset_t j = 0; j < Holes->size(); j++)
		{
			const auto* Hole = Holes->Get(j);
			const auto* HoleIndices = Hole->indices();
			const auto Profile_id = Hole->profile_id();

			TArray<int32> HoleIdx;
			for (flatbuffers::uoffset_t k = 0; k < HoleIndices->size(); k++)
			{
				HoleIdx.Add(HoleIndices->Get(k));
			}

			if (ProfileHolesMap.Contains(Profile_id))
			{
				ProfileHolesMap[Profile_id].Add(HoleIdx);
			}
			else
			{
				TArray<TArray<int32>> HolesForProfile;
				HolesForProfile.Add(HoleIdx);
				ProfileHolesMap.Add(Profile_id, HolesForProfile);
			}
		}
	}

	// Process profiles
	const auto* Profiles = ShellRef->profiles();
	if (!Profiles)
	{
		return;
	}

	for (flatbuffers::uoffset_t i = 0; i < Profiles->size(); i++)
	{
		const ShellProfile* Profile = Profiles->Get(i);
		const auto* Indices = Profile->indices();

		if (!Indices || Indices->size() < 3)
		{
			continue;
		}

		// Check if this profile has holes
		bool bHasHoles = ProfileHolesMap.Contains(i);

		if (bHasHoles)
		{
			// Extract contour indices
			TArray<int32> ContourIndices;
			for (flatbuffers::uoffset_t j = 0; j < Indices->size(); j++)
			{
				ContourIndices.Add(Indices->Get(j));
			}

			// Triangulate polygon with holes
			// This creates NEW vertices and indices (not reusing OutVertices!)
			TArray<FVector> TriangulatedVertices;
			TArray<int32> TriangulatedIndices;

			if (TriangulatePolygonWithHoles(
				OutVertices,           // All vertices (for lookup)
				ContourIndices,        // Contour as indices
				ProfileHolesMap[i],    // Holes
				TriangulatedVertices,  // Output: new vertices
				TriangulatedIndices    // Output: new indices
			))
			{
				// Add triangulated vertices to output
				int32 VertexOffset = OutVertices.Num();
				OutVertices.Append(TriangulatedVertices);

				// Adjust indices and add to output
				for (int32 Idx : TriangulatedIndices)
				{
					OutTriangles.Add(VertexOffset + Idx);
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Triangulation failed for profile %d"), i);
			}
		}
		else
		{
			// Simple fan triangulation for convex polygons (no holes)
			if (Indices->size() >= 3)
			{
				int32 V0 = Indices->Get(0);

				for (flatbuffers::uoffset_t j = 1; j < Indices->size() - 1; j++)
				{
					int32 V1 = Indices->Get(j);
					int32 V2 = Indices->Get(j + 1);

					OutTriangles.Add(V0);
					OutTriangles.Add(V1);
					OutTriangles.Add(V2);
				}
			}
		}
	}

	// Calculate normals (per-face)
	OutNormals.SetNum(OutVertices.Num());
	for (int32 i = 0; i < OutTriangles.Num(); i += 3)
	{
		if (i + 2 < OutTriangles.Num())
		{
			int32 Idx0 = OutTriangles[i + 0];
			int32 Idx1 = OutTriangles[i + 1];
			int32 Idx2 = OutTriangles[i + 2];

			if (Idx0 < OutVertices.Num() && Idx1 < OutVertices.Num() && Idx2 < OutVertices.Num())
			{
				FVector V0 = OutVertices[Idx0];
				FVector V1 = OutVertices[Idx1];
				FVector V2 = OutVertices[Idx2];

				FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();

				OutNormals[Idx0] = Normal;
				OutNormals[Idx1] = Normal;
				OutNormals[Idx2] = Normal;
			}
		}
	}

	// Simple planar UV projection
	OutUVs.SetNum(OutVertices.Num());
	for (int32 i = 0; i < OutVertices.Num(); i++)
	{
		OutUVs[i] = FVector2D(OutVertices[i].X * 0.01f, OutVertices[i].Y * 0.01f);
	}
}

void UFragmentsImporter::ExtractCircleExtrusionGeometry(
	const CircleExtrusion* ExtrusionRef,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutTriangles,
	TArray<FVector>& OutNormals,
	TArray<FVector2D>& OutUVs)
{
	// For now, we'll skip circle extrusion deduplication
	// and fall back to the existing CreateStaticMeshFromCircleExtrusion
	// TODO: Implement full extraction in future optimization

	UE_LOG(LogTemp, Warning, TEXT("Circle extrusion geometry extraction not yet implemented for deduplication"));

	// Leave arrays empty - the calling code will skip if Vertices.Num() == 0
}

void UFragmentsImporter::SpawnFragmentModel(FFragmentItem InFragmentItem, AActor* InParent, const Meshes* MeshesRef, bool bSaveMeshes)
{
	UE_LOG(LogFragments, Log, TEXT("SpawnFragmentModel Start - In Parent: %p, OwnerRef: %p"), InParent, OwnerRef);

	if (!InParent)
	{
		UE_LOG(LogFragments, Error, TEXT("SpawnFragmentModel: InParent is NULL! Early return. "));
		return;
	}

	// Create AFragment

	AFragment* FragmentModel = OwnerRef->GetWorld()->SpawnActor<AFragment>(
		AFragment::StaticClass(), InFragmentItem.GlobalTransform);

	if (!FragmentModel)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to spawn FragmentModel actor!"));
		return;
	}

	UE_LOG(LogFragments, Log, TEXT("Spawned FragmentModel: %s at %s"), *FragmentModel->GetName(),*InFragmentItem.GlobalTransform.ToString());
	

	// Root Component
	USceneComponent* RootSceneComponent = NewObject<USceneComponent>(FragmentModel);
	RootSceneComponent->RegisterComponent();
	FragmentModel->SetRootComponent(RootSceneComponent);
	RootSceneComponent->SetMobility(EComponentMobility::Movable);

	// Set Transform and Info
	//FragmentModel->SetActorTransform(InFragmentItem.GlobalTransform);
	FragmentModel->SetData(InFragmentItem);
	FragmentModel->AttachToActor(InParent, FAttachmentTransformRules::KeepWorldTransform);

#if WITH_EDITOR
	if (!FragmentModel->GetCategory().IsEmpty())
		FragmentModel->SetActorLabel(FragmentModel->GetCategory());
#endif

	// Create Meshes If Sample Exists
	const TArray<FFragmentSample>& Samples = FragmentModel->GetSamples();
	UE_LOG(LogFragments, Log, TEXT("Processing %d samples for FragmentModel"), Samples.Num());

	if (Samples.Num() > 0)
	{
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			const FFragmentSample& Sample = Samples[i];

			//FString PackagePath = FString::Printf(TEXT("/Game/Buildings/%s"), *InFragmentModel->GetModelGuid());
			FString MeshName = FString::Printf(TEXT("%d_%d"), FragmentModel->GetLocalId(), i);
			FString PackagePath = TEXT("/Game/Buildings") / FragmentModel->GetModelGuid() / MeshName;
			const FString SamplePath = PackagePath + TEXT(".") + MeshName;

			FString UniquePackageName = FPackageName::ObjectPathToPackageName(PackagePath);
			FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());

			const Material* material = MeshesRef->materials()->Get(Sample.MaterialIndex);
			const Representation* representation = MeshesRef->representations()->Get(Sample.RepresentationIndex);
			const Transform* local_transform = MeshesRef->local_transforms()->Get(Sample.LocalTransformIndex);

			FTransform LocalTransform = UFragmentsUtils::MakeTransform(local_transform);

			UStaticMesh* Mesh = nullptr;
			//FString MeshName = FString::Printf(TEXT("sample_%d_%d"), InFragmentModel->GetLocalId(), i);
			if (MeshCache.Contains(SamplePath))
			{
				Mesh = MeshCache[SamplePath];
			}
			else if (FPaths::FileExists(PackageFileName))
			{
				UPackage* ExistingPackage = LoadPackage(nullptr, *PackagePath, LOAD_None);
				//UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshObjectPath));
				if (ExistingPackage)
				{
					Mesh = FindObject<UStaticMesh>(ExistingPackage, *MeshName);
				}
			}
			else
			{
				// Create package first (needed for both deduplication and fallback)
				UPackage* MeshPackage = CreatePackage(*PackagePath);

				// Extract geometry data first
				TArray<FVector> Vertices;
				TArray<int32> Triangles;
				TArray<FVector> Normals;
				TArray<FVector2D> UVs;

				if (representation->representation_class() == RepresentationClass::RepresentationClass_SHELL)
				{
					const auto* shell = MeshesRef->shells()->Get(representation->id());
					ExtractShellGeometry(shell, Vertices, Triangles, Normals, UVs);
				}
				else if (representation->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
				{
					const auto* circleExtrusion = MeshesRef->circle_extrusions()->Get(representation->id());
					ExtractCircleExtrusionGeometry(circleExtrusion, Vertices, Triangles, Normals, UVs);
				}

				if (Vertices.Num() > 0 && Triangles.Num() > 0)
				{
					// Use deduplication manager

					FGeometryTemplate* Template = DeduplicationManager->GetOrCreateTemplate(
						Vertices,
						Triangles,
						Normals,
						UVs,
						Sample.MaterialIndex,
						*MeshName,
						MeshPackage
					);

					if (Template)
					{
						// Check if this is a newly created template (ReferenceCount will be 0)
						bool bIsNewTemplate = (Template->ReferenceCount == 0);

						//Add this instance to the template
						DeduplicationManager->AddInstance(
							Template->GeometryHash,
							LocalTransform,
							FragmentModel->GetLocalId(),
							Sample.MaterialIndex
						);
						Mesh = Template->SharedMesh;

						// Apply material to newly created mesh
						if (bIsNewTemplate && Mesh)
						{
							AddMaterialToMesh(Mesh, material);
						}

						// Only save the package if this is a newly created mesh
						if (bIsNewTemplate && Mesh && !FPaths::FileExists(PackageFileName) && bSaveMeshes)
						{
#if WITH_EDITOR
							MeshPackage->FullyLoad();
							Mesh->Rename(*MeshName, MeshPackage);
							Mesh->SetFlags(RF_Public | RF_Standalone);
							MeshPackage->MarkPackageDirty();
							FAssetRegistryModule::AssetCreated(Mesh);
							PackagesToSave.Add(MeshPackage);
#endif
						}

						UE_LOG(LogTemp, Verbose, TEXT("Using deduplicated mesh (hash: %llu, instances: %d, new: %d)"),
							Template->GeometryHash, Template->ReferenceCount, bIsNewTemplate ? 1 : 0);
					}
				}

				MeshCache.Add(SamplePath, Mesh);
			}

			if (Mesh)
			{

				// Add StaticMeshComponent to parent actor
				UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(FragmentModel);
				MeshComp->SetStaticMesh(Mesh);
				MeshComp->SetRelativeTransform(LocalTransform); // local to parent
				MeshComp->AttachToComponent(RootSceneComponent, FAttachmentTransformRules::KeepRelativeTransform);
				MeshComp->RegisterComponent();
				FragmentModel->AddInstanceComponent(MeshComp);
			}
		}
	}

	if (ModelFragmentsMap.Contains(InFragmentItem.ModelGuid))
	{
		ModelFragmentsMap[InFragmentItem.ModelGuid].Fragments.Add(InFragmentItem.LocalId, FragmentModel);
	}

	// Recursively spawn child fragments
	for (FFragmentItem* Child : InFragmentItem.FragmentChildren)
	{
		SpawnFragmentModel(*Child, FragmentModel, MeshesRef, bSaveMeshes);
	}
}

void UFragmentsImporter::BuildSpawnQueue(const FFragmentItem& Item, AActor* ParentActor, TArray<FFragmentSpawnTask>& OutQueue)
{
	// Add this fragmet to the queue
	OutQueue.Add(FFragmentSpawnTask(Item, ParentActor));

	// Note: We don't recursively add children here yet
	// We'll handle parent-child relationships during spawning
}

AFragment* UFragmentsImporter::SpawnSingleFragment(const FFragmentItem& Item, AActor* ParentActor, const Meshes* MeshesRef, bool bSaveMeshes, bool* bOutWasInstanced)
{
	// Initialize output parameter
	if (bOutWasInstanced)
	{
		*bOutWasInstanced = false;
	}

	if (!ParentActor) return nullptr;

	const TArray<FFragmentSample>& Samples = Item.Samples;

	// ==========================================
	// GPU INSTANCING: Check if ALL samples should be instanced
	// If so, we can skip actor creation entirely and just use a proxy
	// ==========================================
	bool bAllSamplesInstanced = bEnableGPUInstancing && (Samples.Num() > 0);
	int32 ValidSampleCount = 0;

	if (bEnableGPUInstancing)
	{
		for (const FFragmentSample& Sample : Samples)
		{
			if (!Sample.ExtractedGeometry.bIsValid) continue;
			ValidSampleCount++;

			// Only Shell geometry supports instancing currently
			if (!Sample.ExtractedGeometry.bIsShell)
			{
				bAllSamplesInstanced = false;
				break;
			}

			const int32 RepId = Sample.RepresentationIndex;
			const FPreExtractedGeometry& Geom = Sample.ExtractedGeometry;
			const uint32 MatHash = HashMaterialProperties(Geom.R, Geom.G, Geom.B, Geom.A, Geom.bIsGlass);

			if (!ShouldUseInstancing(RepId, MatHash))
			{
				bAllSamplesInstanced = false;
				break;
			}
		}

		// If no valid samples, don't treat as all-instanced
		if (ValidSampleCount == 0)
		{
			bAllSamplesInstanced = false;
		}
	}

	// ==========================================
	// FULLY INSTANCED PATH: Queue for batch addition (no ISMC created yet)
	// Actual ISMC creation happens in FinalizeAllISMCs() after spawning completes
	// ==========================================
	if (bAllSamplesInstanced)
	{
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			const FFragmentSample& Sample = Samples[i];
			const FPreExtractedGeometry& ExtractedGeom = Sample.ExtractedGeometry;

			if (!ExtractedGeom.bIsValid) continue;

			const int32 RepId = Sample.RepresentationIndex;
			const uint32 MatHash = HashMaterialProperties(ExtractedGeom.R, ExtractedGeom.G,
				ExtractedGeom.B, ExtractedGeom.A, ExtractedGeom.bIsGlass);

			// Get or create mesh from representation cache
			UStaticMesh* Mesh = nullptr;
			if (UStaticMesh** CachedMesh = RepresentationMeshCache.Find(RepId))
			{
				Mesh = *CachedMesh;
			}
			else
			{
				// Create new mesh
				FString MeshName = FString::Printf(TEXT("Rep_%d"), RepId);
				UPackage* MeshPackage = CreatePackage(*FString::Printf(TEXT("/Game/Buildings/Instanced/%s"), *MeshName));
				Mesh = CreateStaticMeshFromPreExtractedShell(ExtractedGeom, MeshName, MeshPackage);
				if (Mesh)
				{
					RepresentationMeshCache.Add(RepId, Mesh);
					UE_LOG(LogFragments, Verbose, TEXT("GPU Instancing: Created mesh for RepId %d"), RepId);
				}
			}

			if (!Mesh) continue;

			// Get pooled material
			UMaterialInstanceDynamic* Material = GetPooledMaterial(ExtractedGeom.R, ExtractedGeom.G,
				ExtractedGeom.B, ExtractedGeom.A, ExtractedGeom.bIsGlass);

			// Compute world transform: LocalTransform * GlobalTransform
			FTransform SampleWorldTransform = ExtractedGeom.LocalTransform * Item.GlobalTransform;

			// QUEUE instance for batch addition (no ISMC created yet!)
			// Proxies will be created in FinalizeAllISMCs()
			QueueInstanceForBatchAdd(RepId, MatHash, SampleWorldTransform, Item, Mesh, Material);
		}

		// Store null in actor lookup map to indicate this fragment exists but is instanced
		if (ModelFragmentsMap.Contains(Item.ModelGuid))
		{
			ModelFragmentsMap[Item.ModelGuid].Fragments.Add(Item.LocalId, nullptr);
		}

		// Set output flag to indicate fragment was GPU instanced (no actor, but handled)
		if (bOutWasInstanced)
		{
			*bOutWasInstanced = true;
		}

		// Return nullptr since no actor was created
		return nullptr;
	}

	// ==========================================
	// STANDARD PATH: Create AFragment actor
	// ==========================================
	AFragment* FragmentModel = OwnerRef->GetWorld()->SpawnActor<AFragment>(
		AFragment::StaticClass(), Item.GlobalTransform);

	if (!FragmentModel)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to spawn FragmentModel actor!"));
		return nullptr;
	}

	// Root Component
	USceneComponent* RootSceneComponent = NewObject<USceneComponent>(FragmentModel);
	RootSceneComponent->RegisterComponent();
	FragmentModel->SetRootComponent(RootSceneComponent);
	RootSceneComponent->SetMobility(EComponentMobility::Movable);

	// Set Transform and Info
	FragmentModel->SetData(Item);
	FragmentModel->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform);

#if WITH_EDITOR
	if (!FragmentModel->GetCategory().IsEmpty())
		FragmentModel->SetActorLabel(FragmentModel->GetCategory());
#endif

	// Create Meshes If Sample Exists
	const TArray<FFragmentSample>& ActorSamples = FragmentModel->GetSamples();

	if (ActorSamples.Num() > 0)
	{
		for (int32 i = 0; i < ActorSamples.Num(); i++)
		{
			const FFragmentSample& Sample = ActorSamples[i];
			const FPreExtractedGeometry& ExtractedGeom = Sample.ExtractedGeometry;

			// Skip samples with invalid pre-extracted geometry
			if (!ExtractedGeom.bIsValid)
			{
				UE_LOG(LogFragments, Verbose, TEXT("SpawnSingleFragment: Skipping sample %d with invalid geometry (LocalId: %d)"),
					i, FragmentModel->GetLocalId());
				continue;
			}

			const int32 RepId = Sample.RepresentationIndex;
			const uint32 MatHash = HashMaterialProperties(ExtractedGeom.R, ExtractedGeom.G,
				ExtractedGeom.B, ExtractedGeom.A, ExtractedGeom.bIsGlass);

			// ==========================================
			// PER-SAMPLE INSTANCING CHECK (for mixed fragments)
			// Queue for batch addition instead of immediate ISMC creation
			// ==========================================
			if (bEnableGPUInstancing && ExtractedGeom.bIsShell && ShouldUseInstancing(RepId, MatHash))
			{
				// This sample goes to an ISMC instead of a component

				// Get or create mesh
				UStaticMesh* Mesh = nullptr;
				if (UStaticMesh** CachedMesh = RepresentationMeshCache.Find(RepId))
				{
					Mesh = *CachedMesh;
				}
				else
				{
					FString MeshName = FString::Printf(TEXT("Rep_%d"), RepId);
					UPackage* MeshPackage = CreatePackage(*FString::Printf(TEXT("/Game/Buildings/Instanced/%s"), *MeshName));
					Mesh = CreateStaticMeshFromPreExtractedShell(ExtractedGeom, MeshName, MeshPackage);
					if (Mesh)
					{
						RepresentationMeshCache.Add(RepId, Mesh);
					}
				}

				if (Mesh)
				{
					UMaterialInstanceDynamic* Material = GetPooledMaterial(ExtractedGeom.R, ExtractedGeom.G,
						ExtractedGeom.B, ExtractedGeom.A, ExtractedGeom.bIsGlass);

					FTransform SampleWorldTransform = ExtractedGeom.LocalTransform * Item.GlobalTransform;

					// QUEUE for batch addition (ISMCs created in FinalizeAllISMCs)
					QueueInstanceForBatchAdd(RepId, MatHash, SampleWorldTransform, Item, Mesh, Material);
				}

				continue;  // Skip standard component creation for this sample
			}

			// ==========================================
			// STANDARD COMPONENT CREATION PATH
			// ==========================================
			FString MeshName = FString::Printf(TEXT("%d_%d"), FragmentModel->GetLocalId(), i);
			FString PackagePath = TEXT("/Game/Buildings") / FragmentModel->GetModelGuid() / MeshName;
			const FString SamplePath = PackagePath + TEXT(".") + MeshName;

			FString UniquePackageName = FPackageName::ObjectPathToPackageName(PackagePath);
			FString PackageFileName = FPackageName::LongPackageNameToFilename(UniquePackageName, FPackageName::GetAssetPackageExtension());

			// Use pre-extracted local transform instead of FlatBuffer access
			FTransform LocalTransform = ExtractedGeom.LocalTransform;

			UStaticMesh* Mesh = nullptr;

			// Check mesh cache first
			if (MeshCache.Contains(SamplePath))
			{
				Mesh = MeshCache[SamplePath];
			}
			// Check if mesh exists on disk
			else if (FPaths::FileExists(PackageFileName))
			{
				UPackage* ExistingPackage = LoadPackage(nullptr, *PackagePath, LOAD_None);
				if (ExistingPackage)
				{
					Mesh = FindObject<UStaticMesh>(ExistingPackage, *MeshName);
				}
			}
			// Create new mesh from pre-extracted geometry (NO FLATBUFFER ACCESS)
			else
			{
				UPackage* MeshPackage = CreatePackage(*PackagePath);

				if (ExtractedGeom.bIsShell)
				{
					// Use RepresentationId-based caching (more reliable than geometry hashing)
					// All instances with the same RepresentationId share identical geometry
					const int32 RepresentationId = Sample.RepresentationIndex;

					if (UStaticMesh** CachedMesh = RepresentationMeshCache.Find(RepresentationId))
					{
						// Reuse existing mesh for this representation
						Mesh = *CachedMesh;
						UE_LOG(LogFragments, Verbose, TEXT("SpawnSingleFragment: Reusing cached mesh for RepId %d (LocalId: %d)"),
							RepresentationId, FragmentModel->GetLocalId());
					}
					else
					{
						// Create new mesh using the working pre-extracted shell function
						Mesh = CreateStaticMeshFromPreExtractedShell(ExtractedGeom, MeshName, MeshPackage);

						if (Mesh)
						{
							// Cache by RepresentationId for future instances
							RepresentationMeshCache.Add(RepresentationId, Mesh);

							// Save mesh if needed
							if (!FPaths::FileExists(PackageFileName) && bSaveMeshes)
							{
#if WITH_EDITOR
								MeshPackage->FullyLoad();
								Mesh->Rename(*MeshName, MeshPackage);
								Mesh->SetFlags(RF_Public | RF_Standalone);
								MeshPackage->MarkPackageDirty();
								FAssetRegistryModule::AssetCreated(Mesh);
								PackagesToSave.Add(MeshPackage);
#endif
							}

							UE_LOG(LogFragments, Log, TEXT("SpawnSingleFragment: Created and cached mesh for RepId %d (LocalId: %d)"),
								RepresentationId, FragmentModel->GetLocalId());
						}
					}
				}
				else
				{
					// CircleExtrusion: still use FlatBuffer path (not pre-extracted)
					// This is acceptable because CircleExtrusion works correctly with FlatBuffer access
					if (MeshesRef && MeshesRef->representations() && MeshesRef->circle_extrusions())
					{
						const uint32 RepCount = MeshesRef->representations()->size();
						if (static_cast<uint32>(Sample.RepresentationIndex) < RepCount)
						{
							const Representation* representation = MeshesRef->representations()->Get(Sample.RepresentationIndex);
							if (representation && representation->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
							{
								const uint32 ExtrusionId = representation->id();
								if (ExtrusionId < MeshesRef->circle_extrusions()->size())
								{
									const auto* circleExtrusion = MeshesRef->circle_extrusions()->Get(ExtrusionId);

									// Get material from FlatBuffer for CircleExtrusion (still needed)
									const Material* material = nullptr;
									if (MeshesRef->materials() && static_cast<uint32>(Sample.MaterialIndex) < MeshesRef->materials()->size())
									{
										material = MeshesRef->materials()->Get(Sample.MaterialIndex);
									}

									Mesh = CreateStaticMeshFromCircleExtrusion(circleExtrusion, material, *MeshName, MeshPackage);
								}
							}
						}
					}

					// Save CircleExtrusion mesh if needed
					if (Mesh && !FPaths::FileExists(PackageFileName) && bSaveMeshes)
					{
#if WITH_EDITOR
						MeshPackage->FullyLoad();
						Mesh->Rename(*MeshName, MeshPackage);
						Mesh->SetFlags(RF_Public | RF_Standalone);
						MeshPackage->MarkPackageDirty();
						FAssetRegistryModule::AssetCreated(Mesh);
						PackagesToSave.Add(MeshPackage);
#endif
					}
				}

				if (Mesh)
				{
					MeshCache.Add(SamplePath, Mesh);
				}
			}

			if (Mesh)
			{
				// Add StaticMeshComponent to parent actor
				UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(FragmentModel);
				MeshComp->SetStaticMesh(Mesh);
				MeshComp->SetRelativeTransform(LocalTransform);
				MeshComp->AttachToComponent(RootSceneComponent, FAttachmentTransformRules::KeepRelativeTransform);

				// Disable Lumen/Distance Field features to avoid "Preparing mesh distance fields/cards" delays
				// These are expensive to compute at runtime for procedurally generated meshes
				MeshComp->bAffectDistanceFieldLighting = false;  // Skip distance field generation
				MeshComp->bAffectDynamicIndirectLighting = false; // Skip Lumen indirect lighting
				MeshComp->bAffectIndirectLightingWhileHidden = false;

				MeshComp->RegisterComponent();
				FragmentModel->AddInstanceComponent(MeshComp);

				// Configure occlusion culling based on fragment classification
				// Use pre-extracted material alpha instead of FlatBuffer access
				const EOcclusionRole Role = UFragmentOcclusionClassifier::ClassifyFragment(
					Item.Category, ExtractedGeom.A);

				switch (Role)
				{
				case EOcclusionRole::Occluder:
					// Large structural elements that block visibility
					MeshComp->bUseAsOccluder = true;
					MeshComp->SetCastShadow(true);
					break;

				case EOcclusionRole::Occludee:
					// Objects that can be hidden by occluders
					MeshComp->bUseAsOccluder = false;
					MeshComp->SetCastShadow(true);
					break;

				case EOcclusionRole::NonOccluder:
					// Glass/transparent - doesn't block anything
					MeshComp->bUseAsOccluder = false;
					MeshComp->SetCastShadow(false);
					break;
				}
			}
		}
	}

	// Store in lookup map
	if (ModelFragmentsMap.Contains(Item.ModelGuid))
	{
		ModelFragmentsMap[Item.ModelGuid].Fragments.Add(Item.LocalId, FragmentModel);
	}

	// NOTE: No recursive child spawning here - handled by chunking system

	return FragmentModel;
}

void UFragmentsImporter::ProcessSpawnChunk()
{
	// Process any completed async geometry work within frame budget
	ProcessCompletedGeometry();

	// Check if we still have pending async work
	const bool bHasPendingAsyncWork = GeometryWorkerPool.IsValid() &&
		(GeometryWorkerPool->GetPendingWorkCount() > 0 || PendingFragmentMap.Num() > 0);

	if (PendingSpawnQueue.Num() == 0)
	{
		// All fragments spawned, but check if async geometry is still being processed
		if (bHasPendingAsyncWork)
		{
			// Keep timer running to process remaining async work
			UE_LOG(LogFragments, Verbose, TEXT("Spawn queue empty, waiting for %d async geometry items"),
				PendingFragmentMap.Num());
			return;
		}

		// Spawning and async processing complete
		UE_LOG(LogFragments, Log, TEXT("Chunked spawning complete! Total fragments: %d"), FragmentsSpawned);

		// FINALIZE ALL ISMCs - batch-add all queued instances
		// This is the key performance optimization: all instances are added at once
		// instead of one-at-a-time which causes UE5 GPU buffer rebuilds
		FinalizeAllISMCs();

		// Clear timer
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(SpawnChunkTimerHandle);
		}

		// Save meshes if needed
		if (PackagesToSave.Num() > 0)
		{
			DeferredSaveManager.AddPackagesToSave(PackagesToSave);
			PackagesToSave.Empty();
		}

		//Notify Completion
		PendingCallback.ExecuteIfBound(true, TEXT(""), CurrentSpawningModelGuid);
		SpawnProgress = 1.0f;
		return;
	}

	// Process a chunk
	int32 ChunkSize = FMath::Min(FragmentsPerChunk, PendingSpawnQueue.Num());

	for (int32 i = 0; i < ChunkSize; i++)
	{
		FFragmentSpawnTask Task = PendingSpawnQueue[0];
		PendingSpawnQueue.RemoveAt(0);

		// Spawn this fragment
		bool bWasInstanced = false;
		AFragment* SpawnedActor = SpawnSingleFragment(Task.FragmentItem, Task.ParentActor, CurrentMeshesRef, bCurrentSaveMeshes, &bWasInstanced);

		if (SpawnedActor)
		{
			// Add children to queue with this actor as parent
			for (FFragmentItem* Child : Task.FragmentItem.FragmentChildren)
			{
				PendingSpawnQueue.Add(FFragmentSpawnTask(*Child, SpawnedActor));
				TotalFragmentsToSpawn++;
			}
		}
		else if (bWasInstanced && Task.FragmentItem.FragmentChildren.Num() > 0)
		{
			// Fragment was GPU instanced but has children - add them with original parent
			// (This is rare for BIM models since instanced elements are usually leaf nodes)
			for (FFragmentItem* Child : Task.FragmentItem.FragmentChildren)
			{
				PendingSpawnQueue.Add(FFragmentSpawnTask(*Child, Task.ParentActor));
				TotalFragmentsToSpawn++;
			}
		}

		FragmentsSpawned++;

	}

	// Update Progress
	SpawnProgress = (float)FragmentsSpawned / (float)FMath::Max(TotalFragmentsToSpawn, 1);

	UE_LOG(LogFragments, Log, TEXT("Spawn progress: %d/%d (%.1f%%)"), FragmentsPerChunk, TotalFragmentsToSpawn, SpawnProgress * 100.0f);
}

void UFragmentsImporter::ProcessAllTileManagerChunks()
{
	// First, process any completed async geometry work within frame budget
	ProcessCompletedGeometry();

	// Process spawn chunks for all tile managers (per-sample visibility only)
	for (auto& Pair : TileManagers)
	{
		UFragmentTileManager* TileManager = Pair.Value;
		if (!TileManager)
		{
			continue;
		}

		// Per-sample mode: process spawning based on dynamic tiles
		TileManager->ProcessSpawnChunk();
	}

	// Check if all tile managers are idle
	bool bAnyLoading = false;
	for (auto& Pair : TileManagers)
	{
		if (Pair.Value && Pair.Value->IsLoading())
		{
			bAnyLoading = true;
			break;
		}
	}

	// If no tile managers are loading, we can stop the timer (it will restart when streaming updates)
	if (!bAnyLoading && TileManagers.Num() > 0)
	{
		UE_LOG(LogFragments, Verbose, TEXT("All tile managers idle, timer continues for streaming updates"));
	}
}

void UFragmentsImporter::UpdateTileStreaming(const FVector& CameraLocation, const FRotator& CameraRotation,
                                              float FOV, float AspectRatio, float ViewportHeight)
{
	// Update all tile managers with current camera (per-sample visibility only)
	for (auto& Pair : TileManagers)
	{
		UFragmentTileManager* TileManager = Pair.Value;
		if (TileManager)
		{
			TileManager->UpdateVisibleTiles(CameraLocation, CameraRotation, FOV, AspectRatio, ViewportHeight);
		}
	}
}

void UFragmentsImporter::StartChunkedSpawning(const FFragmentItem& RootItem, AActor* OwnerActor, const Meshes* MeshesRef, bool bSaveMeshes)
{
	UE_LOG(LogFragments, Log, TEXT("Starting chunked spawning"));

	// Reset State
	PendingSpawnQueue.Empty();
	FragmentsSpawned = 0;
	TotalFragmentsToSpawn = 1; //Start with root
	SpawnProgress = 0.0f;

	// Store references
	CurrentMeshesRef = MeshesRef;
	bCurrentSaveMeshes = bSaveMeshes;
	CurrentSpawningModelGuid = RootItem.ModelGuid;

	// Add root to queue
	PendingSpawnQueue.Add(FFragmentSpawnTask(RootItem, OwnerActor));

	// Start timer to process chunks
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			SpawnChunkTimerHandle,
			this,
			&UFragmentsImporter::ProcessSpawnChunk,
			0.016f, // ~60 FPS 
			true // Loop
		);
	}
	// At the end of StartChunkedSpawning:
	if (DeduplicationManager)
	{
		int32 UniqueGeometries = 0;
		int32 TotalInstances = 0;
		float Ratio = 0.0f;

		DeduplicationManager->GetStats(UniqueGeometries, TotalInstances, Ratio);

		UE_LOG(LogFragments, Log, TEXT("=== DEDUPLICATION STATS ==="));
		UE_LOG(LogFragments, Log, TEXT("Unique geometries: %d"), UniqueGeometries);
		UE_LOG(LogFragments, Log, TEXT("Total instances: %d"), TotalInstances);
		UE_LOG(LogFragments, Log, TEXT("Deduplication ratio: %.1fx"), Ratio);
		UE_LOG(LogFragments, Log, TEXT("Memory saved: ~%.0f%%"),
			(1.0f - (1.0f / Ratio)) * 100.0f);
	}

	UE_LOG(LogFragments, Log, TEXT("Chunked Spawning Started. Processing %d fragments per frame."), FragmentsPerChunk);
}

UStaticMesh* UFragmentsImporter::CreateStaticMeshFromShell(const Shell* ShellRef, const Material* RefMaterial, const FString& AssetName, UObject* OuterRef)
{
	// Create StaticMesh object
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(OuterRef, FName(*AssetName), RF_Public | RF_Standalone /*| RF_Transient*/);
	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();

	UStaticMeshDescription* StaticMeshDescription = StaticMesh->CreateStaticMeshDescription(OuterRef);
	FMeshDescription& MeshDescription = StaticMeshDescription->GetMeshDescription();
	UStaticMesh::FBuildMeshDescriptionsParams MeshParams;

	//Build Settings
#if WITH_EDITOR
	{
		FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = true;
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
		SrcModel.BuildSettings.bBuildReversedIndexBuffer = true;
		SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
		SrcModel.BuildSettings.bGenerateLightmapUVs = true;
		SrcModel.BuildSettings.SrcLightmapIndex = 0;
		SrcModel.BuildSettings.DstLightmapIndex = 1;
		SrcModel.BuildSettings.MinLightmapResolution = 64;
		SrcModel.BuildSettings.DistanceFieldResolutionScale = 0.0f; // Disable distance field generation for runtime meshes
	}
#endif

	MeshParams.bBuildSimpleCollision = true;
	MeshParams.bCommitMeshDescription = true;
	MeshParams.bMarkPackageDirty = true;
	MeshParams.bUseHashAsGuid = false;
#if !WITH_EDITOR
	MeshParams.bFastBuild = true;
#endif

	// Convert Shell Geometry (vertices and triangles)
	const auto* Points = ShellRef->points();
	TArray<FVector> PointsRef;
	TArray<FVertexID> Vertices;
	Vertices.Reserve(Points->size());

	for (flatbuffers::uoffset_t i = 0; i < Points->size(); i++)
	{
		const auto& P = *Points->Get(i);
		const FVertexID VertId = StaticMeshDescription->CreateVertex();
		StaticMeshDescription->SetVertexPosition(VertId, FVector(P.x()*100, P.z()*100, P.y()*100)); // Fix Z-up, and Unreal Units from m to cm
		PointsRef.Add(FVector(P.x() * 100, P.z() * 100, P.y() * 100));
		Vertices.Add(VertId);

		//UE_LOG(LogTemp, Log, TEXT("\t\t\t\tpoint %d: x: %f, y:%f, z:%f"), i, P.x(), P.y(), P.z());
	}

	FName MaterialSlotName = AddMaterialToMesh(StaticMesh, RefMaterial);
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription->CreatePolygonGroup();
	StaticMeshDescription->SetPolygonGroupMaterialSlotName(PolygonGroupId, MaterialSlotName);

	// Map the holes and identify the profiles that has holes
	const auto* Holes = ShellRef->holes();
	TMap<int32, TArray<TArray<int32>>> ProfileHolesIdx;
	for (flatbuffers::uoffset_t j = 0; j < Holes->size(); j++)
	{
		const auto* Hole = Holes->Get(j);
		const auto* HoleIndices = Hole->indices();
		const auto Profile_id = Hole->profile_id();
		TArray<int32> HoleIdx;

		for (flatbuffers::uoffset_t k = 0; k < HoleIndices->size(); k++)
		{
			HoleIdx.Add(HoleIndices->Get(k));
		}

		if (ProfileHolesIdx.Contains(Profile_id))
		{
			ProfileHolesIdx[Profile_id].Add(HoleIdx);
		}
		else
		{
			TArray<TArray<int32>> HolesForProfile;
			HolesForProfile.Add(HoleIdx);
			ProfileHolesIdx.Add(Profile_id, HolesForProfile);
		}
	}
	// Create Faces (triangles)
	const auto* Profiles = ShellRef->profiles();
	TMap<int32, FPolygonID> PolygonMap;
	
	for (flatbuffers::uoffset_t i = 0; i < Profiles->size(); i++)
	{
		// Create Profile Polygons for those that has no holes reference
		//UE_LOG(LogTemp, Log, TEXT("Processing Profile %d"), i);
		if (!ProfileHolesIdx.Contains(i))
		{
			const ShellProfile* Profile = Profiles->Get(i);
			const auto* Indices = Profile->indices();

			TArray<FVertexInstanceID> VertexInstances;
			VertexInstances.Reserve(Indices->size());
			TArray<FVertexInstanceID> TriangleInstance;
			for (flatbuffers::uoffset_t j = 0; j < Indices->size(); j++)
			{
				const auto Indice = Indices->Get(j);
				if (Vertices.IsValidIndex(Indice))
				{
					TriangleInstance.Add(MeshDescription.CreateVertexInstance(Vertices[Indice]));
				}
				else
					UE_LOG(LogFragments, Log, TEXT("Invalid Indice: shell %s, profile %d, indice %d"), *AssetName, i, j);
			}


			if (!TriangleInstance.IsEmpty())
			{
				FPolygonID PolygonID = MeshDescription.CreatePolygon(PolygonGroupId, TriangleInstance, {});
			}
		}
		// Process profile with holes to create new polygon that fully represent the substraction of holes in profile
		else
		{
			const ShellProfile* Profile = Profiles->Get(i);
			const auto* Indices = Profile->indices();
			if (Indices->size() < 3)
			{
				UE_LOG(LogFragments, Error, TEXT("Profile %d skipped: fewer than 3 points"), i);
				continue;
			}

			TArray<FVector> ProfilePoints;
			TArray<int32> ProfilePointsIndex;
			for (flatbuffers::uoffset_t j = 0; j < Indices->size(); j++)
			{
				const auto Indice = Indices->Get(j);
				ProfilePointsIndex.Add(Indice);
				const auto* Point = Points->Get(Indice);
				FVector Vector = FVector(Point->x() * 100, Point->y() * 100, Point->z() * 100);
				ProfilePoints.Add(Vector);
			}

			TArray<TArray<FVector>> ProfileHolesPoints;

			/*UE_LOG(LogTemp, Log, TEXT("Profile %d has %d points and %d holes"),
				i, Indices->size(), ProfileHolesIdx.Contains(i) ? ProfileHolesIdx[i].Num() : 0);*/

			TArray<int32> OutIndices;
			TArray<FVector> OutVertices;
			if (!TriangulatePolygonWithHoles(PointsRef, ProfilePointsIndex, ProfileHolesIdx[i], OutVertices, OutIndices))
			{
				UE_LOG(LogFragments, Error, TEXT("Profile %d skipped: Triangulation failed"), i);
				continue;
			}

			TMap<int32, FVertexID> TempVertexMap;
			for (int32 j = 0; j < OutVertices.Num(); j++)
			{
				FVertexID VId = StaticMeshDescription->CreateVertex();
				StaticMeshDescription->SetVertexPosition(VId, OutVertices[j]);
				TempVertexMap.Add(j, VId);
			}

			for (int32 j = 0; j < OutIndices.Num(); j+=3)
			{
				TArray<FVertexInstanceID> Triangle;

				Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j]]));
				Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j+1]]));
				Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j+2]]));
				
				if (!Triangle.IsEmpty())
					MeshDescription.CreatePolygon(PolygonGroupId, Triangle);
			}

		}
	}
	
	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MeshDescription);
	FStaticMeshOperations::ComputeTangentsAndNormals(MeshDescription, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);

	StaticMesh->BuildFromMeshDescriptions(TArray<const FMeshDescription*>{&MeshDescription}, MeshParams);

	return StaticMesh;
}

UStaticMesh* UFragmentsImporter::CreateStaticMeshFromCircleExtrusion(const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, const FString& AssetName, UObject* OuterRef)
{
	if (!CircleExtrusion || !CircleExtrusion->axes() || CircleExtrusion->axes()->size() == 0)
		return nullptr;

	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(OuterRef, FName(*AssetName), RF_Public | RF_Standalone | RF_Transient);
	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();

	TArray<const FMeshDescription*> MeshDescriptionPtrs;

	//Build Settings
#if WITH_EDITOR
	{
		FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = true;
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
		SrcModel.BuildSettings.bBuildReversedIndexBuffer = true;
		SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
		SrcModel.BuildSettings.bGenerateLightmapUVs = false;
		SrcModel.BuildSettings.SrcLightmapIndex = 0;
		SrcModel.BuildSettings.DstLightmapIndex = 1;
		SrcModel.BuildSettings.MinLightmapResolution = 64;
		SrcModel.BuildSettings.DistanceFieldResolutionScale = 0.0f; // Disable distance field generation for runtime meshes
	}
#endif

	// LOD0 – Full circle extrusion
	UStaticMeshDescription* LOD0Desc = StaticMesh->CreateStaticMeshDescription(OuterRef);
	BuildFullCircleExtrusion(*LOD0Desc, CircleExtrusion, RefMaterial, StaticMesh);
	MeshDescriptionPtrs.Add(&LOD0Desc->GetMeshDescription());

	{ // To Do: Implementation of LOD for Static Mesh Created. Seaking for better Performance??
		// LOD1 – Line representation
		//UStaticMeshDescription* LOD1Desc = StaticMesh->CreateStaticMeshDescription(OwnerRef);
		//BuildLineOnlyMesh(*LOD1Desc, CircleExtrusion);
		//MeshDescriptionPtrs.Add(&LOD1Desc->GetMeshDescription());

		//// LOD2 – Empty mesh
		//UStaticMeshDescription* LOD2Desc = StaticMesh->CreateStaticMeshDescription(OwnerRef);
		////BuildEmptyMesh(*LOD2Desc);
		//MeshDescriptionPtrs.Add(&LOD2Desc->GetMeshDescription());
	}

	// Build mesh
	UStaticMesh::FBuildMeshDescriptionsParams MeshParams;
	MeshParams.bBuildSimpleCollision = true;
	MeshParams.bCommitMeshDescription = true;
	MeshParams.bMarkPackageDirty = true;
	MeshParams.bUseHashAsGuid = false;

#if !WITH_EDITOR
	MeshParams.bFastBuild = true;
#endif

	StaticMesh->BuildFromMeshDescriptions(MeshDescriptionPtrs, MeshParams);

	// THIS IS EDITOR ONLY. TO DO: Find the way to make it in Runtime??
	//if (StaticMesh->GetNumSourceModels() >= 3)
	//{
	//	StaticMesh->GetSourceModel(0).ScreenSize.Default = 1.0f;
	//	StaticMesh->GetSourceModel(1).ScreenSize.Default = 0.5f;
	//	StaticMesh->GetSourceModel(2).ScreenSize.Default = 0.1f;
	//}
	//else
	//{
	//	UE_LOG(LogTemp, Warning, TEXT("Unexpected: Only %d LODs were created!"), StaticMesh->GetNumSourceModels());
	//}
	return StaticMesh;
}

FName UFragmentsImporter::AddMaterialToMesh(UStaticMesh*& CreatedMesh, const Material* RefMaterial)
{
	if (!RefMaterial || !CreatedMesh) return FName();

	// Extract material properties
	uint8 R = RefMaterial->r();
	uint8 G = RefMaterial->g();
	uint8 B = RefMaterial->b();
	uint8 A = RefMaterial->a();
	bool bIsGlass = A < 255; // Glass is determined by transparency

	// Use pooled material for CRC-based deduplication
	UMaterialInstanceDynamic* DynamicMaterial = GetPooledMaterial(R, G, B, A, bIsGlass);
	if (!DynamicMaterial)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to get pooled material"));
		return FName();
	}

	// Add Material
	return CreatedMesh->AddMaterial(DynamicMaterial);
}

bool UFragmentsImporter::TriangulatePolygonWithHoles(const TArray<FVector>& Points,
	const TArray<int32>& Profiles,
	const TArray<TArray<int32>>& Holes,
	TArray<FVector>& OutVertices,
	TArray<int32>& OutIndices)
{
	TESStesselator* Tess = tessNewTess(nullptr);
	FPlaneProjection Projection = UFragmentsUtils::BuildProjectionPlane(Points, Profiles);
	
	auto AddContour = [&](const TArray<int32>& Indices, bool bIsHole)
		{
			TArray<FVector2D> Projected;
			TArray<float> Contour;

			for (int32 Index : Indices)
			{
				FVector2D P2d = Projection.Project(Points[Index]);
				Projected.Add(P2d);
			}

			if (Projected.Num() < 3)
			{
				UE_LOG(LogFragments, Error, TEXT("Contour has fewer than 3 points, skipping."));
				return;
			}

			// Check for colinearity
			bool bColinear = true;
			const FVector2D& A = Projected[0];

			for (int i = 1; i < Projected.Num() - 1; ++i)
			{
				FVector2D Dir1 = (Projected[i] - A).GetSafeNormal();
				FVector2D Dir2 = (Projected[i + 1] - Projected[i]).GetSafeNormal();
				if (!Dir1.Equals(Dir2, 0.001f))
				{
					bColinear = false;
					break;
				}
			}
			if (bColinear)
			{
				UE_LOG(LogFragments, Error, TEXT("Contour is colinear in 2D projection, skipping."));
				return;
			}

			TArray<FVector2D> UniqueProjected;
			UniqueProjected.Reserve(Projected.Num());

			for (int32 i = 0; i < Projected.Num(); ++i)
			{
				if (i == 0 || !Projected[i].Equals(Projected[i - 1], 0.001))
				{
					UniqueProjected.Add(Projected[i]);
				}
			}
			Projected = UniqueProjected;

			/*for (const FVector2D& P : Projected)
			{
				UE_LOG(LogTemp, Log, TEXT("\tProjected Point before winding check: X: %.3f, Y: %.3f"), P.X, P.Y);
			}*/

			// Fix winding
			bool bClockwise = UFragmentsUtils::IsClockwise(Projected);
			//UE_LOG(LogTemp, Log, TEXT("Contour winding is %s"), bClockwise ? TEXT("CW") : TEXT("CCW"));

			if (!bIsHole && bClockwise)
			{
				Algo::Reverse(Projected); // Outer should be CCW
			}
			else if (bIsHole && !bClockwise)
			{
				Algo::Reverse(Projected); // Holes should be CW
			}
			
			for (const FVector2D& P : Projected)
			{
				Contour.Add(P.X);
				Contour.Add(P.Y);
				//UE_LOG(LogTemp, Warning, TEXT("    X: %.6f, Y: %.6f"), P.X, P.Y);
			}

			tessAddContour(Tess, 2, Contour.GetData(), sizeof(float) * 2, Projected.Num());
		};

	AddContour(Profiles, false);

	for (const TArray<int32>& Hole : Holes)
	{
		AddContour(Hole, true);
	}

	if (!tessTesselate(Tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, nullptr))
	{
		UE_LOG(LogFragments, Error, TEXT("tessTesselate failed."));
		tessDeleteTess(Tess);
		return false;
	}

	const int32 VertexCount = tessGetVertexCount(Tess);
	const TESSreal* Vertices = tessGetVertices(Tess);

	if (VertexCount == 0)
	{
		for (const int32& P : Profiles)
		{
			UE_LOG(LogFragments, Warning, TEXT("\tPoints of Vertex 0 X: %.6f, Y: %.6f, Z: %.6f"), Points[P].X, Points[P].Y, Points[P].Z);
		}

		int32 HoleIdx = 0;
		for (const auto& H : Holes)
		{
			for (const int32& P : H)
			{
				UE_LOG(LogFragments, Warning, TEXT("\tPoints of Hole %d 0 X: %.6f, Y: %.6f, Z: %.6f"), HoleIdx, Points[P].X, Points[P].Y, Points[P].Z);
			}
			HoleIdx++;
		}
	}

	TMap<int32, int32> IndexRemap;

	for (int32 i = 0; i < VertexCount; i++)
	{
		FVector2D P2d(Vertices[i * 2], Vertices[i * 2 + 1]);
		FVector P3d = Projection.Unproject(P2d);
		IndexRemap.Add(i, OutVertices.Num());
		OutVertices.Add(P3d);
	}
	const int32* Indices = tessGetElements(Tess);
	const int32 ElementCount = tessGetElementCount(Tess);
	//UE_LOG(LogTemp, Log, TEXT("Tessellated VertexCount: %d, ElementCount: %d"), VertexCount, ElementCount);

	for (int32 i = 0; i < ElementCount; ++i)
	{
		const int32* Poly = &Indices[i * 3];
		for (int j = 0; j < 3; ++j)
		{
			int32 Idx = Poly[j];
			if (Idx != TESS_UNDEF)
			{
				OutIndices.Add(IndexRemap[Idx]);
			}
		}
	}

	tessDeleteTess(Tess);

	return true;
}

void UFragmentsImporter::BuildFullCircleExtrusion(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, UStaticMesh* StaticMesh)
{
	FStaticMeshAttributes Attributes(StaticMeshDescription.GetMeshDescription());
	Attributes.Register();

	FName MaterialSlotName = AddMaterialToMesh(StaticMesh, RefMaterial);
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription.CreatePolygonGroup();
	StaticMeshDescription.SetPolygonGroupMaterialSlotName(PolygonGroupId, MaterialSlotName);

	FMeshDescription& MeshDescription = StaticMeshDescription.GetMeshDescription();

	const auto* Axes = CircleExtrusion->axes();
	const auto* Radii = CircleExtrusion->radius();
	int32 SegmentCount = 16;

	for (flatbuffers::uoffset_t axisIndex = 0; axisIndex < Axes->size(); ++axisIndex)
	{
		const auto* Axis = Axes->Get(axisIndex);
		const auto* Orders = Axis->order();
		const auto* Parts = Axis->parts();
		const auto* Wires = Axis->wires();
		const auto* WireSets = Axis->wire_sets();
		//TArray<TArray<FVertexID>> AllRings;

		for (flatbuffers::uoffset_t i = 0; i < Orders->size(); i++)
		{
			int32 OrderIndex = Orders->Get(i);
			int32 PartIndex = Parts->Get(i);

			TArray<TArray<FVertexID>> RingVertexIDs;

			// Handle CIRCLE_CURVE
			if (Axis->circle_curves() && PartIndex == (int)AxisPartClass::AxisPartClass_CIRCLE_CURVE)
			{
				const auto* Curves = Axis->circle_curves();
				const float Radius = Radii->Get(axisIndex) * 100.0f;

				TArray<FVector> ArcCenters;
				TArray<FVector> ArcTangents;

				// Sample arc
				for (flatbuffers::uoffset_t c = 0; c < Curves->size(); ++c)
				{
					const auto* Circle = Curves->Get(c);
					FVector Center = FVector(Circle->position().x(), Circle->position().z(), Circle->position().y()) * 100.0f;
					FVector XDir = FVector(Circle->x_direction().x(), Circle->x_direction().z(), Circle->x_direction().y());
					FVector YDir = FVector(Circle->y_direction().x(), Circle->y_direction().z(), Circle->y_direction().y());
					float ApertureRad = FMath::DegreesToRadians(Circle->aperture());
					float ArcRadius = Circle->radius() * 100.0f;

					int32 ArcDivs = FMath::Clamp(FMath::RoundToInt(ApertureRad * ArcRadius * 0.05f), 4, 32);
					for (int32 j = 0; j <= ArcDivs; ++j)
					{
						float t = static_cast<float>(j) / ArcDivs;
						float angle = -ApertureRad / 2.0f + t * ApertureRad;
						FVector Pos = Center + ArcRadius * (FMath::Cos(angle) * XDir + FMath::Sin(angle) * YDir);
						ArcCenters.Add(Pos);
					}
				}

				// Compute tangents
				for (int32 j = 0; j < ArcCenters.Num(); ++j)
				{
					if (j == 0)
						ArcTangents.Add((ArcCenters[1] - ArcCenters[0]).GetSafeNormal());
					else if (j == ArcCenters.Num() - 1)
						ArcTangents.Add((ArcCenters.Last() - ArcCenters[j - 1]).GetSafeNormal());
					else
						ArcTangents.Add((ArcCenters[j + 1] - ArcCenters[j - 1]).GetSafeNormal());
				}

				// Initial frame from first tangent
				FVector PrevTangent = ArcTangents[0];
				FVector PrevX, PrevY;
				PrevTangent.FindBestAxisVectors(PrevX, PrevY);

				TArray<TArray<FVertexID>> AllRings;

				for (int32 k = 0; k < ArcCenters.Num(); ++k)
				{
					const FVector& Tangent = ArcTangents[k];
					FQuat AlignQuat = FQuat::FindBetweenNormals(PrevTangent, Tangent);
					FVector CurrX = AlignQuat.RotateVector(PrevX);
					FVector CurrY = AlignQuat.RotateVector(PrevY);

					TArray<FVertexID> Ring;
					for (int32 j = 0; j < SegmentCount; ++j)
					{
						float Angle = 2.0f * PI * j / SegmentCount;
						FVector Offset = FMath::Cos(Angle) * CurrX + FMath::Sin(Angle) * CurrY;
						FVector Pos = ArcCenters[k] + Offset * Radius;

						FVertexID V = StaticMeshDescription.CreateVertex();
						StaticMeshDescription.SetVertexPosition(V, Pos);
						Ring.Add(V);
					}

					AllRings.Add(Ring);
					PrevTangent = Tangent;
					PrevX = CurrX;
					PrevY = CurrY;
				}

				// Stitch rings
				for (int32 k = 0; k < AllRings.Num() - 1; ++k)
				{
					const auto& RingA = AllRings[k];
					const auto& RingB = AllRings[k + 1];

					for (int32 j = 0; j < SegmentCount; ++j)
					{
						int32 Next = (j + 1) % SegmentCount;

						FVertexID V00 = RingA[j];
						FVertexID V01 = RingB[j];
						FVertexID V10 = RingA[Next];
						FVertexID V11 = RingB[Next];

						TArray<FVertexInstanceID> Tri1 = {
							MeshDescription.CreateVertexInstance(V00),
							MeshDescription.CreateVertexInstance(V01),
							MeshDescription.CreateVertexInstance(V10)
						};
						TArray<FVertexInstanceID> Tri2 = {
							MeshDescription.CreateVertexInstance(V10),
							MeshDescription.CreateVertexInstance(V01),
							MeshDescription.CreateVertexInstance(V11)
						};

						MeshDescription.CreatePolygon(PolygonGroupId, Tri1);
						MeshDescription.CreatePolygon(PolygonGroupId, Tri2);
					}
				}
			}

			else if (Wires && PartIndex == (int)AxisPartClass::AxisPartClass_WIRE)
			{
				// Handle Wire
				const auto* Wire = Wires->Get(OrderIndex);
				FVector P1 = FVector(Wire->p1().x(), Wire->p1().z(), Wire->p1().y()) * 100.0f;
				FVector P2 = FVector(Wire->p2().x(), Wire->p2().z(), Wire->p2().y()) * 100.0f;

				FVector Direction = (P2 - P1).GetSafeNormal();
				FVector XDir, YDir;
				Direction.FindBestAxisVectors(XDir, YDir);

				TArray<FVertexID> Ring1, Ring2;

				for (int32 j = 0; j < SegmentCount; j++)
				{
					float Angle = 2.0f * PI * j / SegmentCount;
					FVector Offset = FMath::Cos(Angle) * XDir + FMath::Sin(Angle) * YDir;
					FVector V1 = P1 + Offset * Radii->Get(OrderIndex) * 100.0f;
					FVector V2 = P2 + Offset * Radii->Get(OrderIndex) * 100.0f;

					FVertexID ID1 = StaticMeshDescription.CreateVertex();
					FVertexID ID2 = StaticMeshDescription.CreateVertex();

					StaticMeshDescription.SetVertexPosition(ID1, V1);
					StaticMeshDescription.SetVertexPosition(ID2, V2);

					Ring1.Add(ID1);
					Ring2.Add(ID2);
				}

				for (int32 j = 0; j < SegmentCount; ++j)
				{
					int32 Next = (j + 1) % SegmentCount;

					FVertexID V00 = Ring1[j];
					FVertexID V01 = Ring2[j];
					FVertexID V10 = Ring1[Next];
					FVertexID V11 = Ring2[Next];

					TArray<FVertexInstanceID> Tri1 = {
						StaticMeshDescription.CreateVertexInstance(V00),
						StaticMeshDescription.CreateVertexInstance(V01),
						StaticMeshDescription.CreateVertexInstance(V10)
					};

					TArray<FVertexInstanceID> Tri2 = {
						StaticMeshDescription.CreateVertexInstance(V10),
						StaticMeshDescription.CreateVertexInstance(V01),
						StaticMeshDescription.CreateVertexInstance(V11)
					};

					StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri1);
					StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri2);
				}

				return;
			}

			else if (WireSets && PartIndex == (int)AxisPartClass::AxisPartClass_WIRE_SET)
			{
				const auto* WSet = WireSets->Get(OrderIndex);
				const auto* Points = WSet->ps();

				if (!Points || Points->size() < 2)
					continue;

				TArray<TArray<FVertexID>> Rings;

				for (flatbuffers::uoffset_t p = 0; p < Points->size(); p++)
				{
					const auto& Pt = Points->Get(p);
					FVector Pos = FVector(Pt->x(), Pt->z(), Pt->y()) * 100.0f;

					// compute Tangent along polyline
					FVector Tangent;
					if (p == 0)
					{
						const auto& Next = Points->Get(p + 1);
						Tangent = (FVector(Next->x(), Next->z(), Next->y()) * 100.0f - Pos).GetSafeNormal();
					}
					else if (p == Points->size() - 1)
					{
						const auto& Prev = Points->Get(p - 1);
						Tangent = (Pos - FVector(Prev->x(), Prev->z(), Prev->y()) * 100.0f).GetSafeNormal();
					}
					else
					{
						const auto& Prev = Points->Get(p - 1);
						const auto& Next = Points->Get(p + 1);
						Tangent = (FVector(Next->x(), Next->z(), Next->y()) * 100.0f - FVector(Prev->x(), Prev->z(), Prev->y()) * 100.0f).GetSafeNormal();
					}

					// Frame
					FVector XDir, YDir;
					Tangent.FindBestAxisVectors(XDir, YDir);

					TArray<FVertexID> Ring;
					for (int32 j = 0; j < SegmentCount; j++)
					{
						float Angle = 2.0f * PI * j / SegmentCount;
						FVector Offset = FMath::Cos(Angle) * XDir + FMath::Sin(Angle) * YDir;
						FVector RingPos = Pos + Offset * Radii->Get(OrderIndex) * 100.0f;

						FVertexID Vtx = StaticMeshDescription.CreateVertex();
						StaticMeshDescription.SetVertexPosition(Vtx, RingPos);
						Ring.Add(Vtx);
					}

					// Connect rings
					for (int32 k = 0; k < Rings.Num() - 1; ++k)
					{
						const auto& RingA = Rings[k];
						const auto& RingB = Rings[k + 1];

						for (int32 j = 0; j < SegmentCount; ++j)
						{
							int32 Next = (j + 1) % SegmentCount;

							FVertexID V00 = RingA[j];
							FVertexID V01 = RingB[j];
							FVertexID V10 = RingA[Next];
							FVertexID V11 = RingB[Next];

							TArray<FVertexInstanceID> Tri1 = {
								MeshDescription.CreateVertexInstance(V00),
								MeshDescription.CreateVertexInstance(V01),
								MeshDescription.CreateVertexInstance(V10)
							};

							TArray<FVertexInstanceID> Tri2 = {
								MeshDescription.CreateVertexInstance(V10),
								MeshDescription.CreateVertexInstance(V01),
								MeshDescription.CreateVertexInstance(V11)
							};

							MeshDescription.CreatePolygon(PolygonGroupId, Tri1);
							MeshDescription.CreatePolygon(PolygonGroupId, Tri2);
						}
					}
				}
			}
		}
	}

	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(StaticMeshDescription.GetMeshDescription());
	FStaticMeshOperations::ComputeTangentsAndNormals(StaticMeshDescription.GetMeshDescription(), EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);
}

void UFragmentsImporter::BuildLineOnlyMesh(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion)
{
	FStaticMeshAttributes Attributes(StaticMeshDescription.GetMeshDescription());
	Attributes.Register();
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription.CreatePolygonGroup();
	FMeshDescription& MeshDescription = StaticMeshDescription.GetMeshDescription();

	const auto* Axes = CircleExtrusion->axes();
	if (!Axes) return;

	for (auto Axis : *Axes)
	{
		const auto* Orders = Axis->order();
		const auto* Wires = Axis->wires();

		for (uint32 i = 0; i < Orders->size(); ++i)
		{
			const auto* Wire = Wires->Get(Orders->Get(i));
			FVector P1 = FVector(Wire->p1().x(), Wire->p1().z(), Wire->p1().y()) * 100.0f;
			FVector P2 = FVector(Wire->p2().x(), Wire->p2().z(), Wire->p2().y()) * 100.0f;

			FVertexID V0 = StaticMeshDescription.CreateVertex();
			FVertexID V1 = StaticMeshDescription.CreateVertex();
			StaticMeshDescription.SetVertexPosition(V0, P1);
			StaticMeshDescription.SetVertexPosition(V1, P2);

			TArray<FVertexInstanceID> Tri;
			FVertexInstanceID I0 = StaticMeshDescription.CreateVertexInstance(V0);
			FVertexInstanceID I1 = StaticMeshDescription.CreateVertexInstance(V1);
			FVertexInstanceID I2 = StaticMeshDescription.CreateVertexInstance(V1); // degenerate triangle
			Tri.Add(I0);
			Tri.Add(I1);
			Tri.Add(I2);

			StaticMeshDescription.GetMeshDescription().CreatePolygon(PolygonGroupId, Tri,{});
		}
	}
}

TArray<FVector> UFragmentsImporter::SampleRingPoints(const FVector& Center, const FVector XDir, const FVector& YDir, float Radius, int SegmentCount, float ApertureRadians)
{
	TArray<FVector> Ring;
	for (int32 i = 0; i <= SegmentCount; i++)
	{
		float t = (float)i / SegmentCount;
		float angle = -ApertureRadians / 2.0f + t * ApertureRadians;
		FVector Point = Center + Radius * (FMath::Cos(angle) * XDir + FMath::Sin(angle) * YDir);
		Ring.Add(Point);
	}
	return Ring;
}

void UFragmentsImporter::SavePackagesWithProgress(const TArray<UPackage*>& InPackagesToSave)
{
#if WITH_EDITOR
	if (PackagesToSave.Num() == 0)
		return;

	// Create a slow task with progress bar and allow cancel
	FScopedSlowTask SlowTask((float)PackagesToSave.Num(), FText::FromString(TEXT("Saving Static Meshes...")));
	SlowTask.MakeDialog(true);

	for (UPackage* Package : PackagesToSave)
	{
		if (SlowTask.ShouldCancel())
		{
			UE_LOG(LogFragments, Warning, TEXT("User canceled saving packages."));
			break;
		}

		FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.SaveFlags = RF_Public | RF_Standalone;

		bool bSuccess = UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs);
		if (!bSuccess)
		{
			UE_LOG(LogFragments, Error, TEXT("Failed to save package: %s"), *Package->GetName());
		}
		else
		{
			UE_LOG(LogFragments, Log, TEXT("Saved package: %s"), *Package->GetName());
		}

		SlowTask.EnterProgressFrame(1.f);
	}
#else
	// Runtime: do not save packages, just log
	UE_LOG(LogFragments, Log, TEXT("Skipping package saving in runtime environment."));
#endif
}

//////////////////////////////////////////////////////////////////////////
// ASYNC GEOMETRY PROCESSING (Phase 1)
//////////////////////////////////////////////////////////////////////////

void UFragmentsImporter::InitializeWorkerPool()
{
	if (!GeometryWorkerPool.IsValid())
	{
		GeometryWorkerPool = MakeUnique<FGeometryWorkerPool>();
		GeometryWorkerPool->Initialize();
		UE_LOG(LogFragments, Log, TEXT("=== ASYNC GEOMETRY PROCESSING ENABLED ==="));
		UE_LOG(LogFragments, Log, TEXT("Geometry worker pool initialized with parallel tessellation support"));
		UE_LOG(LogFragments, Log, TEXT("Shell geometry will be processed on background threads"));
	}
}

void UFragmentsImporter::ShutdownWorkerPool()
{
	if (GeometryWorkerPool.IsValid())
	{
		GeometryWorkerPool->Shutdown();
		GeometryWorkerPool.Reset();
		UE_LOG(LogFragments, Log, TEXT("Geometry worker pool shut down"));
	}
}

void UFragmentsImporter::ProcessCompletedGeometry()
{
	if (!GeometryWorkerPool.IsValid() || !GeometryWorkerPool->HasCompletedWork())
	{
		return;
	}

	const double StartTime = FPlatformTime::Seconds();
	const double BudgetSeconds = GeometryProcessingBudgetMs / 1000.0;
	int32 ProcessedCount = 0;

	FRawGeometryData GeometryData;
	while (GeometryWorkerPool->DequeueCompletedWork(GeometryData))
	{
		if (!GeometryData.bSuccess)
		{
			UE_LOG(LogFragments, Warning, TEXT("Async geometry processing failed for mesh %s: %s"),
				*GeometryData.MeshName, *GeometryData.ErrorMessage);

			// Remove from pending map
			PendingFragmentMap.Remove(GeometryData.WorkItemId);
			continue;
		}

		// Find the pending fragment data
		FPendingFragmentData* PendingData = PendingFragmentMap.Find(GeometryData.WorkItemId);
		if (!PendingData)
		{
			UE_LOG(LogFragments, Warning, TEXT("No pending fragment found for work item %llu"), GeometryData.WorkItemId);
			continue;
		}

		// Validate package path
		if (GeometryData.PackagePath.IsEmpty())
		{
			UE_LOG(LogFragments, Error, TEXT("ProcessCompletedGeometry: Empty package path for mesh %s"), *GeometryData.MeshName);
			PendingFragmentMap.Remove(GeometryData.WorkItemId);
			continue;
		}

		// Create the UStaticMesh from raw data (must be on game thread)
		UPackage* MeshPackage = CreatePackage(*GeometryData.PackagePath);
		if (!MeshPackage)
		{
			UE_LOG(LogFragments, Error, TEXT("ProcessCompletedGeometry: Failed to create package %s"), *GeometryData.PackagePath);
			PendingFragmentMap.Remove(GeometryData.WorkItemId);
			continue;
		}

		UStaticMesh* Mesh = CreateMeshFromRawData(GeometryData, MeshPackage);

		if (Mesh)
		{
			// Cache the mesh
			const FString SamplePath = GeometryData.PackagePath + TEXT(".") + GeometryData.MeshName;
			MeshCache.Add(SamplePath, Mesh);

			// Finalize the fragment with the mesh
			FinalizeFragmentWithMesh(GeometryData, Mesh);

			// Handle package saving
			if (PendingData->bSaveMeshes)
			{
#if WITH_EDITOR
				FString PackageFileName = FPackageName::LongPackageNameToFilename(
					FPackageName::ObjectPathToPackageName(GeometryData.PackagePath),
					FPackageName::GetAssetPackageExtension());

				if (!FPaths::FileExists(PackageFileName))
				{
					MeshPackage->FullyLoad();
					Mesh->Rename(*GeometryData.MeshName, MeshPackage);
					Mesh->SetFlags(RF_Public | RF_Standalone);
					MeshPackage->MarkPackageDirty();
					FAssetRegistryModule::AssetCreated(Mesh);
					PackagesToSave.Add(MeshPackage);
				}
#endif
			}

			ProcessedCount++;
		}

		// Remove from pending map
		PendingFragmentMap.Remove(GeometryData.WorkItemId);

		// Check frame budget
		if ((FPlatformTime::Seconds() - StartTime) > BudgetSeconds)
		{
			break;
		}
	}

	if (ProcessedCount > 0)
	{
		UE_LOG(LogFragments, Verbose, TEXT("Processed %d completed geometry items in %.2fms"),
			ProcessedCount, (FPlatformTime::Seconds() - StartTime) * 1000.0);
	}
}

UStaticMesh* UFragmentsImporter::CreateMeshFromRawData(const FRawGeometryData& GeometryData, UObject* OuterRef)
{
	// Must be called from game thread
	check(IsInGameThread());

	if (GeometryData.Positions.Num() == 0 || GeometryData.Indices.Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("CreateMeshFromRawData: No geometry data for %s"), *GeometryData.MeshName);
		return nullptr;
	}

	if (!OuterRef)
	{
		UE_LOG(LogFragments, Error, TEXT("CreateMeshFromRawData: OuterRef is null for %s"), *GeometryData.MeshName);
		return nullptr;
	}

	// Log geometry stats for debugging
	UE_LOG(LogFragments, Verbose, TEXT("CreateMeshFromRawData: %s - %d verts, %d indices"),
		*GeometryData.MeshName, GeometryData.Positions.Num(), GeometryData.Indices.Num());

	// Create StaticMesh object
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(OuterRef, FName(*GeometryData.MeshName), RF_Public | RF_Standalone);
	if (!StaticMesh)
	{
		UE_LOG(LogFragments, Error, TEXT("CreateMeshFromRawData: Failed to create StaticMesh for %s"), *GeometryData.MeshName);
		return nullptr;
	}

	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();

	// Use GetTransientPackage() for mesh description to avoid package-related crashes
	UStaticMeshDescription* StaticMeshDescription = StaticMesh->CreateStaticMeshDescription(GetTransientPackage());
	if (!StaticMeshDescription)
	{
		UE_LOG(LogFragments, Error, TEXT("CreateMeshFromRawData: Failed to create StaticMeshDescription for %s"), *GeometryData.MeshName);
		return nullptr;
	}
	FMeshDescription& MeshDescription = StaticMeshDescription->GetMeshDescription();
	UStaticMesh::FBuildMeshDescriptionsParams MeshParams;

	// Build Settings
#if WITH_EDITOR
	{
		FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = false; // We already computed normals
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
		SrcModel.BuildSettings.bBuildReversedIndexBuffer = true;
		SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
		SrcModel.BuildSettings.bGenerateLightmapUVs = true;
		SrcModel.BuildSettings.SrcLightmapIndex = 0;
		SrcModel.BuildSettings.DstLightmapIndex = 1;
		SrcModel.BuildSettings.MinLightmapResolution = 64;
		SrcModel.BuildSettings.DistanceFieldResolutionScale = 0.0f; // Disable distance field generation for runtime meshes
	}
#endif

	MeshParams.bBuildSimpleCollision = true;
	MeshParams.bCommitMeshDescription = true;
	MeshParams.bMarkPackageDirty = true;
	MeshParams.bUseHashAsGuid = false;
#if !WITH_EDITOR
	MeshParams.bFastBuild = true;
#endif

	// Create vertices
	TArray<FVertexID> Vertices;
	Vertices.Reserve(GeometryData.Positions.Num());

	for (int32 i = 0; i < GeometryData.Positions.Num(); i++)
	{
		const FVertexID VertId = StaticMeshDescription->CreateVertex();
		StaticMeshDescription->SetVertexPosition(VertId, FVector(GeometryData.Positions[i]));
		Vertices.Add(VertId);
	}

	// Add material
	FName MaterialSlotName = AddMaterialToMeshFromRawData(StaticMesh,
		GeometryData.R, GeometryData.G, GeometryData.B, GeometryData.A, GeometryData.bIsGlass);
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription->CreatePolygonGroup();
	StaticMeshDescription->SetPolygonGroupMaterialSlotName(PolygonGroupId, MaterialSlotName);

	// Create triangles
	int32 TrianglesCreated = 0;
	for (int32 i = 0; i < GeometryData.Indices.Num(); i += 3)
	{
		uint32 I0 = GeometryData.Indices[i];
		uint32 I1 = GeometryData.Indices[i + 1];
		uint32 I2 = GeometryData.Indices[i + 2];

		if (I0 < (uint32)Vertices.Num() && I1 < (uint32)Vertices.Num() && I2 < (uint32)Vertices.Num())
		{
			TArray<FVertexInstanceID> TriangleInstance;
			TriangleInstance.Add(MeshDescription.CreateVertexInstance(Vertices[I0]));
			TriangleInstance.Add(MeshDescription.CreateVertexInstance(Vertices[I1]));
			TriangleInstance.Add(MeshDescription.CreateVertexInstance(Vertices[I2]));

			// Set normals on vertex instances
			if (I0 < (uint32)GeometryData.Normals.Num() &&
				I1 < (uint32)GeometryData.Normals.Num() &&
				I2 < (uint32)GeometryData.Normals.Num())
			{
				TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals =
					MeshDescription.VertexInstanceAttributes().GetAttributesRef<FVector3f>(MeshAttribute::VertexInstance::Normal);

				VertexInstanceNormals[TriangleInstance[0]] = GeometryData.Normals[I0];
				VertexInstanceNormals[TriangleInstance[1]] = GeometryData.Normals[I1];
				VertexInstanceNormals[TriangleInstance[2]] = GeometryData.Normals[I2];
			}

			MeshDescription.CreatePolygon(PolygonGroupId, TriangleInstance);
			TrianglesCreated++;
		}
	}

	// Only proceed if we actually created triangles
	if (TrianglesCreated == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("CreateMeshFromRawData: No valid triangles created for %s"), *GeometryData.MeshName);
		return nullptr;
	}

	// Note: Tangents will be computed by BuildFromMeshDescriptions via build settings (bRecomputeTangents = true)
	// We skip explicit ComputeTangentsAndNormals call as it can crash on certain mesh configurations

	StaticMesh->BuildFromMeshDescriptions(TArray<const FMeshDescription*>{&MeshDescription}, MeshParams);

	return StaticMesh;
}

FName UFragmentsImporter::AddMaterialToMeshFromRawData(UStaticMesh*& CreatedMesh, uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass)
{
	if (!CreatedMesh) return FName();

	// Use pooled material
	UMaterialInstanceDynamic* DynamicMaterial = GetPooledMaterial(R, G, B, A, bIsGlass);
	if (!DynamicMaterial)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to get pooled material"));
		return FName();
	}

	return CreatedMesh->AddMaterial(DynamicMaterial);
}

uint32 UFragmentsImporter::HashMaterialProperties(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass) const
{
	uint32 Hash = 0;
	Hash = HashCombine(Hash, GetTypeHash(R));
	Hash = HashCombine(Hash, GetTypeHash(G));
	Hash = HashCombine(Hash, GetTypeHash(B));
	Hash = HashCombine(Hash, GetTypeHash(A));
	Hash = HashCombine(Hash, GetTypeHash(bIsGlass));
	return Hash;
}

UMaterialInstanceDynamic* UFragmentsImporter::GetPooledMaterial(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass)
{
	uint32 Hash = HashMaterialProperties(R, G, B, A, bIsGlass);

	if (UMaterialInstanceDynamic** Found = MaterialPool.Find(Hash))
	{
		return *Found;
	}

	// Create new material instance
	UMaterialInterface* BaseMat = bIsGlass ? BaseGlassMaterial : BaseMaterial;
	if (!BaseMat)
	{
		// Load materials if not already loaded
		BaseGlassMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentGlassMaterial.M_BaseFragmentGlassMaterial"));
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial"));
		BaseMat = bIsGlass ? BaseGlassMaterial : BaseMaterial;
	}

	if (!BaseMat)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to load base material for pooling"));
		return nullptr;
	}

	UMaterialInstanceDynamic* NewMat = UMaterialInstanceDynamic::Create(BaseMat, this);
	if (!NewMat)
	{
		return nullptr;
	}

	float Rf = R / 255.f;
	float Gf = G / 255.f;
	float Bf = B / 255.f;
	float Af = A / 255.f;

	if (Af < 1.0f)
	{
		NewMat->SetScalarParameterValue(TEXT("Opacity"), Af);
	}
	NewMat->SetVectorParameterValue(TEXT("BaseColor"), FVector4(Rf, Gf, Bf, Af));

	MaterialPool.Add(Hash, NewMat);
	return NewMat;
}

void UFragmentsImporter::SubmitShellForAsyncProcessing(
	const Shell* ShellRef,
	const Material* MaterialRef,
	const FFragmentItem& FragmentItem,
	int32 SampleIndex,
	const FString& MeshName,
	const FString& PackagePath,
	const FTransform& LocalTransform,
	AFragment* FragmentActor,
	AActor* ParentActor,
	bool bSaveMeshes)
{
	if (!GeometryWorkerPool.IsValid())
	{
		InitializeWorkerPool();
	}

	// Generate unique work item ID
	uint64 WorkItemId = GeometryWorkerPool->GenerateWorkItemId();

	// Extract work item from FlatBuffers (copies data for thread safety)
	FGeometryWorkItem WorkItem = FGeometryDataExtractor::ExtractShellWorkItem(
		ShellRef,
		MaterialRef,
		FragmentItem,
		SampleIndex,
		MeshName,
		PackagePath,
		LocalTransform,
		ParentActor,
		bSaveMeshes,
		WorkItemId
	);

	// Store pending fragment data for later completion
	FPendingFragmentData PendingData;
	PendingData.FragmentActor = FragmentActor;
	PendingData.ParentActor = ParentActor;
	PendingData.LocalTransform = LocalTransform;
	PendingData.SampleIndex = SampleIndex;
	PendingData.bSaveMeshes = bSaveMeshes;
	PendingData.PackagePath = PackagePath;
	PendingData.MeshName = MeshName;

	PendingFragmentMap.Add(WorkItemId, MoveTemp(PendingData));

	// Submit work to pool
	GeometryWorkerPool->SubmitWork(MoveTemp(WorkItem));

	UE_LOG(LogFragments, Verbose, TEXT("Submitted Shell for async processing: %s (WorkItemId: %llu)"),
		*MeshName, WorkItemId);
}

void UFragmentsImporter::FinalizeFragmentWithMesh(const FRawGeometryData& GeometryData, UStaticMesh* Mesh)
{
	FPendingFragmentData* PendingData = PendingFragmentMap.Find(GeometryData.WorkItemId);
	if (!PendingData)
	{
		return;
	}

	AFragment* FragmentActor = PendingData->FragmentActor.Get();
	if (!FragmentActor)
	{
		// Fragment actor doesn't exist yet or was destroyed
		// This shouldn't happen in normal flow
		UE_LOG(LogFragments, Warning, TEXT("FinalizeFragmentWithMesh: Fragment actor not found for mesh %s"), *GeometryData.MeshName);
		return;
	}

	// Add the mesh component to the fragment
	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(FragmentActor);
	MeshComp->SetStaticMesh(Mesh);
	MeshComp->SetRelativeTransform(PendingData->LocalTransform);

	USceneComponent* RootComp = FragmentActor->GetRootComponent();
	if (RootComp)
	{
		MeshComp->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
	}

	// Disable Lumen/Distance Field features for fast loading
	MeshComp->bAffectDistanceFieldLighting = false;
	MeshComp->bAffectDynamicIndirectLighting = false;
	MeshComp->bAffectIndirectLightingWhileHidden = false;

	MeshComp->RegisterComponent();
	FragmentActor->AddInstanceComponent(MeshComp);

	// Configure occlusion culling
	const EOcclusionRole Role = UFragmentOcclusionClassifier::ClassifyFragment(
		GeometryData.Category, GeometryData.A);

	switch (Role)
	{
	case EOcclusionRole::Occluder:
		MeshComp->bUseAsOccluder = true;
		MeshComp->SetCastShadow(true);
		break;
	case EOcclusionRole::Occludee:
		MeshComp->bUseAsOccluder = false;
		MeshComp->SetCastShadow(true);
		break;
	case EOcclusionRole::NonOccluder:
		MeshComp->bUseAsOccluder = false;
		MeshComp->SetCastShadow(false);
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// EAGER GEOMETRY EXTRACTION
// Extracts all geometry data from FlatBuffers at load time to eliminate
// FlatBuffer access during spawn phase. This prevents crashes when
// FlatBuffer pointers become invalid in the async/TileManager path.
//////////////////////////////////////////////////////////////////////////

UStaticMesh* UFragmentsImporter::CreateStaticMeshFromPreExtractedShell(
	const FPreExtractedGeometry& Geometry,
	const FString& AssetName,
	UObject* OuterRef)
{
	if (!Geometry.bIsValid || !Geometry.bIsShell)
	{
		UE_LOG(LogFragments, Warning, TEXT("CreateStaticMeshFromPreExtractedShell: Invalid geometry for %s"), *AssetName);
		return nullptr;
	}

	if (Geometry.Vertices.Num() == 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("CreateStaticMeshFromPreExtractedShell: No vertices for %s"), *AssetName);
		return nullptr;
	}

	// Create StaticMesh object
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(OuterRef, FName(*AssetName), RF_Public | RF_Standalone);
	StaticMesh->InitResources();
	StaticMesh->SetLightingGuid();

	UStaticMeshDescription* StaticMeshDescription = StaticMesh->CreateStaticMeshDescription(OuterRef);
	FMeshDescription& MeshDescription = StaticMeshDescription->GetMeshDescription();
	UStaticMesh::FBuildMeshDescriptionsParams MeshParams;

	// Build Settings
#if WITH_EDITOR
	{
		FStaticMeshSourceModel& SrcModel = StaticMesh->AddSourceModel();
		SrcModel.BuildSettings.bRecomputeNormals = true;
		SrcModel.BuildSettings.bRecomputeTangents = true;
		SrcModel.BuildSettings.bRemoveDegenerates = true;
		SrcModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
		SrcModel.BuildSettings.bBuildReversedIndexBuffer = true;
		SrcModel.BuildSettings.bUseFullPrecisionUVs = false;
		SrcModel.BuildSettings.bGenerateLightmapUVs = true;
		SrcModel.BuildSettings.SrcLightmapIndex = 0;
		SrcModel.BuildSettings.DstLightmapIndex = 1;
		SrcModel.BuildSettings.MinLightmapResolution = 64;
		SrcModel.BuildSettings.DistanceFieldResolutionScale = 0.0f; // Disable distance field generation for runtime meshes
	}
#endif

	MeshParams.bBuildSimpleCollision = true;
	MeshParams.bCommitMeshDescription = true;
	MeshParams.bMarkPackageDirty = true;
	MeshParams.bUseHashAsGuid = false;
#if !WITH_EDITOR
	MeshParams.bFastBuild = true;
#endif

	// Create vertices from pre-extracted data
	TArray<FVertexID> Vertices;
	Vertices.Reserve(Geometry.Vertices.Num());

	for (int32 i = 0; i < Geometry.Vertices.Num(); i++)
	{
		const FVertexID VertId = StaticMeshDescription->CreateVertex();
		StaticMeshDescription->SetVertexPosition(VertId, Geometry.Vertices[i]);
		Vertices.Add(VertId);
	}

	// Add material using pre-extracted color data
	FName MaterialSlotName = AddMaterialToMeshFromRawData(
		StaticMesh,
		Geometry.R, Geometry.G, Geometry.B, Geometry.A,
		Geometry.bIsGlass);
	const FPolygonGroupID PolygonGroupId = StaticMeshDescription->CreatePolygonGroup();
	StaticMeshDescription->SetPolygonGroupMaterialSlotName(PolygonGroupId, MaterialSlotName);

	// Build hole map
	TMap<int32, TArray<TArray<int32>>> ProfileHolesMap;
	for (int32 ProfileIdx = 0; ProfileIdx < Geometry.ProfileHoles.Num(); ProfileIdx++)
	{
		if (Geometry.ProfileHoles[ProfileIdx].Num() > 0)
		{
			ProfileHolesMap.Add(ProfileIdx, Geometry.ProfileHoles[ProfileIdx]);
		}
	}

	// Process profiles
	bool bHasValidPolygons = false;
	for (int32 ProfileIdx = 0; ProfileIdx < Geometry.ProfileIndices.Num(); ProfileIdx++)
	{
		const TArray<int32>& ProfileVertexIndices = Geometry.ProfileIndices[ProfileIdx];

		if (ProfileVertexIndices.Num() < 3)
		{
			continue;
		}

		bool bHasHoles = ProfileHolesMap.Contains(ProfileIdx);

		if (!bHasHoles)
		{
			// Simple polygon - direct creation
			TArray<FVertexInstanceID> TriangleInstance;
			TriangleInstance.Reserve(ProfileVertexIndices.Num());

			for (int32 Idx : ProfileVertexIndices)
			{
				if (Vertices.IsValidIndex(Idx))
				{
					TriangleInstance.Add(MeshDescription.CreateVertexInstance(Vertices[Idx]));
				}
			}

			if (TriangleInstance.Num() >= 3)
			{
				MeshDescription.CreatePolygon(PolygonGroupId, TriangleInstance, {});
				bHasValidPolygons = true;
			}
		}
		else
		{
			// Polygon with holes - tessellate
			TArray<FVector> OutVertices;
			TArray<int32> OutIndices;

			if (TriangulatePolygonWithHoles(
				Geometry.Vertices,
				ProfileVertexIndices,
				ProfileHolesMap[ProfileIdx],
				OutVertices,
				OutIndices))
			{
				// Create new vertices for tessellated geometry
				TMap<int32, FVertexID> TempVertexMap;
				for (int32 j = 0; j < OutVertices.Num(); j++)
				{
					FVertexID VId = StaticMeshDescription->CreateVertex();
					StaticMeshDescription->SetVertexPosition(VId, OutVertices[j]);
					TempVertexMap.Add(j, VId);
				}

				// Create triangles
				for (int32 j = 0; j < OutIndices.Num(); j += 3)
				{
					if (j + 2 < OutIndices.Num())
					{
						TArray<FVertexInstanceID> Triangle;
						Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j]]));
						Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j + 1]]));
						Triangle.Add(MeshDescription.CreateVertexInstance(TempVertexMap[OutIndices[j + 2]]));

						MeshDescription.CreatePolygon(PolygonGroupId, Triangle);
						bHasValidPolygons = true;
					}
				}
			}
			else
			{
				UE_LOG(LogFragments, Warning, TEXT("Tessellation failed for profile %d in mesh %s"), ProfileIdx, *AssetName);
			}
		}
	}

	if (!bHasValidPolygons)
	{
		UE_LOG(LogFragments, Warning, TEXT("CreateStaticMeshFromPreExtractedShell: No valid polygons for %s"), *AssetName);
		return nullptr;
	}

	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MeshDescription);
	FStaticMeshOperations::ComputeTangentsAndNormals(MeshDescription, EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents);

	StaticMesh->BuildFromMeshDescriptions(TArray<const FMeshDescription*>{&MeshDescription}, MeshParams);

	return StaticMesh;
}

void UFragmentsImporter::PreExtractAllGeometry(FFragmentItem& RootItem, const Meshes* MeshesRef)
{
	if (!MeshesRef)
	{
		UE_LOG(LogFragments, Warning, TEXT("PreExtractAllGeometry: MeshesRef is null, skipping extraction"));
		return;
	}

	// Statistics for logging
	int32 TotalSamples = 0;
	int32 SuccessfulExtractions = 0;
	int32 FailedExtractions = 0;

	// Use a stack-based approach to avoid deep recursion
	TArray<FFragmentItem*> ItemStack;
	ItemStack.Add(&RootItem);

	while (ItemStack.Num() > 0)
	{
		FFragmentItem* CurrentItem = ItemStack.Pop();
		if (!CurrentItem)
		{
			continue;
		}

		// Process all samples in this item
		for (FFragmentSample& Sample : CurrentItem->Samples)
		{
			TotalSamples++;

			if (ExtractSampleGeometry(Sample, MeshesRef, CurrentItem->LocalId))
			{
				SuccessfulExtractions++;
			}
			else
			{
				FailedExtractions++;
			}
		}

		// Add children to the stack for processing
		for (FFragmentItem* Child : CurrentItem->FragmentChildren)
		{
			if (Child)
			{
				ItemStack.Add(Child);
			}
		}
	}

	// Calculate approximate memory usage for extracted geometry
	int64 TotalVertexBytes = 0;
	int64 TotalProfileBytes = 0;
	int64 TotalHoleBytes = 0;

	// Re-traverse to calculate memory
	ItemStack.Add(&RootItem);
	while (ItemStack.Num() > 0)
	{
		FFragmentItem* CurrentItem = ItemStack.Pop();
		if (!CurrentItem) continue;

		for (const FFragmentSample& Sample : CurrentItem->Samples)
		{
			const FPreExtractedGeometry& Geom = Sample.ExtractedGeometry;
			if (Geom.bIsValid && Geom.bIsShell)
			{
				// Vertices: TArray overhead (24 bytes) + sizeof(FVector) * count
				TotalVertexBytes += 24 + (sizeof(FVector) * Geom.Vertices.Num());

				// Profile indices: TArray overhead + nested array overhead + actual indices
				TotalProfileBytes += 24;  // Outer array
				for (const TArray<int32>& Profile : Geom.ProfileIndices)
				{
					TotalProfileBytes += 24 + (sizeof(int32) * Profile.Num());
				}

				// Holes: triple-nested arrays
				TotalHoleBytes += 24;  // Outer array
				for (const TArray<TArray<int32>>& HolesForProfile : Geom.ProfileHoles)
				{
					TotalHoleBytes += 24;
					for (const TArray<int32>& Hole : HolesForProfile)
					{
						TotalHoleBytes += 24 + (sizeof(int32) * Hole.Num());
					}
				}
			}
		}

		for (FFragmentItem* Child : CurrentItem->FragmentChildren)
		{
			if (Child) ItemStack.Add(Child);
		}
	}

	int64 TotalBytes = TotalVertexBytes + TotalProfileBytes + TotalHoleBytes;
	float TotalMB = TotalBytes / (1024.0f * 1024.0f);

	UE_LOG(LogFragments, Log, TEXT("=== GEOMETRY PRE-EXTRACTION COMPLETE ==="));
	UE_LOG(LogFragments, Log, TEXT("Total samples: %d"), TotalSamples);
	UE_LOG(LogFragments, Log, TEXT("Successful extractions: %d"), SuccessfulExtractions);
	UE_LOG(LogFragments, Log, TEXT("Failed extractions: %d"), FailedExtractions);
	UE_LOG(LogFragments, Log, TEXT("=== GEOMETRY MEMORY USAGE ==="));
	UE_LOG(LogFragments, Log, TEXT("Vertex data: %.2f MB"), TotalVertexBytes / (1024.0f * 1024.0f));
	UE_LOG(LogFragments, Log, TEXT("Profile data: %.2f MB"), TotalProfileBytes / (1024.0f * 1024.0f));
	UE_LOG(LogFragments, Log, TEXT("Hole data: %.2f MB"), TotalHoleBytes / (1024.0f * 1024.0f));
	UE_LOG(LogFragments, Log, TEXT("Total pre-extracted geometry: %.2f MB"), TotalMB);

	if (FailedExtractions > 0)
	{
		UE_LOG(LogFragments, Warning, TEXT("Some geometry extractions failed. These fragments will be skipped during spawn."));
	}

	// ==========================================
	// GPU INSTANCING: Count instances per RepresentationId + Material combination
	// This runs AFTER geometry extraction so we have access to pre-extracted material data
	// ==========================================
	RepresentationMaterialInstanceCount.Empty();
	{
		TArray<FFragmentItem*> CountStack;
		CountStack.Add(&RootItem);

		while (CountStack.Num() > 0)
		{
			FFragmentItem* CurrentItem = CountStack.Pop();
			if (!CurrentItem) continue;

			for (const FFragmentSample& Sample : CurrentItem->Samples)
			{
				// Only count samples with valid pre-extracted geometry
				if (Sample.RepresentationIndex >= 0 && Sample.ExtractedGeometry.bIsValid)
				{
					// Compute material hash from pre-extracted geometry data
					const FPreExtractedGeometry& Geom = Sample.ExtractedGeometry;
					uint32 MatHash = HashMaterialProperties(Geom.R, Geom.G, Geom.B, Geom.A, Geom.bIsGlass);
					int64 ComboKey = ((int64)Sample.RepresentationIndex) | ((int64)MatHash << 32);

					int32& Count = RepresentationMaterialInstanceCount.FindOrAdd(ComboKey);
					Count++;
				}
			}

			// Add children to count stack
			for (FFragmentItem* Child : CurrentItem->FragmentChildren)
			{
				if (Child) CountStack.Add(Child);
			}
		}
	}

	// Log instancing analysis
	int32 InstanceableCount = 0;
	int32 UniqueInstanceableGroups = 0;
	for (const auto& Pair : RepresentationMaterialInstanceCount)
	{
		if (Pair.Value >= InstancingThreshold)
		{
			InstanceableCount += Pair.Value;
			UniqueInstanceableGroups++;
		}
	}

	UE_LOG(LogFragments, Log, TEXT("=== GPU INSTANCING ANALYSIS ==="));
	UE_LOG(LogFragments, Log, TEXT("Instancing threshold: %d instances"), InstancingThreshold);
	UE_LOG(LogFragments, Log, TEXT("Total unique RepId+Material combinations: %d"), RepresentationMaterialInstanceCount.Num());
	UE_LOG(LogFragments, Log, TEXT("Groups meeting threshold: %d"), UniqueInstanceableGroups);
	UE_LOG(LogFragments, Log, TEXT("Fragments eligible for instancing: %d"), InstanceableCount);
	UE_LOG(LogFragments, Log, TEXT("Estimated draw call reduction: %d -> %d (%.1f%%)"),
		SuccessfulExtractions,
		SuccessfulExtractions - InstanceableCount + UniqueInstanceableGroups,
		InstanceableCount > 0 ? ((float)(InstanceableCount - UniqueInstanceableGroups) / SuccessfulExtractions * 100.0f) : 0.0f);
}

bool UFragmentsImporter::ExtractSampleGeometry(FFragmentSample& Sample, const Meshes* MeshesRef, int32 ItemLocalId)
{
	// Reset the extracted geometry to ensure clean state
	Sample.ExtractedGeometry = FPreExtractedGeometry();

	// Validate indices are valid
	if (Sample.RepresentationIndex < 0 || Sample.MaterialIndex < 0 || Sample.LocalTransformIndex < 0)
	{
		UE_LOG(LogFragments, Verbose, TEXT("ExtractSampleGeometry: Invalid indices for item %d, sample %d"),
			ItemLocalId, Sample.SampleIndex);
		return false;
	}

	// Validate FlatBuffer arrays exist
	if (!MeshesRef->representations() || !MeshesRef->materials() || !MeshesRef->local_transforms())
	{
		UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Missing FlatBuffer arrays for item %d"), ItemLocalId);
		return false;
	}

	// Bounds check representation index
	const uint32 RepCount = MeshesRef->representations()->size();
	if (static_cast<uint32>(Sample.RepresentationIndex) >= RepCount)
	{
		UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: RepresentationIndex %d >= count %u for item %d"),
			Sample.RepresentationIndex, RepCount, ItemLocalId);
		return false;
	}

	// Get representation
	const Representation* representation = MeshesRef->representations()->Get(Sample.RepresentationIndex);
	if (!representation)
	{
		UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Representation is null for item %d"), ItemLocalId);
		return false;
	}

	// Get material
	const uint32 MatCount = MeshesRef->materials()->size();
	if (static_cast<uint32>(Sample.MaterialIndex) >= MatCount)
	{
		UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: MaterialIndex %d >= count %u for item %d"),
			Sample.MaterialIndex, MatCount, ItemLocalId);
		return false;
	}
	const Material* material = MeshesRef->materials()->Get(Sample.MaterialIndex);

	// Get local transform
	const uint32 TransformCount = MeshesRef->local_transforms()->size();
	if (static_cast<uint32>(Sample.LocalTransformIndex) >= TransformCount)
	{
		UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: LocalTransformIndex %d >= count %u for item %d"),
			Sample.LocalTransformIndex, TransformCount, ItemLocalId);
		return false;
	}
	const Transform* local_transform = MeshesRef->local_transforms()->Get(Sample.LocalTransformIndex);

	// Extract material data
	if (material)
	{
		Sample.ExtractedGeometry.R = material->r();
		Sample.ExtractedGeometry.G = material->g();
		Sample.ExtractedGeometry.B = material->b();
		Sample.ExtractedGeometry.A = material->a();
		Sample.ExtractedGeometry.bIsGlass = material->a() < 255;
	}

	// Extract local transform
	if (local_transform)
	{
		Sample.ExtractedGeometry.LocalTransform = UFragmentsUtils::MakeTransform(local_transform);
	}

	// Store representation ID for debugging
	Sample.ExtractedGeometry.RepresentationId = representation->id();

	// Handle Shell geometry
	if (representation->representation_class() == RepresentationClass::RepresentationClass_SHELL)
	{
		Sample.ExtractedGeometry.bIsShell = true;

		if (!MeshesRef->shells())
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: shells() is null for item %d"), ItemLocalId);
			return false;
		}

		const uint32 ShellId = representation->id();
		const uint32 ShellCount = MeshesRef->shells()->size();

		if (ShellId >= ShellCount)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Shell id %u >= count %u for item %d"),
				ShellId, ShellCount, ItemLocalId);
			return false;
		}

		const Shell* shell = MeshesRef->shells()->Get(ShellId);
		if (!shell)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Shell %u is null for item %d"), ShellId, ItemLocalId);
			return false;
		}

		// Validate and extract points
		if (!shell->points() || shell->points()->size() == 0)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Shell %u has no points for item %d"), ShellId, ItemLocalId);
			return false;
		}

		const auto* Points = shell->points();
		const uint32 PointCount = Points->size();

		// Sanity check - prevent extremely large allocations
		constexpr uint32 MaxPointCount = 1000000;
		if (PointCount > MaxPointCount)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Point count %u exceeds limit for item %d"), PointCount, ItemLocalId);
			return false;
		}

		Sample.ExtractedGeometry.Vertices.Reserve(PointCount);
		for (flatbuffers::uoffset_t i = 0; i < PointCount; i++)
		{
			const auto* P = Points->Get(i);
			if (P)
			{
				// Convert to Unreal coordinates: Z-up, cm units
				Sample.ExtractedGeometry.Vertices.Add(FVector(P->x() * 100.0f, P->z() * 100.0f, P->y() * 100.0f));
			}
		}

		// Validate and extract profiles
		const auto* Profiles = shell->profiles();
		if (!Profiles)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Shell %u has no profiles for item %d"), ShellId, ItemLocalId);
			return false;
		}

		const uint32 ProfileCount = Profiles->size();
		constexpr uint32 MaxProfileCount = 100000;
		if (ProfileCount > MaxProfileCount)
		{
			UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Profile count %u exceeds limit for item %d"), ProfileCount, ItemLocalId);
			return false;
		}

		// Build hole map first
		TMap<int32, TArray<TArray<int32>>> ProfileHolesMap;
		if (shell->holes())
		{
			const auto* Holes = shell->holes();
			const uint32 HoleCount = Holes->size();

			for (flatbuffers::uoffset_t j = 0; j < HoleCount && j < MaxProfileCount; j++)
			{
				const auto* Hole = Holes->Get(j);
				if (!Hole) continue;

				int32 ProfileId = Hole->profile_id();

				TArray<int32> HoleIndices;
				if (Hole->indices())
				{
					const uint32 HoleIndexCount = Hole->indices()->size();
					constexpr uint32 MaxIndicesPerProfile = 100000;
					if (HoleIndexCount <= MaxIndicesPerProfile)
					{
						HoleIndices.Reserve(HoleIndexCount);
						for (flatbuffers::uoffset_t k = 0; k < HoleIndexCount; k++)
						{
							HoleIndices.Add(Hole->indices()->Get(k));
						}
					}
				}

				ProfileHolesMap.FindOrAdd(ProfileId).Add(MoveTemp(HoleIndices));
			}
		}

		// Extract profile indices
		Sample.ExtractedGeometry.ProfileIndices.Reserve(ProfileCount);
		Sample.ExtractedGeometry.ProfileHoles.SetNum(ProfileCount);

		bool bHasValidProfile = false;
		for (flatbuffers::uoffset_t i = 0; i < ProfileCount; i++)
		{
			const auto* Profile = Profiles->Get(i);
			TArray<int32> ProfileVertexIndices;

			if (Profile && Profile->indices())
			{
				const auto* Indices = Profile->indices();
				const uint32 IndexCount = Indices->size();

				// Skip profiles with fewer than 3 indices (not a valid polygon)
				if (IndexCount >= 3)
				{
					constexpr uint32 MaxIndicesPerProfile = 100000;
					if (IndexCount <= MaxIndicesPerProfile)
					{
						ProfileVertexIndices.Reserve(IndexCount);
						for (flatbuffers::uoffset_t j = 0; j < IndexCount; j++)
						{
							ProfileVertexIndices.Add(Indices->Get(j));
						}
						bHasValidProfile = true;
					}
					else
					{
						UE_LOG(LogFragments, Verbose, TEXT("ExtractSampleGeometry: Profile %d has %u indices (limit %u), skipping"),
							i, IndexCount, MaxIndicesPerProfile);
					}
				}
			}

			Sample.ExtractedGeometry.ProfileIndices.Add(MoveTemp(ProfileVertexIndices));

			// Copy holes for this profile
			if (TArray<TArray<int32>>* HolesForProfile = ProfileHolesMap.Find(i))
			{
				Sample.ExtractedGeometry.ProfileHoles[i] = *HolesForProfile;
			}
		}

		if (!bHasValidProfile)
		{
			UE_LOG(LogFragments, Verbose, TEXT("ExtractSampleGeometry: No valid profiles for Shell %u, item %d"), ShellId, ItemLocalId);
			return false;
		}

		// Mark as valid
		Sample.ExtractedGeometry.bIsValid = true;
		return true;
	}
	// Handle CircleExtrusion geometry
	else if (representation->representation_class() == RepresentationClass_CIRCLE_EXTRUSION)
	{
		Sample.ExtractedGeometry.bIsShell = false;

		// For CircleExtrusion, we mark as valid but don't pre-extract the complex geometry
		// The spawn phase will still use the FlatBuffer path for CircleExtrusion
		// This is acceptable because CircleExtrusion uses a different (working) code path
		// TODO: Implement full CircleExtrusion extraction if needed
		Sample.ExtractedGeometry.bIsValid = true;
		return true;
	}

	// Unknown representation class
	UE_LOG(LogFragments, Warning, TEXT("ExtractSampleGeometry: Unknown representation class %d for item %d"),
		static_cast<int32>(representation->representation_class()), ItemLocalId);
	return false;
}

// ==========================================
// GPU INSTANCING METHODS (Phase 4)
// ==========================================

bool UFragmentsImporter::ShouldUseInstancing(int32 RepresentationId, uint32 MaterialHash) const
{
	if (!bEnableGPUInstancing)
	{
		return false;
	}

	int64 ComboKey = ((int64)RepresentationId) | ((int64)MaterialHash << 32);
	if (const int32* Count = RepresentationMaterialInstanceCount.Find(ComboKey))
	{
		return *Count >= InstancingThreshold;
	}
	return false;
}

UInstancedStaticMeshComponent* UFragmentsImporter::GetOrCreateISMC(
	int32 RepresentationId, uint32 MaterialHash,
	UStaticMesh* Mesh, UMaterialInstanceDynamic* Material)
{
	if (!Mesh)
	{
		UE_LOG(LogFragments, Warning, TEXT("GetOrCreateISMC: Mesh is null for RepId=%d"), RepresentationId);
		return nullptr;
	}

	int64 ComboKey = ((int64)RepresentationId) | ((int64)MaterialHash << 32);

	// Check if ISMC already exists for this combination
	if (FInstancedMeshGroup* Existing = InstancedMeshGroups.Find(ComboKey))
	{
		return Existing->ISMC;
	}

	// Create host actor if needed (single actor holds all ISMCs for organization)
	if (!ISMCHostActor && OwnerRef)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = OwnerRef;
		ISMCHostActor = OwnerRef->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform::Identity, SpawnParams);

		if (ISMCHostActor)
		{
#if WITH_EDITOR
			ISMCHostActor->SetActorLabel(TEXT("FragmentISMCHost"));
#endif
			UE_LOG(LogFragments, Log, TEXT("Created ISMC host actor"));
		}
	}

	if (!ISMCHostActor)
	{
		UE_LOG(LogFragments, Warning, TEXT("GetOrCreateISMC: Failed to create host actor"));
		return nullptr;
	}

	// Create new ISMC
	UInstancedStaticMeshComponent* ISMC = NewObject<UInstancedStaticMeshComponent>(ISMCHostActor);
	if (!ISMC)
	{
		UE_LOG(LogFragments, Error, TEXT("GetOrCreateISMC: Failed to create ISMC for RepId=%d"), RepresentationId);
		return nullptr;
	}

	ISMC->SetStaticMesh(Mesh);
	if (Material)
	{
		ISMC->SetMaterial(0, Material);
	}
	ISMC->SetMobility(EComponentMobility::Static);

	// Disable Lumen/Distance Field to avoid "Preparing mesh distance fields" delays
	ISMC->bAffectDistanceFieldLighting = false;
	ISMC->bAffectDynamicIndirectLighting = false;
	ISMC->bAffectIndirectLightingWhileHidden = false;

	// CRITICAL: Disable shadow casting - this was causing 280ms+ Shadow Depths overhead!
	// Instanced furniture/repeated elements typically don't need per-instance shadows
	ISMC->SetCastShadow(false);
	ISMC->bCastDynamicShadow = false;
	ISMC->bCastStaticShadow = false;

	// Store LocalId in custom data for picking support
	ISMC->NumCustomDataFloats = 1;

	// Default: most instanced elements are furniture, not occluders
	ISMC->bUseAsOccluder = false;

	// Attach and register
	ISMC->AttachToComponent(ISMCHostActor->GetRootComponent(),
		FAttachmentTransformRules::KeepRelativeTransform);
	ISMC->RegisterComponent();
	ISMCHostActor->AddInstanceComponent(ISMC);

	// Create and store the group
	FInstancedMeshGroup Group;
	Group.ISMC = ISMC;
	Group.RepresentationId = RepresentationId;
	Group.MaterialHash = MaterialHash;
	Group.InstanceCount = 0;
	InstancedMeshGroups.Add(ComboKey, Group);

	UE_LOG(LogFragments, Log, TEXT("Created ISMC for RepId=%d, MatHash=%u"), RepresentationId, MaterialHash);
	return ISMC;
}

void UFragmentsImporter::QueueInstanceForBatchAdd(int32 RepresentationId, uint32 MaterialHash,
	const FTransform& WorldTransform, const FFragmentItem& Item,
	UStaticMesh* Mesh, UMaterialInstanceDynamic* Material)
{
	int64 ComboKey = ((int64)RepresentationId) | ((int64)MaterialHash << 32);

	// Check if ISMC already exists (from previous incremental finalization)
	FInstancedMeshGroup* ExistingGroup = InstancedMeshGroups.Find(ComboKey);
	if (ExistingGroup && ExistingGroup->ISMC != nullptr)
	{
		// ISMC already exists - add directly to it instead of queuing
		// This handles the TileManager streaming case where ISMC was finalized earlier
		AddInstanceToExistingISMC(RepresentationId, MaterialHash, WorldTransform, Item, Mesh, Material);
		return;
	}

	// Get or create group (but DON'T create ISMC yet - that happens in FinalizeAllISMCs or incrementally)
	FInstancedMeshGroup& Group = InstancedMeshGroups.FindOrAdd(ComboKey);
	Group.RepresentationId = RepresentationId;
	Group.MaterialHash = MaterialHash;
	Group.CachedMesh = Mesh;
	Group.CachedMaterial = Material;

	// Queue the instance data for batch addition later
	Group.PendingInstances.Emplace(WorldTransform, Item.LocalId, Item.Guid, Item.Category, Item.ModelGuid, Item.Attributes);
	TotalPendingInstances++;

	// ==========================================
	// INCREMENTAL FINALIZATION: Prevent OOM on large models
	// ==========================================

	// Check 1: If this group has too many pending instances, finalize it now
	if (IncrementalFinalizationThreshold > 0 &&
		Group.PendingInstances.Num() >= IncrementalFinalizationThreshold)
	{
		UE_LOG(LogFragments, Log, TEXT("Incremental finalization triggered: RepId=%d has %d pending instances (threshold=%d)"),
			RepresentationId, Group.PendingInstances.Num(), IncrementalFinalizationThreshold);
		FinalizeISMCGroup(ComboKey, Group);
		return;
	}

	// Check 2: If total pending instances exceed the global limit, finalize largest groups
	if (MaxPendingInstancesTotal > 0 && TotalPendingInstances >= MaxPendingInstancesTotal)
	{
		UE_LOG(LogFragments, Warning, TEXT("Global pending limit reached: %d instances (limit=%d) - finalizing groups"),
			TotalPendingInstances, MaxPendingInstancesTotal);

		// Find and finalize groups with the most pending instances until under limit
		while (TotalPendingInstances >= MaxPendingInstancesTotal * 0.8) // Target 80% to give headroom
		{
			int64 LargestKey = 0;
			int32 LargestCount = 0;

			for (auto& Pair : InstancedMeshGroups)
			{
				if (Pair.Value.ISMC == nullptr && Pair.Value.PendingInstances.Num() > LargestCount)
				{
					LargestKey = Pair.Key;
					LargestCount = Pair.Value.PendingInstances.Num();
				}
			}

			if (LargestCount == 0)
			{
				break; // No more groups to finalize
			}

			FInstancedMeshGroup& LargestGroup = InstancedMeshGroups[LargestKey];
			FinalizeISMCGroup(LargestKey, LargestGroup);
		}
	}
}

void UFragmentsImporter::FinalizeAllISMCs()
{
	if (InstancedMeshGroups.Num() == 0)
	{
		UE_LOG(LogFragments, Log, TEXT("FinalizeAllISMCs: No ISMC groups to finalize"));
		return;
	}

	UE_LOG(LogFragments, Log, TEXT("=== FINALIZING ISMCs: %d groups ==="), InstancedMeshGroups.Num());

	// Create host actor if needed
	if (!ISMCHostActor && OwnerRef)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = OwnerRef;
		ISMCHostActor = OwnerRef->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform::Identity, SpawnParams);
		if (ISMCHostActor)
		{
#if WITH_EDITOR
			ISMCHostActor->SetActorLabel(TEXT("FragmentISMCHost"));
#endif
		}
	}

	if (!ISMCHostActor)
	{
		UE_LOG(LogFragments, Error, TEXT("FinalizeAllISMCs: Failed to create host actor"));
		return;
	}

	int32 TotalInstancesAdded = 0;
	int32 TotalISMCsCreated = 0;

	for (auto& Pair : InstancedMeshGroups)
	{
		FInstancedMeshGroup& Group = Pair.Value;

		// Skip already-finalized groups (from incremental finalization)
		if (Group.ISMC != nullptr)
		{
			UE_LOG(LogFragments, Verbose, TEXT("FinalizeAllISMCs: Skipping already-finalized group RepId=%d (%d instances)"),
				Group.RepresentationId, Group.InstanceCount);
			continue;
		}

		if (Group.PendingInstances.Num() == 0)
		{
			continue;
		}

		if (!Group.CachedMesh)
		{
			UE_LOG(LogFragments, Warning, TEXT("FinalizeAllISMCs: No cached mesh for RepId=%d"), Group.RepresentationId);
			continue;
		}

		// Create ISMC (NOT registered yet - we'll register after adding all instances)
		UInstancedStaticMeshComponent* ISMC = NewObject<UInstancedStaticMeshComponent>(ISMCHostActor);
		if (!ISMC)
		{
			UE_LOG(LogFragments, Error, TEXT("FinalizeAllISMCs: Failed to create ISMC for RepId=%d"), Group.RepresentationId);
			continue;
		}

		ISMC->SetStaticMesh(Group.CachedMesh);
		if (Group.CachedMaterial)
		{
			ISMC->SetMaterial(0, Group.CachedMaterial);
		}
		ISMC->SetMobility(EComponentMobility::Static);

		// Disable expensive features
		ISMC->bAffectDistanceFieldLighting = false;
		ISMC->bAffectDynamicIndirectLighting = false;
		ISMC->bAffectIndirectLightingWhileHidden = false;
		ISMC->SetCastShadow(false);
		ISMC->bCastDynamicShadow = false;
		ISMC->bCastStaticShadow = false;
		ISMC->bUseAsOccluder = false;

		// Custom data for picking
		ISMC->NumCustomDataFloats = 1;

		// Attach to host (still not registered)
		ISMC->AttachToComponent(ISMCHostActor->GetRootComponent(),
			FAttachmentTransformRules::KeepRelativeTransform);

		// Build transform array for batch add
		TArray<FTransform> Transforms;
		Transforms.Reserve(Group.PendingInstances.Num());
		for (const FPendingInstanceData& Pending : Group.PendingInstances)
		{
			Transforms.Add(Pending.WorldTransform);
		}

		// BATCH ADD ALL INSTANCES AT ONCE (this is the key optimization!)
		// This avoids the UE5 per-instance GPU buffer rebuild issue
		TArray<int32> NewIndices = ISMC->AddInstances(Transforms, /*bShouldReturnIndices=*/true, /*bWorldSpace=*/true);

		// NOW register the component (after all instances are added)
		ISMC->RegisterComponent();
		ISMCHostActor->AddInstanceComponent(ISMC);

		// Set custom data and build lookup maps (after registration, with dirty marking disabled)
		for (int32 i = 0; i < Group.PendingInstances.Num(); i++)
		{
			const FPendingInstanceData& Pending = Group.PendingInstances[i];
			int32 InstanceIndex = (i < NewIndices.Num()) ? NewIndices[i] : i;

			// Store LocalId in custom data
			ISMC->SetCustomDataValue(InstanceIndex, 0, static_cast<float>(Pending.LocalId), /*bMarkRenderStateDirty=*/false);

			// Update lookup maps
			Group.InstanceToLocalId.Add(InstanceIndex, Pending.LocalId);
			Group.LocalIdToInstance.Add(Pending.LocalId, InstanceIndex);

			// Create proxy for this instance
			FFragmentProxy Proxy;
			Proxy.ISMC = ISMC;
			Proxy.InstanceIndex = InstanceIndex;
			Proxy.LocalId = Pending.LocalId;
			Proxy.GlobalId = Pending.GlobalId;
			Proxy.Category = Pending.Category;
			Proxy.ModelGuid = Pending.ModelGuid;
			Proxy.Attributes = Pending.Attributes;
			Proxy.WorldTransform = Pending.WorldTransform;
			LocalIdToProxyMap.Add(Pending.LocalId, Proxy);
		}

		// Mark render state dirty once for all custom data
		ISMC->MarkRenderStateDirty();

		Group.ISMC = ISMC;
		Group.InstanceCount = Group.PendingInstances.Num();

		TotalInstancesAdded += Group.PendingInstances.Num();
		TotalISMCsCreated++;

		// Clear pending instances to free memory
		Group.PendingInstances.Empty();

		UE_LOG(LogFragments, Log, TEXT("Created ISMC for RepId=%d with %d instances"),
			Group.RepresentationId, Group.InstanceCount);
	}

	UE_LOG(LogFragments, Log, TEXT("=== ISMC FINALIZATION COMPLETE: %d ISMCs, %d total instances ==="),
		TotalISMCsCreated, TotalInstancesAdded);

	// Reset pending counter
	TotalPendingInstances = 0;
}

int32 UFragmentsImporter::FinalizeISMCGroup(int64 ComboKey, FInstancedMeshGroup& Group)
{
	// Skip if already finalized or no pending instances
	if (Group.ISMC != nullptr || Group.PendingInstances.Num() == 0)
	{
		return 0;
	}

	// Ensure host actor exists
	if (!ISMCHostActor && OwnerRef)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = OwnerRef;
		ISMCHostActor = OwnerRef->GetWorld()->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform::Identity, SpawnParams);
		if (ISMCHostActor)
		{
#if WITH_EDITOR
			ISMCHostActor->SetActorLabel(TEXT("FragmentISMCHost"));
#endif
		}
	}

	if (!ISMCHostActor)
	{
		UE_LOG(LogFragments, Error, TEXT("FinalizeISMCGroup: Failed to create host actor"));
		return -1;
	}

	if (!Group.CachedMesh)
	{
		UE_LOG(LogFragments, Warning, TEXT("FinalizeISMCGroup: No cached mesh for RepId=%d"), Group.RepresentationId);
		return -1;
	}

	// Create ISMC (NOT registered yet - we'll register after adding all instances)
	UInstancedStaticMeshComponent* ISMC = NewObject<UInstancedStaticMeshComponent>(ISMCHostActor);
	if (!ISMC)
	{
		UE_LOG(LogFragments, Error, TEXT("FinalizeISMCGroup: Failed to create ISMC for RepId=%d"), Group.RepresentationId);
		return -1;
	}

	ISMC->SetStaticMesh(Group.CachedMesh);
	if (Group.CachedMaterial)
	{
		ISMC->SetMaterial(0, Group.CachedMaterial);
	}
	ISMC->SetMobility(EComponentMobility::Static);

	// Disable expensive features
	ISMC->bAffectDistanceFieldLighting = false;
	ISMC->bAffectDynamicIndirectLighting = false;
	ISMC->bAffectIndirectLightingWhileHidden = false;
	ISMC->SetCastShadow(false);
	ISMC->bCastDynamicShadow = false;
	ISMC->bCastStaticShadow = false;
	ISMC->bUseAsOccluder = false;

	// Custom data for picking
	ISMC->NumCustomDataFloats = 1;

	// Attach to host (still not registered)
	ISMC->AttachToComponent(ISMCHostActor->GetRootComponent(),
		FAttachmentTransformRules::KeepRelativeTransform);

	// Build transform array for batch add
	TArray<FTransform> Transforms;
	Transforms.Reserve(Group.PendingInstances.Num());
	for (const FPendingInstanceData& Pending : Group.PendingInstances)
	{
		Transforms.Add(Pending.WorldTransform);
	}

	// BATCH ADD ALL INSTANCES AT ONCE
	TArray<int32> NewIndices = ISMC->AddInstances(Transforms, /*bShouldReturnIndices=*/true, /*bWorldSpace=*/true);

	// NOW register the component (after all instances are added)
	ISMC->RegisterComponent();
	ISMCHostActor->AddInstanceComponent(ISMC);

	// Set custom data and build lookup maps
	for (int32 i = 0; i < Group.PendingInstances.Num(); i++)
	{
		const FPendingInstanceData& Pending = Group.PendingInstances[i];
		int32 InstanceIndex = (i < NewIndices.Num()) ? NewIndices[i] : i;

		// Store LocalId in custom data
		ISMC->SetCustomDataValue(InstanceIndex, 0, static_cast<float>(Pending.LocalId), /*bMarkRenderStateDirty=*/false);

		// Update lookup maps
		Group.InstanceToLocalId.Add(InstanceIndex, Pending.LocalId);
		Group.LocalIdToInstance.Add(Pending.LocalId, InstanceIndex);

		// Create proxy for this instance
		FFragmentProxy Proxy;
		Proxy.ISMC = ISMC;
		Proxy.InstanceIndex = InstanceIndex;
		Proxy.LocalId = Pending.LocalId;
		Proxy.GlobalId = Pending.GlobalId;
		Proxy.Category = Pending.Category;
		Proxy.ModelGuid = Pending.ModelGuid;
		Proxy.Attributes = Pending.Attributes;
		Proxy.WorldTransform = Pending.WorldTransform;
		LocalIdToProxyMap.Add(Pending.LocalId, Proxy);
	}

	// Mark render state dirty once for all custom data
	ISMC->MarkRenderStateDirty();

	Group.ISMC = ISMC;
	int32 InstancesAdded = Group.PendingInstances.Num();
	Group.InstanceCount = InstancesAdded;

	// Update pending counter
	TotalPendingInstances -= InstancesAdded;
	TotalPendingInstances = FMath::Max(0, TotalPendingInstances);

	// Clear pending instances to free memory
	Group.PendingInstances.Empty();
	Group.PendingInstances.Shrink();  // Release allocated memory

	UE_LOG(LogFragments, Log, TEXT("FinalizeISMCGroup: Created ISMC for RepId=%d with %d instances (incremental)"),
		Group.RepresentationId, InstancesAdded);

	return InstancesAdded;
}

bool UFragmentsImporter::AddInstanceToExistingISMC(int32 RepresentationId, uint32 MaterialHash,
	const FTransform& WorldTransform, const FFragmentItem& Item,
	UStaticMesh* Mesh, UMaterialInstanceDynamic* Material)
{
	int64 ComboKey = ((int64)RepresentationId) | ((int64)MaterialHash << 32);

	FInstancedMeshGroup* Group = InstancedMeshGroups.Find(ComboKey);
	if (!Group || !Group->ISMC)
	{
		// ISMC not yet created - queue for batch addition instead
		QueueInstanceForBatchAdd(RepresentationId, MaterialHash, WorldTransform, Item, Mesh, Material);
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = Group->ISMC;
	if (!ISMC || !IsValid(ISMC))
	{
		UE_LOG(LogFragments, Warning, TEXT("AddInstanceToExistingISMC: ISMC invalid for RepId=%d"), RepresentationId);
		return false;
	}

	// Add single instance to existing ISMC
	// Note: This is less efficient than batch add, but necessary for streaming
	int32 NewIndex = ISMC->AddInstance(WorldTransform, /*bWorldSpace=*/true);
	if (NewIndex == INDEX_NONE)
	{
		UE_LOG(LogFragments, Warning, TEXT("AddInstanceToExistingISMC: Failed to add instance for RepId=%d"), RepresentationId);
		return false;
	}

	// Set custom data
	ISMC->SetCustomDataValue(NewIndex, 0, static_cast<float>(Item.LocalId), /*bMarkRenderStateDirty=*/true);

	// Update lookup maps
	Group->InstanceToLocalId.Add(NewIndex, Item.LocalId);
	Group->LocalIdToInstance.Add(Item.LocalId, NewIndex);
	Group->InstanceCount++;

	// Create proxy
	FFragmentProxy Proxy;
	Proxy.ISMC = ISMC;
	Proxy.InstanceIndex = NewIndex;
	Proxy.LocalId = Item.LocalId;
	Proxy.GlobalId = Item.Guid;
	Proxy.Category = Item.Category;
	Proxy.ModelGuid = Item.ModelGuid;
	Proxy.Attributes = Item.Attributes;
	Proxy.WorldTransform = WorldTransform;
	LocalIdToProxyMap.Add(Item.LocalId, Proxy);

	return true;
}

FFindResult UFragmentsImporter::FindFragmentByLocalIdUnified(int32 LocalId, const FString& ModelGuid)
{
	// Check proxy map first (instanced fragments)
	if (FFragmentProxy* Proxy = LocalIdToProxyMap.Find(LocalId))
	{
		if (Proxy->ModelGuid == ModelGuid)
		{
			return FFindResult::FromProxy(*Proxy);
		}
	}

	// Check actor map (non-instanced fragments)
	if (FFragmentLookup* Lookup = ModelFragmentsMap.Find(ModelGuid))
	{
		if (AFragment** Actor = Lookup->Fragments.Find(LocalId))
		{
			if (*Actor)
			{
				return FFindResult::FromActor(*Actor);
			}
		}
	}

	return FFindResult::NotFound();
}