#include "Spatial/FragmentHISMManager.h"
#include "Spatial/FragmentTile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentHISM, Log, All);

UFragmentHISMManager::UFragmentHISMManager()
	: RootActor(nullptr)
	, TotalInstanceCount(0)
{
}

void UFragmentHISMManager::Initialize(AActor* OwnerActor, const FString& InModelGuid)
{
	if (!OwnerActor)
	{
		UE_LOG(LogFragmentHISM, Error, TEXT("Initialize: Null OwnerActor"));
		return;
	}

	RootActor = OwnerActor;
	ModelGuid = InModelGuid;

	UE_LOG(LogFragmentHISM, Log, TEXT("Initialized HISM Manager for model: %s"), *ModelGuid);
}

UHierarchicalInstancedStaticMeshComponent* UFragmentHISMManager::GetOrCreateHISMForTile(
	UFragmentTile* Tile,
	UStaticMesh* Mesh,
	UMaterialInterface* Material)
{
	if (!Tile || !Mesh || !RootActor)
	{
		return nullptr;
	}

	// Check if we already have a HISM for this tile + mesh combo
	TMap<UStaticMesh*, UHierarchicalInstancedStaticMeshComponent*>* TileMeshMap = TileHISMComponents.Find(Tile);
	if (TileMeshMap)
	{
		UHierarchicalInstancedStaticMeshComponent** ExistingHISM = TileMeshMap->Find(Mesh);
		if (ExistingHISM && *ExistingHISM)
		{
			return *ExistingHISM;
		}
	}

	// Create new HISM component for this tile + mesh
	UHierarchicalInstancedStaticMeshComponent* NewHISM = CreateHISMComponent(Mesh, Material);
	if (!NewHISM)
	{
		return nullptr;
	}

	// Store in tile map
	if (!TileMeshMap)
	{
		TileHISMComponents.Add(Tile, TMap<UStaticMesh*, UHierarchicalInstancedStaticMeshComponent*>());
		TileMeshMap = TileHISMComponents.Find(Tile);
	}
	TileMeshMap->Add(Mesh, NewHISM);

	// Track globally
	AllHISMComponents.Add(NewHISM);

	UE_LOG(LogFragmentHISM, Verbose, TEXT("Created HISM for tile %p, mesh %s (total HISMs: %d)"),
	       Tile, *Mesh->GetName(), AllHISMComponents.Num());

	return NewHISM;
}

UHierarchicalInstancedStaticMeshComponent* UFragmentHISMManager::CreateHISMComponent(
	UStaticMesh* Mesh,
	UMaterialInterface* Material)
{
	if (!RootActor || !Mesh)
	{
		return nullptr;
	}

	// Create HISM component
	UHierarchicalInstancedStaticMeshComponent* HISM = NewObject<UHierarchicalInstancedStaticMeshComponent>(
		RootActor,
		UHierarchicalInstancedStaticMeshComponent::StaticClass(),
		NAME_None,
		RF_Transient
	);

	if (!HISM)
	{
		UE_LOG(LogFragmentHISM, Error, TEXT("Failed to create HISM component"));
		return nullptr;
	}

	// Configure HISM
	HISM->SetStaticMesh(Mesh);
	HISM->SetMobility(EComponentMobility::Static);
	HISM->SetCastShadow(true);
	HISM->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	HISM->SetCollisionProfileName(TEXT("BlockAll"));

	// Apply material if provided
	if (Material)
	{
		HISM->SetMaterial(0, Material);
	}

	// Attach to root actor
	HISM->AttachToComponent(RootActor->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	HISM->RegisterComponent();

	// Start hidden (will be shown when tile becomes visible)
	HISM->SetVisibility(false);

	return HISM;
}

int32 UFragmentHISMManager::AddInstance(UFragmentTile* Tile, int32 LocalID, int32 SampleIndex,
                                         UStaticMesh* Mesh, const FTransform& WorldTransform,
                                         UMaterialInterface* Material)
{
	if (!Tile || !Mesh)
	{
		return -1;
	}

	// Get or create HISM for this tile + mesh
	UHierarchicalInstancedStaticMeshComponent* HISM = GetOrCreateHISMForTile(Tile, Mesh, Material);
	if (!HISM)
	{
		UE_LOG(LogFragmentHISM, Error, TEXT("Failed to get HISM for tile %p, mesh %s"), Tile, *Mesh->GetName());
		return -1;
	}

	// Add instance to HISM
	const int32 InstanceIndex = HISM->AddInstance(WorldTransform);
	if (InstanceIndex < 0)
	{
		UE_LOG(LogFragmentHISM, Error, TEXT("Failed to add instance to HISM"));
		return -1;
	}

	// Create instance record
	FInstanceRecord Record;
	Record.InstanceIndex = InstanceIndex;
	Record.Mesh = Mesh;
	Record.WorldTransform = WorldTransform;
	Record.LocalID = LocalID;
	Record.SampleIndex = SampleIndex;

	// Store in tile records
	TArray<FInstanceRecord>* TileRecords = TileInstanceRecords.Find(Tile);
	if (!TileRecords)
	{
		TileInstanceRecords.Add(Tile, TArray<FInstanceRecord>());
		TileRecords = TileInstanceRecords.Find(Tile);
	}
	TileRecords->Add(Record);

	// Store in LocalID map (for selection/queries)
	TArray<FInstanceRecord>* LocalIDRecords = LocalIDToInstances.Find(LocalID);
	if (!LocalIDRecords)
	{
		LocalIDToInstances.Add(LocalID, TArray<FInstanceRecord>());
		LocalIDRecords = LocalIDToInstances.Find(LocalID);
	}
	LocalIDRecords->Add(Record);

	// Store in HISM→LocalID map (for ray-cast)
	TMap<int32, int32>* HISMMap = HISMInstanceToLocalID.Find(HISM);
	if (!HISMMap)
	{
		HISMInstanceToLocalID.Add(HISM, TMap<int32, int32>());
		HISMMap = HISMInstanceToLocalID.Find(HISM);
	}
	HISMMap->Add(InstanceIndex, LocalID);

	TotalInstanceCount++;

	return InstanceIndex;
}

void UFragmentHISMManager::SetTileVisibility(UFragmentTile* Tile, bool bVisible)
{
	if (!Tile)
	{
		return;
	}

	// Find all HISM components for this tile
	TMap<UStaticMesh*, UHierarchicalInstancedStaticMeshComponent*>* TileMeshMap = TileHISMComponents.Find(Tile);
	if (!TileMeshMap)
	{
		UE_LOG(LogFragmentHISM, Warning, TEXT("SetTileVisibility: No HISM components for tile %p"), Tile);
		return;
	}

	// Set visibility on all HISM components for this tile
	int32 ComponentCount = 0;
	for (auto& Pair : *TileMeshMap)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
		if (HISM)
		{
			HISM->SetVisibility(bVisible);
			ComponentCount++;
		}
	}

	UE_LOG(LogFragmentHISM, Verbose, TEXT("Set tile %p visibility to %s (%d HISM components)"),
	       Tile, bVisible ? TEXT("VISIBLE") : TEXT("HIDDEN"), ComponentCount);
}

