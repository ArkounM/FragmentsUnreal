#include "Spatial/FragmentTileManager.h"
#include "Spatial/FragmentRegistry.h"
#include "Spatial/PerSampleVisibilityController.h"
#include "Spatial/DynamicTileGenerator.h"
#include "Spatial/OcclusionSpawnController.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Fragment/Fragment.h"
#include "HAL/PlatformTime.h"
#include "Components/StaticMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentTileManager, Log, All);

namespace
{
	/** Calculate memory budget based on device RAM (100 MB per GB like engine_fragment) */
	int64 CalculateDeviceMemoryBudget()
	{
		const int32 PhysicalGB = FPlatformMemory::GetPhysicalGBRam();

		// Clamp to reasonable range
		const int32 ClampedGB = FMath::Clamp(PhysicalGB, 2, 64);

		// 100 MB per GB of system RAM
		const int64 Budget = static_cast<int64>(ClampedGB) * 100 * 1024 * 1024;

		UE_LOG(LogFragmentTileManager, Log, TEXT("Device RAM: %d GB, Cache budget: %lld MB"),
		       PhysicalGB, Budget / (1024 * 1024));

		return Budget;
	}
}

UFragmentTileManager::UFragmentTileManager()
{
}

void UFragmentTileManager::Initialize(const FString& InModelGuid, UFragmentsImporter* InImporter)
{
	if (!InImporter)
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("Initialize: Invalid importer"));
		return;
	}

	ModelGuid = InModelGuid;
	Importer = InImporter;

	// Reset state
	TotalFragmentsToSpawn = 0;
	FragmentsSpawned = 0;
	SpawnProgress = 0.0f;
	LoadingStage = TEXT("Idle");
	LastCameraPosition = FVector::ZeroVector;
	LastCameraRotation = FRotator::ZeroRotator;
	LastUpdateTime = 0.0;
	LastCameraMovementTime = 0.0;

	// Set device-aware memory budget (if auto-detect enabled)
	if (bAutoDetectCacheBudget)
	{
		MaxCachedBytes = CalculateDeviceMemoryBudget();
	}

	UE_LOG(LogFragmentTileManager, Log, TEXT("TileManager initialized for model: %s, Cache budget: %lld MB"),
	       *ModelGuid, MaxCachedBytes / (1024 * 1024));
}

