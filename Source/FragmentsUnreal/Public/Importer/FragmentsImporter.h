

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Importer/DeferredPackageSaveManager.h"
#include "Importer/FragmentsAsyncLoader.h" // Added for async delegate

#include "FragmentsImporter.generated.h"


FRAGMENTSUNREAL_API DECLARE_LOG_CATEGORY_EXTERN(LogFragments, Log, All);

// Forward Declarations
class UFragmentsAsyncLoader;
class UFragmentTileManager;

// Use FlatBuffers Model type
using Model = ::Model;

// Task for spawning a signle fragment
struct FFragmentSpawnTask
{
	FFragmentItem FragmentItem;
	AActor* ParentActor;

	FFragmentSpawnTask() : ParentActor(nullptr) {}
	FFragmentSpawnTask(const FFragmentItem& InItem, AActor* InParent)
		: FragmentItem(InItem), ParentActor(InParent) {}
};

UCLASS()
class FRAGMENTSUNREAL_API UFragmentsImporter :public UObject
{
	GENERATED_BODY()

public:
	// Adding new async loading function
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	void ProcessFragmentAsync(const FString& FragmentPath, AActor* Owner, FOnFragmentLoadComplete OnComplete);

	UFragmentsImporter();

	FString Process(AActor* OwnerA, const FString& FragPath, TArray<AFragment*>& OutFragments, bool bSaveMeshes = true);
	void SetOwnerRef(AActor* NewOwnerRef) { OwnerRef = NewOwnerRef; }
	void GetItemData(AFragment*& InFragment);
	void GetItemData(FFragmentItem* InFragmentItem);
	TArray<FItemAttribute> GetItemPropertySets(AFragment* InFragment);
	AFragment* GetItemByLocalId(int32 LocalId, const FString& ModelGuid);
	FFragmentItem* GetFragmentItemByLocalId(int32 LocalId, const FString& InModelGuid);
	FString LoadFragment(const FString& FragPath);
	void ProcessLoadedFragment(const FString& ModelGuid, AActor* InOwnerRef, bool bInSaveMesh);
	TArray<int32> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);
	void UnloadFragment(const FString& ModelGuid);

	/**
	 * Update tile streaming based on camera parameters
	 * Called by FragmentsComponent to update visible tiles
	 */
	void UpdateTileStreaming(const FVector& CameraLocation, const FRotator& CameraRotation,
	                         float FOV, float AspectRatio);

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}
	// Non-const version for async loader to add models
	FORCEINLINE TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels_Mutable()
	{
		return FragmentModels;
	}

	// Get model wrapper by GUID
	FORCEINLINE class UFragmentModelWrapper* GetFragmentModel(const FString& ModelGuid) const
	{
		UFragmentModelWrapper* const* Found = FragmentModels.Find(ModelGuid);
		return Found ? *Found : nullptr;
	}

	// Get owner actor reference
	FORCEINLINE AActor* GetOwnerRef() const { return OwnerRef; }

	/**
	 * Spawn a single fragment (used by TileManager for streaming)
	 * @param Item Fragment item to spawn
	 * @param ParentActor Parent actor to attach to
	 * @param MeshesRef Meshes reference from FlatBuffers model
	 * @param bSaveMeshes Whether to save meshes to disk
	 * @return Spawned fragment actor or nullptr
	 */
	AFragment* SpawnSingleFragment(const FFragmentItem& Item, AActor* ParentActor, const Meshes* MeshesRef, bool bSaveMeshes);

protected:
	// Call when Async Loading Completes
	UFUNCTION()
	void OnAsyncLoadComplete(bool bSuccess, const FString& ErrorMessage, const FString& ModelGuid);

