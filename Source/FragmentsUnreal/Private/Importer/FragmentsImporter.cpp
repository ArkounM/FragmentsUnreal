


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

AFragment* UFragmentsImporter::SpawnSingleFragment(const FFragmentItem& Item, AActor* ParentActor, const Meshes* MeshesRef, bool bSaveMeshes)
{
	if (!ParentActor) return nullptr;

	// Create AFragment actor
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
	const TArray<FFragmentSample>& Samples = FragmentModel->GetSamples();

	if (Samples.Num() > 0)
	{
		for (int32 i = 0; i < Samples.Num(); i++)
		{
			const FFragmentSample& Sample = Samples[i];

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
			// Create new mesh using deduplication
			else
			{
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
					UPackage* MeshPackage = CreatePackage(*PackagePath);

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

						// Add this instance to the template
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
				else
				{
					// Fallback to old method if extraction failed
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
				const uint8 MaterialAlpha = material ? material->a() : 255;
				const EOcclusionRole Role = UFragmentOcclusionClassifier::ClassifyFragment(
					Item.Category, MaterialAlpha);

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
	if (PendingSpawnQueue.Num() == 0)
	{
		// Spawning complete
		UE_LOG(LogFragments, Log, TEXT("Chunked spawning complete! Total fragments: %d"), FragmentsSpawned);

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
		AFragment* SpawnedActor = SpawnSingleFragment(Task.FragmentItem, Task.ParentActor, CurrentMeshesRef, bCurrentSaveMeshes);

		if (SpawnedActor)
		{
			// Add children to queue with this actor as parent
			for (FFragmentItem* Child : Task.FragmentItem.FragmentChildren)
			{
				PendingSpawnQueue.Add(FFragmentSpawnTask(*Child, SpawnedActor));
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
	if (!RefMaterial || !CreatedMesh)return FName();

	bool HasTransparency = false;
	float R = RefMaterial->r() / 255.f;
	float G = RefMaterial->g() / 255.f;
	float B = RefMaterial->b() / 255.f;
	float A = RefMaterial->a() / 255.f;

	UMaterialInterface* Material = nullptr;
	if (A < 1)
	{
		///Script/Engine.Material'/FragmentsUnreal/Materials/M_BaseFragmentMaterial.M_BaseFragmentMaterial'
		Material = BaseGlassMaterial;
		HasTransparency = true;
	}
	else
	{
		Material = BaseMaterial;
	}
	if (!Material)
	{
		UE_LOG(LogFragments, Error, TEXT("Unable to load Base Material"));
		return FName();
	}

	UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(Material, CreatedMesh);
	if (!DynamicMaterial)
	{
		UE_LOG(LogFragments, Error, TEXT("Failed to create dynamic material."));
		return FName();
	}

	if (HasTransparency)
	{
		DynamicMaterial->SetScalarParameterValue(TEXT("Opacity"), A);
	}

	DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), FVector4(R, G, B, A));


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