void UFragmentTileManager::UpdateVisibleTiles(const FVector& CameraLocation, const FRotator& CameraRotation,
                                                float FOV, float AspectRatio, float ViewportHeight)
{
	if (!SampleVisibility || !TileGenerator || !FragmentRegistry)
	{
		UE_LOG(LogFragmentTileManager, Warning, TEXT("UpdateVisibleTiles: Per-sample system not initialized"));
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();
	const float TimeSinceUpdate = CurrentTime - LastUpdateTime;

	// Check if update needed
	const bool bTimeThresholdMet = TimeSinceUpdate >= CameraUpdateInterval;
	const float DistanceMoved = FVector::Dist(LastCameraPosition, CameraLocation);
	const bool bMovedSignificantly = DistanceMoved >= MinCameraMovement;

	const FRotator RotationDelta = CameraRotation - LastCameraRotation;
	const float RotationChange = FMath::Max3(
		FMath::Abs(RotationDelta.Pitch),
		FMath::Abs(RotationDelta.Yaw),
		FMath::Abs(RotationDelta.Roll)
	);
	const bool bRotatedSignificantly = RotationChange >= MinCameraRotation;

	UE_LOG(LogFragmentTileManager, VeryVerbose,
	       TEXT("Visibility update: time=%d move=%d (%.0fcm) rotate=%d (%.1fdeg)"),
	       bTimeThresholdMet, bMovedSignificantly, DistanceMoved, bRotatedSignificantly, RotationChange);

	// Track camera movement
	if (bMovedSignificantly || bRotatedSignificantly)
	{
		LastCameraMovementTime = CurrentTime;
	}

	// === STEP 1: Per-sample visibility evaluation ===
	SampleVisibility->bShowAllVisible = bShowAllVisible;
	SampleVisibility->GraphicsQuality = GraphicsQuality;
	SampleVisibility->UpdateVisibility(CameraLocation, CameraRotation, FOV, AspectRatio, ViewportHeight);

	// === STEP 2: Generate dynamic tiles from visible samples ===
	const TArray<FFragmentVisibilityResult>& VisibleSamples = SampleVisibility->GetVisibleSamples();
	TileGenerator->GenerateTiles(VisibleSamples, FragmentRegistry);

	// === STEP 3: Determine fragments to spawn/show/hide ===
	TArray<int32> ToSpawn = TileGenerator->GetFragmentsToSpawn(SpawnedFragments);
	TArray<int32> ToHide = TileGenerator->GetFragmentsToUnload(SpawnedFragments);

	// Check how many can be shown from cache vs need actual spawning
	int32 CacheHits = 0;
	TArray<int32> ActuallyNeedSpawn;
	for (int32 LocalId : ToSpawn)
	{
		if (HiddenFragments.Contains(LocalId))
		{
			CacheHits++;
		}
		else
		{
			ActuallyNeedSpawn.Add(LocalId);
		}
	}

	// Update spawn tracking (only count actual spawns, not cache hits)
	TotalFragmentsToSpawn = ActuallyNeedSpawn.Num();
	FragmentsSpawned = 0;

	UE_LOG(LogFragmentTileManager, Verbose,
	       TEXT("Visibility: %d visible, %d tiles, %d to show (%d cache hits), %d to hide"),
	       VisibleSamples.Num(), TileGenerator->GetTileCount(), ToSpawn.Num(), CacheHits, ToHide.Num());

	// === STEP 4: Show cached fragments immediately (cache hits) ===
	for (int32 LocalId : ToSpawn)
	{
		if (HiddenFragments.Contains(LocalId))
		{
			ShowFragmentById(LocalId);
		}
	}

	// === STEP 5: Hide fragments that left frustum (don't destroy - keep in cache) ===
	for (int32 LocalId : ToHide)
	{
		HideFragmentById(LocalId);
	}

	// === STEP 6: Evict hidden fragments if memory over budget ===
	EvictFragmentsToFitBudget();

	// Update last camera state
	LastCameraPosition = CameraLocation;
	LastCameraRotation = CameraRotation;
	LastUpdateTime = CurrentTime;
	LastPriorityCameraLocation = CameraLocation;
	LastPriorityFOV = FOV;

	UpdateSpawnProgress();
}

// =============================================================================
// PER-SAMPLE VISIBILITY METHODS
// =============================================================================

void UFragmentTileManager::ProcessSpawnChunk()
{
	if (!TileGenerator || !Importer)
	{
		return;
	}

	// Get fragments to spawn - filter out those already spawned or in hidden cache
	TArray<int32> ToSpawn = TileGenerator->GetFragmentsToSpawn(SpawnedFragments);

	// Filter out fragments that are in hidden cache (already handled)
	TArray<int32> ActuallyNeedSpawn;
	for (int32 LocalId : ToSpawn)
	{
		if (!HiddenFragments.Contains(LocalId))
		{
			ActuallyNeedSpawn.Add(LocalId);
		}
	}

	if (ActuallyNeedSpawn.Num() == 0)
	{
		if (TotalFragmentsToSpawn > 0 && FragmentsSpawned >= TotalFragmentsToSpawn)
		{
			LoadingStage = TEXT("Complete");
			SpawnProgress = 1.0f;
		}
		else if (TotalFragmentsToSpawn == 0)
		{
			LoadingStage = TEXT("Idle");
		}
		return;
	}

	// Sort by priority: non-deferred first, then by distance (closest first)
	ActuallyNeedSpawn.Sort([this](const int32& A, const int32& B)
	{
		const FFragmentVisibilityData* DataA = FragmentRegistry ? FragmentRegistry->FindFragment(A) : nullptr;
		const FFragmentVisibilityData* DataB = FragmentRegistry ? FragmentRegistry->FindFragment(B) : nullptr;

		if (!DataA || !DataB) return false;

		// Calculate base distances
		const float DistA = FVector::DistSquared(DataA->WorldBounds.GetCenter(), LastPriorityCameraLocation);
		const float DistB = FVector::DistSquared(DataB->WorldBounds.GetCenter(), LastPriorityCameraLocation);

		// Apply occlusion deferral priority adjustment
		float PriorityA = DistA;
		float PriorityB = DistB;

		if (OcclusionController && bEnableOcclusionDeferral)
		{
			PriorityA = OcclusionController->GetSpawnPriority(A, DistA);
			PriorityB = OcclusionController->GetSpawnPriority(B, DistB);
		}

		return PriorityA < PriorityB;
	});

	// Time-based spawning within frame budget
	const double StartTime = FPlatformTime::Seconds();
	const double MaxSpawnTimeSec = MaxSpawnTimeMs / 1000.0;
	int32 SpawnedThisFrame = 0;

	for (int32 LocalId : ActuallyNeedSpawn)
	{
		// Check time budget
		const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
		if (ElapsedTime >= MaxSpawnTimeSec && SpawnedThisFrame > 0)
		{
			UE_LOG(LogFragmentTileManager, VeryVerbose,
			       TEXT("Spawn budget exhausted: %.2fms, %d spawned"),
			       ElapsedTime * 1000.0, SpawnedThisFrame);
			break;
		}

		if (SpawnFragmentById(LocalId))
		{
			SpawnedThisFrame++;
			FragmentsSpawned++;
		}
	}

	// Update occlusion tracking based on render results
	UpdateOcclusionTracking();

	// Update loading stage
	if (FragmentsSpawned < TotalFragmentsToSpawn)
	{
		LoadingStage = FString::Printf(TEXT("Spawning %d/%d"), FragmentsSpawned, TotalFragmentsToSpawn);
	}
	else if (TotalFragmentsToSpawn > 0)
	{
		LoadingStage = TEXT("Complete");
	}

	UpdateSpawnProgress();
}

void UFragmentTileManager::InitializePerSampleVisibility(UFragmentRegistry* InRegistry)
{
	if (!InRegistry || !InRegistry->IsBuilt())
	{
		UE_LOG(LogFragmentTileManager, Warning, TEXT("InitializePerSampleVisibility: Invalid registry"));
		return;
	}

	FragmentRegistry = InRegistry;

	// Create per-sample visibility controller
	SampleVisibility = NewObject<UPerSampleVisibilityController>(this);
	SampleVisibility->Initialize(FragmentRegistry);
	SampleVisibility->bShowAllVisible = bShowAllVisible;
	SampleVisibility->GraphicsQuality = GraphicsQuality;
	SampleVisibility->MinCameraMovement = MinCameraMovement;
	SampleVisibility->MinCameraRotation = MinCameraRotation;

	// Create dynamic tile generator
	TileGenerator = NewObject<UDynamicTileGenerator>(this);

	// Create occlusion spawn controller for deferred spawning
	OcclusionController = NewObject<UOcclusionSpawnController>(this);
	OcclusionController->Initialize(FragmentRegistry);
	OcclusionController->bEnableOcclusionDeferral = bEnableOcclusionDeferral;

	// Clear per-sample state
	SpawnedFragments.Empty();
	HiddenFragments.Empty();
	SpawnedFragmentActors.Empty();
	FragmentLastUsedTime.Empty();
	PerSampleCacheBytes = 0;

	UE_LOG(LogFragmentTileManager, Log, TEXT("Per-sample visibility initialized: %d fragments in registry, Cache budget: %lld MB, OcclusionDeferral: %s"),
	       FragmentRegistry->GetFragmentCount(), MaxCachedBytes / (1024 * 1024),
	       bEnableOcclusionDeferral ? TEXT("Enabled") : TEXT("Disabled"));
}

bool UFragmentTileManager::SpawnFragmentById(int32 LocalId)
{
	// Skip if already spawned (visible)
	if (SpawnedFragments.Contains(LocalId))
	{
		return false;
	}

	// Check if in hidden cache - show it instead of spawning
	if (HiddenFragments.Contains(LocalId))
	{
		return ShowFragmentById(LocalId);
	}

	if (!Importer)
	{
		return false;
	}

	// Get model wrapper
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentById: No model wrapper for %s"), *ModelGuid);
		return false;
	}

	// Get FFragmentItem from model hierarchy
	FFragmentItem* FragmentItem = nullptr;
	if (!Wrapper->GetModelItemRef().FindFragmentByLocalId(LocalId, FragmentItem))
	{
		UE_LOG(LogFragmentTileManager, Warning, TEXT("SpawnFragmentById: Could not find fragment LocalId %d"), LocalId);
		return false;
	}

	// Get Meshes reference from parsed model
	const Model* ParsedModel = Wrapper->GetParsedModel();
	if (!ParsedModel || !ParsedModel->meshes())
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentById: No meshes in model"));
		return false;
	}

	const Meshes* MeshesRef = ParsedModel->meshes();

	// Find parent actor
	AActor* ParentActor = nullptr;

	// Check if parent fragment is already spawned
	FFragmentItem* ParentItem = FindParentFragmentItem(&Wrapper->GetModelItemRef(), FragmentItem);
	if (ParentItem && ParentItem->LocalId >= 0)
	{
		AFragment** FoundParent = SpawnedFragmentActors.Find(ParentItem->LocalId);
		if (FoundParent && *FoundParent)
		{
			ParentActor = *FoundParent;
		}
		else
		{
			// Try global lookup
			ParentActor = Importer->GetItemByLocalId(ParentItem->LocalId, ModelGuid);
		}
	}

	// Fall back to owner actor
	if (!ParentActor)
	{
		ParentActor = Importer->GetOwnerRef();
		if (!ParentActor)
		{
			UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentById: No parent or owner actor"));
			return false;
		}
	}

	// Spawn fragment
	AFragment* SpawnedActor = Importer->SpawnSingleFragment(*FragmentItem, ParentActor, MeshesRef, false);

	if (SpawnedActor)
	{
		SpawnedFragments.Add(LocalId);
		SpawnedFragmentActors.Add(LocalId, SpawnedActor);

		// Track memory usage
		int64 FragmentMemory = CalculateFragmentMemoryUsage(SpawnedActor);
		PerSampleCacheBytes += FragmentMemory;

		// Update LRU tracking
		TouchFragment(LocalId);

		UE_LOG(LogFragmentTileManager, Verbose, TEXT("Spawned fragment LocalId %d (%lld KB)"),
		       LocalId, FragmentMemory / 1024);
		return true;
	}

	return false;
}

