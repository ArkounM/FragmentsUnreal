

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Index/index_generated.h"
#include "Utils/FragmentsUtils.h"
#include "Utils/FrameBudgetCoordinator.h"
#include "Importer/DeferredPackageSaveManager.h"
#include "Importer/FragmentsAsyncLoader.h" // Added for async delegate
#include "Utils/FragmentsLog.h"
#include "FragmentsImporter.generated.h"

// Forward Declarations
class UFragmentsAsyncLoader;
class UFragmentTileManager;
class AFragment;
class UHierarchicalInstancedStaticMeshComponent;

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

	// ==========================================
	// GPU INSTANCING API (Phase 4)
	// ==========================================

	/**
	 * Unified lookup by LocalId - works for both actors and instanced proxies.
	 * @param LocalId The BIM element's local ID
	 * @param ModelGuid The model GUID
	 * @return FFindResult containing either an actor or proxy data
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments")
	FFindResult FindFragmentByLocalIdUnified(int32 LocalId, const FString& ModelGuid);

	/**
	 * Check if a representation+material combination should use GPU instancing.
	 * @return true if instance count >= InstancingThreshold
	 */
	bool ShouldUseInstancing(int32 RepresentationId, uint32 MaterialHash) const;

	/**
	 * Get or create an HISMC for a representation+material combination.
	 * Uses Hierarchical ISM for per-cluster culling performance.
	 * @param RepresentationId The FlatBuffers representation ID (unique geometry)
	 * @param MaterialHash Hash of material properties
	 * @param Mesh The static mesh to use for this HISMC
	 * @param Material The material instance to apply
	 * @return The HISMC (existing or newly created), or nullptr on failure
	 */
	UHierarchicalInstancedStaticMeshComponent* GetOrCreateISMC(int32 RepresentationId, uint32 MaterialHash,
		UStaticMesh* Mesh, UMaterialInstanceDynamic* Material);

	/**
	 * Queue an instance to be batch-added later (during spawn phase).
	 * Instances are collected in PendingInstances arrays and batch-added
	 * when FinalizeAllISMCs() is called after spawning completes.
	 * @param MaterialAlpha Alpha value from the material (0-255) for occlusion classification
	 */
	void QueueInstanceForBatchAdd(int32 RepresentationId, uint32 MaterialHash,
		const FTransform& WorldTransform, const FFragmentItem& Item,
		UStaticMesh* Mesh, UMaterialInstanceDynamic* Material, uint8 MaterialAlpha);

	/**
	 * Finalize all ISMCs by batch-adding all pending instances.
	 * Called once after all fragment spawning is complete.
	 * This avoids the UE5 performance issue of per-instance GPU buffer rebuilds.
	 */
	void FinalizeAllISMCs();

	/**
	 * Finalize a single ISMC group by batch-adding its pending instances.
	 * Used for incremental finalization when pending count exceeds threshold.
	 * @param ComboKey The RepresentationId + MaterialHash key
	 * @param Group The ISMC group to finalize
	 * @return Number of instances added, or -1 on failure
	 */
	int32 FinalizeISMCGroup(int64 ComboKey, FInstancedMeshGroup& Group);

	/**
	 * Add a single instance to an existing (already finalized) ISMC.
	 * Used by TileManager streaming path for incremental instance addition.
	 * @param RepresentationId The geometry representation ID
	 * @param MaterialHash Hash of material properties
	 * @param WorldTransform Transform for the new instance
	 * @param Item Fragment item data for proxy creation
	 * @param Mesh The static mesh (must match existing ISMC mesh)
	 * @param Material The material instance
	 * @param MaterialAlpha Alpha value from the material (0-255) for occlusion classification
	 * @return true if instance was added successfully
	 */
	bool AddInstanceToExistingISMC(int32 RepresentationId, uint32 MaterialHash,
		const FTransform& WorldTransform, const FFragmentItem& Item,
		UStaticMesh* Mesh, UMaterialInstanceDynamic* Material, uint8 MaterialAlpha);
	FString LoadFragment(const FString& FragPath);
	void ProcessLoadedFragment(const FString& ModelGuid, AActor* InOwnerRef, bool bInSaveMesh);
	TArray<int32> GetElementsByCategory(const FString& InCategory, const FString& ModelGuid);
	void UnloadFragment(const FString& ModelGuid);

	/**
	 * Update tile streaming based on camera parameters
	 * Called by FragmentsComponent to update visible tiles
	 */
	void UpdateTileStreaming(const FVector& CameraLocation, const FRotator& CameraRotation,
	                         float FOV, float AspectRatio, float ViewportHeight);

	FORCEINLINE const TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels() const
	{
		return FragmentModels;
	}
	// Non-const version for async loader to add models
	FORCEINLINE TMap<FString, class UFragmentModelWrapper*>& GetFragmentModels_Mutable()
	{
		return FragmentModels;
	}

	// Get a specific fragment model by GUID
	FORCEINLINE class UFragmentModelWrapper* GetFragmentModel(const FString& ModelGuid) const
	{
		if (const auto* Found = FragmentModels.Find(ModelGuid))
		{
			return *Found;
		}
		return nullptr;
	}

	// Get owner actor reference
	FORCEINLINE AActor* GetOwnerRef() const
	{
		return OwnerRef;
	}

	// Spawn a single fragment actor with its geometry (public for TileManager access)
	// @param bOutWasInstanced Optional output - set to true if fragment was handled via GPU instancing (no actor created)
	// @param RemainingBudgetMs Optional budget - if provided and exceeded, spawning stops early. Pass nullptr for unlimited.
	// @param OutSamplesProcessed Optional output - number of samples actually processed (for partial spawn tracking)
	AFragment* SpawnSingleFragment(const FFragmentItem& Item, AActor* ParentActor, const Meshes* MeshesRef, bool bSaveMeshes, bool* bOutWasInstanced = nullptr, float* RemainingBudgetMs = nullptr, int32* OutSamplesProcessed = nullptr);

	// ==========================================
	// EAGER GEOMETRY EXTRACTION (Public for AsyncLoader access)
	// ==========================================

	/**
	 * Pre-extract all geometry data from FlatBuffers at load time.
	 * Populates FFragmentSample::ExtractedGeometry for all samples in the hierarchy.
	 * This eliminates FlatBuffer dependencies during the spawn phase.
	 *
	 * @param RootItem The root fragment item to process (recursively processes children)
	 * @param MeshesRef The FlatBuffers meshes reference
	 */
	void PreExtractAllGeometry(FFragmentItem& RootItem, const Meshes* MeshesRef);

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

	/** Add material to mesh from raw color data (used by pre-extracted geometry path) */
	FName AddMaterialToMeshFromRawData(UStaticMesh*& CreatedMesh, uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass);

	bool TriangulatePolygonWithHoles(
		const TArray<FVector>& Points,
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

	// Extract geometry from shell representation
	void ExtractShellGeometry(
		const Shell* ShellRef,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs
	);

	// Extract geometry from circle extrusion representation

	void ExtractCircleExtrusionGeometry(
		const CircleExtrusion* ExtrusionRef,
		TArray<FVector>& OutVertices,
		TArray<int32>& OutTriangles,
		TArray<FVector>& OutNormals,
		TArray<FVector2D>& OutUVs
	);

	/**
	 * Extract geometry data for a single sample from FlatBuffers.
	 * Populates the ExtractedGeometry field with validated, copied data.
	 *
	 * @param Sample The sample to extract geometry for (modified in place)
	 * @param MeshesRef The FlatBuffers meshes reference
	 * @param ItemLocalId The local ID of the containing fragment (for logging)
	 * @return true if geometry was extracted successfully, false otherwise
	 */
	bool ExtractSampleGeometry(FFragmentSample& Sample, const Meshes* MeshesRef, int32 ItemLocalId);

	/**
	 * Create a static mesh from pre-extracted shell geometry.
	 * This uses data from FPreExtractedGeometry and never accesses FlatBuffers.
	 *
	 * @param Geometry The pre-extracted geometry data
	 * @param AssetName Name for the created mesh asset
	 * @param OuterRef Package/outer for the mesh
	 * @return Created UStaticMesh or nullptr on failure
	 */
	UStaticMesh* CreateStaticMeshFromPreExtractedShell(
		const FPreExtractedGeometry& Geometry,
		const FString& AssetName,
		UObject* OuterRef);

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

	// Representation-based mesh cache (Key = RepresentationId)
	// More reliable than geometry hashing since all instances of the same
	// RepresentationId share identical geometry in FlatBuffers format
	UPROPERTY()
	TMap<int32, UStaticMesh*> RepresentationMeshCache;

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

	// ==========================================
	// FRAME BUDGET COORDINATION
	// ==========================================

	/** Frame budget coordinator for unified time budgeting across geometry and spawning */
	FFrameBudgetCoordinator FrameBudgetCoordinator;

	/** Total frame budget in milliseconds across all processing (geometry + spawning) */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "1.0", ClampMax = "16.0"))
	float TotalFrameBudgetMs = 4.0f;

	/** Ratio of total budget allocated to geometry processing (0.0 to 1.0) */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GeometryBudgetRatio = 0.5f;

	/** Minimum budget threshold - skip phase if allocation would be below this (ms) */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float MinimumBudgetThresholdMs = 0.5f;

	/** Enable adaptive budget adjustment based on actual frame times */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance")
	bool bEnableAdaptiveBudget = true;

	/** Maximum number of NEW mesh creations per frame (cache hits are unlimited).
	 *  Lower values = smoother frame rate during initial load, but slower overall.
	 *  Set to 0 for unlimited. */
	UPROPERTY(EditAnywhere, Category = "Fragments|Performance", meta = (ClampMin = "0", ClampMax = "100"))
	int32 MaxNewMeshCreationsPerFrame = 1;

	/** Counter for new mesh creations this frame (reset in ProcessAllTileManagerChunks) */
	int32 NewMeshCreationsThisFrame = 0;

	/** Check if we can create another new mesh this frame */
	FORCEINLINE bool CanCreateNewMesh() const
	{
		return (MaxNewMeshCreationsPerFrame == 0) || (NewMeshCreationsThisFrame < MaxNewMeshCreationsPerFrame);
	}

	/** Increment the new mesh creation counter */
	FORCEINLINE void OnNewMeshCreated()
	{
		NewMeshCreationsThisFrame++;
	}

	/** Material pool for CRC-based deduplication */
	UPROPERTY()
	TMap<uint32, UMaterialInstanceDynamic*> MaterialPool;

	/** Hash material properties for pooling */
	uint32 HashMaterialProperties(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass) const;

	/** Get or create pooled material instance */
	UMaterialInstanceDynamic* GetPooledMaterial(uint8 R, uint8 G, uint8 B, uint8 A, bool bIsGlass);

	// ==========================================
	// GPU INSTANCING MEMBERS (Phase 4)
	// ==========================================

	/** Instancing threshold - use ISMC if >= this many instances share the same representation+material */
	int32 InstancingThreshold = 10;

	/** Enable/disable GPU instancing (can be toggled for debugging) */
	bool bEnableGPUInstancing = true;

	/** Threshold for incremental ISMC finalization. When pending instances exceed this,
	 *  finalize the group immediately to prevent OOM. Set to 0 to disable incremental finalization. */
	int32 IncrementalFinalizationThreshold = 500;

	/** Maximum pending instances across all groups before forced finalization.
	 *  This is a memory safety limit to prevent OOM on large models. */
	int32 MaxPendingInstancesTotal = 50000;

	/** Current total pending instances across all groups (for memory tracking) */
	int32 TotalPendingInstances = 0;

	/** Count of instances per RepresentationId + MaterialHash combination.
	 *  Key = (int64)RepresentationId | ((int64)MaterialHash << 32)
	 *  Built during PreExtractAllGeometry, used during spawn to decide instancing. */
	TMap<int64, int32> RepresentationMaterialInstanceCount;

	/** ISMC groups keyed by RepresentationId + MaterialHash.
	 *  Each group contains one ISMC with all instances sharing that geometry+material. */
	UPROPERTY()
	TMap<int64, FInstancedMeshGroup> InstancedMeshGroups;

	/** Proxy map for instanced fragments (LocalId -> proxy data).
	 *  Used for lookups on fragments that don't have AFragment actors. */
	UPROPERTY()
	TMap<int32, FFragmentProxy> LocalIdToProxyMap;

	/** Host actor for ISMC components.
	 *  All ISMCs are attached to this single actor for organization. */
	UPROPERTY()
	AActor* ISMCHostActor = nullptr;

public:

	TArray<class AFragment*> FragmentActors;

	/** Show debug wireframe bounds for streaming tiles (green=loading, red=culled) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bShowDebugTileBounds = false;

};