private:

	void CollectPropertiesRecursive(const Model* InModel, int32 StartLocalId, TSet<int32>& Visited, TArray<FItemAttribute>& OutAttributes);
	void SpawnStaticMesh(UStaticMesh* StaticMesh, const Transform* LocalTransform, const Transform* GlobalTransform, AActor* Owner, FName OptionalTag = FName());
	void SpawnFragmentModel(AFragment* InFragmentModel, AActor* InParent, const Meshes* MeshesRef, bool bSaveMeshes);
	void SpawnFragmentModel(FFragmentItem InFragmentItem, AActor* InParent, const Meshes* MeshesRef, bool bSaveMeshes);
	UStaticMesh* CreateStaticMeshFromShell(
		const Shell* ShellRef,
		const Material* RefMaterial,
		const FString& AssetName,
		UObject* OuterRef
	);
	UStaticMesh* CreateStaticMeshFromCircleExtrusion(
		const CircleExtrusion* CircleExtrusion,
		const Material* RefMaterial,
		const FString& AssetName,
		UObject* OuterRef
	);

	FName AddMaterialToMesh(UStaticMesh*& CreatedMesh, const Material* RefMaterial);

	bool TriangulatePolygonWithHoles(const TArray<FVector>& Points,
		const TArray<int32>& Profiles,
		const TArray<TArray<int32>>& Holes,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutIndices);

	void BuildFullCircleExtrusion(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion, const Material* RefMaterial, UStaticMesh* StaticMesh);

	void BuildLineOnlyMesh(UStaticMeshDescription& StaticMeshDescription, const CircleExtrusion* CircleExtrusion);

	TArray<FVector> SampleRingPoints(const FVector& Center, const FVector XDir, const FVector& YDir, float Radius, int SegmentCount, float ApertureRadians);

	void SavePackagesWithProgress(const TArray<UPackage*>& InPackagesToSave);

	// Async Loader Instance
	UPROPERTY()
	UFragmentsAsyncLoader* AsyncLoader;

	// Chunked Spawning Functions
	// Build flat queue of all fragments to spawn (recursive)
	void BuildSpawnQueue(const FFragmentItem& Item, AActor* ParentActor, TArray<FFragmentSpawnTask>& OutQueue);

	// Process one chunk of spawning
	void ProcessSpawnChunk();

	// Process spawn chunks for all tile managers (called by timer)
	void ProcessAllTileManagerChunks();

	//Start Chunk Spawning
	void StartChunkedSpawning(const FFragmentItem& RootItem, AActor* OwnerActor, const Meshes* MeshesRef, bool bSaveMeshes);

private:

	UPROPERTY()
	AActor* OwnerRef = nullptr;

	UPROPERTY()
	UMaterialInterface* BaseMaterial = nullptr;
	
	UPROPERTY()
	UMaterialInterface* BaseGlassMaterial = nullptr;

	UPROPERTY()
	TMap<FString, class UFragmentModelWrapper*> FragmentModels;

	UPROPERTY()
	TMap<FString, FFragmentLookup> ModelFragmentsMap;

	// Tile managers for streaming (one per loaded model)
	UPROPERTY()
	TMap<FString, UFragmentTileManager*> TileManagers;

	UPROPERTY()
	TMap<FString, UStaticMesh*> MeshCache;

	UPROPERTY()
	TArray<UPackage*> PackagesToSave;

	FDeferredPackageSaveManager DeferredSaveManager;

	// Pending Completion Callback
	FOnFragmentLoadComplete PendingCallback;

	// Owner actor for pending async spawn
	UPROPERTY()
	AActor* PendingOwner;

	// CHUNKED SPAWNING MEMBERS
	// Queue of fragments waiting to be spawned
	TArray<FFragmentSpawnTask> PendingSpawnQueue;

	// Meshes referencce for spawning
	const Meshes* CurrentMeshesRef = nullptr;

	// Save meshes flag
	bool bCurrentSaveMeshes = false;

	// Current model GUID being spawned 
	FString CurrentSpawningModelGuid;

	// Timer Handle for chunked spawning
	FTimerHandle SpawnChunkTimerHandle;

	// How many fragments to spawn per tick
	int32 FragmentsPerChunk = 1;

	// Current spawn progress (0.0 to 1.0)
	float SpawnProgress = 0.0f;

	// Total fragments to spawn
	int32 TotalFragmentsToSpawn = 0;

	// Fragments spawned thus far
	int32 FragmentsSpawned = 0;

public:

	TArray<class AFragment*> FragmentActors;

};