void UFragmentTileManager::HideFragmentById(int32 LocalId)
{
	AFragment** ActorPtr = SpawnedFragmentActors.Find(LocalId);
	if (!ActorPtr || !*ActorPtr)
	{
		return;
	}

	AFragment* Actor = *ActorPtr;

	// Just hide the actor, don't destroy (matches engine_fragment behavior)
	Actor->SetActorHiddenInGame(true);

	// Move from spawned to hidden set
	SpawnedFragments.Remove(LocalId);
	HiddenFragments.Add(LocalId);

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Hid fragment LocalId %d (cached)"), LocalId);
}

bool UFragmentTileManager::ShowFragmentById(int32 LocalId)
{
	// Check if fragment is in hidden cache
	if (!HiddenFragments.Contains(LocalId))
	{
		return false;
	}

	AFragment** ActorPtr = SpawnedFragmentActors.Find(LocalId);
	if (!ActorPtr || !*ActorPtr)
	{
		// Actor was destroyed, need to respawn
		HiddenFragments.Remove(LocalId);
		return false;
	}

	AFragment* Actor = *ActorPtr;
	Actor->SetActorHiddenInGame(false);

	// Move from hidden to spawned set
	HiddenFragments.Remove(LocalId);
	SpawnedFragments.Add(LocalId);

	// Update LRU tracking
	TouchFragment(LocalId);

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Showed fragment LocalId %d (cache hit)"), LocalId);
	return true;
}