void UFragmentHISMManager::RemoveTileInstances(UFragmentTile* Tile)
{
	if (!Tile)
	{
		return;
	}

	// Find tile's HISM components
	TMap<UStaticMesh*, UHierarchicalInstancedStaticMeshComponent*>* TileMeshMap = TileHISMComponents.Find(Tile);
	if (!TileMeshMap)
	{
		return;
	}

	// Get instance records for cleanup
	TArray<FInstanceRecord>* TileRecords = TileInstanceRecords.Find(Tile);
	int32 InstancesRemoved = 0;

	if (TileRecords)
	{
		// Remove from LocalID map
		for (const FInstanceRecord& Record : *TileRecords)
		{
			TArray<FInstanceRecord>* LocalIDRecords = LocalIDToInstances.Find(Record.LocalID);
			if (LocalIDRecords)
			{
				LocalIDRecords->RemoveAll([&Record](const FInstanceRecord& R) {
					return R.InstanceIndex == Record.InstanceIndex && R.Mesh == Record.Mesh;
				});

				if (LocalIDRecords->Num() == 0)
				{
					LocalIDToInstances.Remove(Record.LocalID);
				}
			}
		}

		InstancesRemoved = TileRecords->Num();
		TotalInstanceCount -= InstancesRemoved;

		// Clear tile records
		TileInstanceRecords.Remove(Tile);
	}

	// Destroy HISM components for this tile
	for (auto& Pair : *TileMeshMap)
	{
		UHierarchicalInstancedStaticMeshComponent* HISM = Pair.Value;
		if (HISM)
		{
			// Remove from HISM→LocalID map
			HISMInstanceToLocalID.Remove(HISM);

			// Remove from global list
			AllHISMComponents.Remove(HISM);

			// Destroy component
			HISM->DestroyComponent();
		}
	}

	// Remove tile from map
	TileHISMComponents.Remove(Tile);

	UE_LOG(LogFragmentHISM, Log, TEXT("Removed tile %p: %d instances, %d HISM components destroyed"),
	       Tile, InstancesRemoved, TileMeshMap->Num());
}

TArray<FInstanceRecord>* UFragmentHISMManager::GetInstanceRecords(int32 LocalID)
{
	return LocalIDToInstances.Find(LocalID);
}

int32 UFragmentHISMManager::GetLocalIDForInstance(UHierarchicalInstancedStaticMeshComponent* HISM, int32 InstanceIndex)
{
	if (!HISM)
	{
		return -1;
	}

	TMap<int32, int32>* HISMMap = HISMInstanceToLocalID.Find(HISM);
	if (!HISMMap)
	{
		return -1;
	}

	int32* LocalIDPtr = HISMMap->Find(InstanceIndex);
	return LocalIDPtr ? *LocalIDPtr : -1;
}

int64 UFragmentHISMManager::GetEstimatedMemoryUsage() const
{
	int64 TotalBytes = 0;

	// Instance data: ~64 bytes per instance (FTransform = 48 bytes + overhead)
	TotalBytes += TotalInstanceCount * 64;

	// HISM components: ~1KB per component
	TotalBytes += AllHISMComponents.Num() * 1024;

	// Tracking maps (approximate)
	TotalBytes += TileHISMComponents.Num() * 256;
	TotalBytes += TileInstanceRecords.Num() * 512;
	TotalBytes += LocalIDToInstances.Num() * 128;

	return TotalBytes;
}