void UFragmentTileManager::UnloadFragmentById(int32 LocalId)
{
	AFragment** ActorPtr = SpawnedFragmentActors.Find(LocalId);
	if (!ActorPtr || !*ActorPtr)
	{
		SpawnedFragments.Remove(LocalId);
		HiddenFragments.Remove(LocalId);
		SpawnedFragmentActors.Remove(LocalId);
		FragmentLastUsedTime.Remove(LocalId);
		return;
	}

	AFragment* Actor = *ActorPtr;

	// Calculate memory before destroying
	int64 FragmentMemory = CalculateFragmentMemoryUsage(Actor);

	// Destroy the actor
	Actor->Destroy();

	// Remove from all tracking
	SpawnedFragments.Remove(LocalId);
	HiddenFragments.Remove(LocalId);
	SpawnedFragmentActors.Remove(LocalId);
	FragmentLastUsedTime.Remove(LocalId);

	// Update cache memory tracking
	PerSampleCacheBytes = FMath::Max((int64)0, PerSampleCacheBytes - FragmentMemory);

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Unloaded fragment LocalId %d (%lld KB freed)"), LocalId, FragmentMemory / 1024);
}

int64 UFragmentTileManager::CalculateFragmentMemoryUsage(AFragment* Actor) const
{
	if (!Actor)
	{
		return 0;
	}

	int64 TotalBytes = 0;

	// Get all static mesh components
	TArray<UStaticMeshComponent*> MeshComponents;
	Actor->GetComponents<UStaticMeshComponent>(MeshComponents);

	for (UStaticMeshComponent* MeshComp : MeshComponents)
	{
		if (!MeshComp || !MeshComp->GetStaticMesh())
		{
			continue;
		}

		UStaticMesh* Mesh = MeshComp->GetStaticMesh();

		// Get mesh resource size
		if (Mesh->GetRenderData())
		{
			for (const FStaticMeshLODResources& LOD : Mesh->GetRenderData()->LODResources)
			{
				TotalBytes += LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices() * sizeof(FVector);
				TotalBytes += LOD.VertexBuffers.StaticMeshVertexBuffer.GetResourceSize();
				TotalBytes += LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() * sizeof(FColor);
				TotalBytes += LOD.IndexBuffer.GetAllocatedSize();
			}
		}

		// Material instances
		TArray<UMaterialInterface*> Materials = MeshComp->GetMaterials();
		TotalBytes += Materials.Num() * 1024;
	}

	// Actor overhead
	TotalBytes += 4096;

	return TotalBytes;
}

FFragmentItem* UFragmentTileManager::FindParentFragmentItem(const FFragmentItem* Root, const FFragmentItem* Target)
{
	if (!Root || !Target)
	{
		return nullptr;
	}

	// Check if any of Root's children is the target
	for (FFragmentItem* Child : Root->FragmentChildren)
	{
		if (Child == Target || Child->LocalId == Target->LocalId)
		{
			return const_cast<FFragmentItem*>(Root);
		}

		// Recursively search in child's subtree
		FFragmentItem* Found = FindParentFragmentItem(Child, Target);
		if (Found)
		{
			return Found;
		}
	}

	return nullptr;
}

void UFragmentTileManager::UpdateSpawnProgress()
{
	if (TotalFragmentsToSpawn > 0)
	{
		SpawnProgress = static_cast<float>(FragmentsSpawned) / static_cast<float>(TotalFragmentsToSpawn);
		SpawnProgress = FMath::Clamp(SpawnProgress, 0.0f, 1.0f);
	}
	else
	{
		SpawnProgress = 0.0f;
	}
}

bool UFragmentTileManager::IsLoading() const
{
	return TotalFragmentsToSpawn > 0 && FragmentsSpawned < TotalFragmentsToSpawn;
}

void UFragmentTileManager::TouchFragment(int32 LocalId)
{
	if (!Importer)
	{
		return;
	}

	UWorld* World = Importer->GetWorld();
	if (World)
	{
		FragmentLastUsedTime.Add(LocalId, World->GetTimeSeconds());
	}
}

bool UFragmentTileManager::IsPerSampleMemoryOverBudget() const
{
	return PerSampleCacheBytes > MaxCachedBytes;
}

void UFragmentTileManager::EvictFragmentsToFitBudget()
{
	if (!Importer)
	{
		return;
	}

	// Only evict if over budget
	if (!IsPerSampleMemoryOverBudget())
	{
		return;
	}

	UWorld* World = Importer->GetWorld();
	if (!World)
	{
		return;
	}

	const double CurrentTime = World->GetTimeSeconds();

	UE_LOG(LogFragmentTileManager, Warning, TEXT("Cache over budget: %lld MB / %lld MB - evicting hidden fragments"),
	       PerSampleCacheBytes / (1024 * 1024), MaxCachedBytes / (1024 * 1024));

	// Build list of eviction candidates from HIDDEN fragments only
	TArray<int32> EvictionCandidates;

	for (int32 LocalId : HiddenFragments)
	{
		double* LastUsedPtr = FragmentLastUsedTime.Find(LocalId);
		double TimeSinceUsed = CurrentTime - (LastUsedPtr ? *LastUsedPtr : 0.0);

		if (TimeSinceUsed >= MinTimeBeforeUnload)
		{
			EvictionCandidates.Add(LocalId);
		}
	}

	// Sort by last used time (LRU first)
	EvictionCandidates.Sort([this](const int32& A, const int32& B)
	{
		double TimeA = FragmentLastUsedTime.FindRef(A);
		double TimeB = FragmentLastUsedTime.FindRef(B);
		return TimeA < TimeB;
	});

	// Evict fragments until under budget
	int32 EvictedCount = 0;
	for (int32 LocalId : EvictionCandidates)
	{
		if (!IsPerSampleMemoryOverBudget())
		{
			break;
		}

		UnloadFragmentById(LocalId);
		EvictedCount++;
	}

	if (EvictedCount > 0)
	{
		UE_LOG(LogFragmentTileManager, Log, TEXT("Evicted %d hidden fragments - Cache now: %lld MB"),
		       EvictedCount, PerSampleCacheBytes / (1024 * 1024));
	}
}

TSet<int32> UFragmentTileManager::CollectRenderedFragments() const
{
	TSet<int32> RenderedFragments;

	if (!Importer)
	{
		return RenderedFragments;
	}

	UWorld* World = Importer->GetWorld();
	if (!World)
	{
		return RenderedFragments;
	}

	const float CurrentTime = World->GetTimeSeconds();
	const float RenderTimeThreshold = 0.033f;

	for (const auto& Pair : SpawnedFragmentActors)
	{
		AFragment* Actor = Pair.Value;
		if (!Actor || !SpawnedFragments.Contains(Pair.Key))
		{
			continue;
		}

		bool bWasRendered = false;
		TArray<UStaticMeshComponent*> MeshComponents;
		Actor->GetComponents<UStaticMeshComponent>(MeshComponents);

		for (UStaticMeshComponent* MeshComp : MeshComponents)
		{
			if (MeshComp)
			{
				const float LastRenderTime = MeshComp->GetLastRenderTimeOnScreen();
				if ((CurrentTime - LastRenderTime) < RenderTimeThreshold)
				{
					bWasRendered = true;
					break;
				}
			}
		}

		if (bWasRendered)
		{
			RenderedFragments.Add(Pair.Key);
		}
	}

	return RenderedFragments;
}

void UFragmentTileManager::UpdateOcclusionTracking()
{
	if (!OcclusionController || !bEnableOcclusionDeferral)
	{
		return;
	}

	TSet<int32> RenderedFragments = CollectRenderedFragments();
	OcclusionController->UpdateOcclusionTracking(RenderedFragments, SpawnedFragments);
}
