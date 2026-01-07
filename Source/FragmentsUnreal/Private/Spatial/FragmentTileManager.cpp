#include "Spatial/FragmentTileManager.h"
#include "Spatial/FragmentOctree.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Fragment/Fragment.h"
#include "HAL/PlatformTime.h"
#include "SceneView.h"

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

void UFragmentTileManager::Initialize(const FString& InModelGuid, UFragmentOctree* InOctree, UFragmentsImporter* InImporter)
{
	if (!InOctree || !InImporter)
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("Initialize: Invalid octree or importer"));
		return;
	}

	ModelGuid = InModelGuid;
	Octree = InOctree;
	Importer = InImporter;

	// Reset state
	VisibleTiles.Empty();
	LoadedTiles.Empty();
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

namespace
{
	/** Calculate hash of frustum state for change detection */
	uint32 CalculateFrustumHash(const FVector& Position, const FRotator& Rotation, float FOV, float AspectRatio)
	{
		// Simple hash: combine position, rotation, and FOV
		// Use 10cm precision for position, 1 degree for rotation
		uint32 Hash = 0;
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Position.X / 10.0f)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Position.Y / 10.0f)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Position.Z / 10.0f)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Rotation.Yaw)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Rotation.Pitch)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Rotation.Roll)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(FOV)));
		Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(AspectRatio * 100.0f)));
		return Hash;
	}
}

void UFragmentTileManager::UpdateVisibleTiles(const FVector& CameraLocation, const FRotator& CameraRotation,
                                                float FOV, float AspectRatio)
{
	if (!Octree || !Importer)
	{
		return;
	}

	const double CurrentTime = FPlatformTime::Seconds();
	const float TimeSinceUpdate = CurrentTime - LastUpdateTime;

	// Check if update needed (time threshold OR significant movement OR significant rotation)
	const bool bTimeThresholdMet = TimeSinceUpdate >= CameraUpdateInterval;

	const float DistanceMoved = FVector::Dist(LastCameraPosition, CameraLocation);
	const bool bMovedSignificantly = DistanceMoved >= MinCameraMovement;

	// Check rotation change
	const FRotator RotationDelta = CameraRotation - LastCameraRotation;
	const float RotationChange = FMath::Max3(
		FMath::Abs(RotationDelta.Pitch),
		FMath::Abs(RotationDelta.Yaw),
		FMath::Abs(RotationDelta.Roll)
	);
	const bool bRotatedSignificantly = RotationChange >= MinCameraRotation;

	if (!bTimeThresholdMet && !bMovedSignificantly && !bRotatedSignificantly)
	{
		// OPTIMIZATION: Only process unload timers if camera moved recently (prevents delayed pops)
		const double TimeSinceMovement = CurrentTime - LastCameraMovementTime;
		if (TimeSinceMovement < 5.0)
		{
			ProcessUnloadTimers(TimeSinceUpdate);
		}
		else
		{
			UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Deferring unload timers: camera stationary for %.1fs"), TimeSinceMovement);
		}
		return;
	}

	UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Update triggered: time=%d move=%d (%.0fcm) rotate=%d (%.1fdeg)"),
	       bTimeThresholdMet, bMovedSignificantly, DistanceMoved, bRotatedSignificantly, RotationChange);

	// Track camera movement for deferred eviction/unload
	if (bMovedSignificantly || bRotatedSignificantly)
	{
		LastCameraMovementTime = CurrentTime;
	}

	// OPTIMIZATION: Check if frustum has actually changed (skip expensive octree query if identical)
	const uint32 CurrentFrustumHash = CalculateFrustumHash(CameraLocation, CameraRotation, FOV, AspectRatio);
	if (CurrentFrustumHash == LastFrustumHash)
	{
		// Frustum identical to last frame - skip expensive query
		UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Frustum unchanged (hash=%u), skipping octree query"), CurrentFrustumHash);

		// Still need to update time tracking
		LastUpdateTime = CurrentTime;

		// Still process unload timers
		const double TimeSinceMovement = CurrentTime - LastCameraMovementTime;
		if (TimeSinceMovement < 5.0)
		{
			ProcessUnloadTimers(TimeSinceUpdate);
		}

		return;  // Early exit - frustum unchanged
	}

	// Build camera frustum
	FConvexVolume Frustum = BuildCameraFrustum(CameraLocation, CameraRotation, FOV, AspectRatio);

	// Query octree for tiles intersecting frustum
	TArray<UFragmentTile*> FrustumTiles;
	Octree->QueryVisibleTiles(Frustum, FrustumTiles);

	// Filter by screen-space size
	TArray<UFragmentTile*> NewVisibleTiles;
	for (UFragmentTile* Tile : FrustumTiles)
	{
		if (!Tile)
		{
			continue;
		}

		// Calculate screen-space coverage
		const float ScreenSize = CalculateTileScreenSize(Tile, CameraLocation, FOV);

		// Only load tiles with sufficient screen coverage
		if (ScreenSize >= MinScreenCoverage)
		{
			NewVisibleTiles.Add(Tile);
		}
		else
		{
			UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Tile %p culled: screen size %.4f < min %.4f"),
			       Tile, ScreenSize, MinScreenCoverage);
		}
	}

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Camera update: %d frustum tiles, %d after screen filter"),
	       FrustumTiles.Num(), NewVisibleTiles.Num());

	// Store camera state for priority sorting
	LastPriorityCameraLocation = CameraLocation;
	LastPriorityFOV = FOV;

	// Update tile states based on new visible set
	UpdateTileStates(NewVisibleTiles);

	// Update last update tracking
	LastCameraPosition = CameraLocation;
	LastCameraRotation = CameraRotation;
	LastUpdateTime = CurrentTime;

	// Update frustum hash (for next frame comparison)
	LastFrustumHash = CurrentFrustumHash;
	LastAspectRatio = AspectRatio;

	// OPTIMIZATION: Only process unload timers if camera moved recently
	const double TimeSinceMovement = CurrentTime - LastCameraMovementTime;
	if (TimeSinceMovement < 5.0)
	{
		ProcessUnloadTimers(TimeSinceUpdate);
	}
	else
	{
		UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Deferring unload timers: camera stationary for %.1fs"), TimeSinceMovement);
	}
}

void UFragmentTileManager::UpdateTileStates(const TArray<UFragmentTile*>& NewVisibleTiles)
{
	// Build sets for comparison
	TSet<UFragmentTile*> NewVisibleSet(NewVisibleTiles);
	TSet<UFragmentTile*> OldVisibleSet(VisibleTiles);

	// OPTIMIZATION: Early exit if sets are identical (common for stationary camera)
	if (NewVisibleSet.Num() == OldVisibleSet.Num())
	{
		bool bSetsIdentical = true;
		for (UFragmentTile* Tile : NewVisibleSet)
		{
			if (!OldVisibleSet.Contains(Tile))
			{
				bSetsIdentical = false;
				break;
			}
		}

		if (bSetsIdentical)
		{
			// No change - skip processing entirely
			UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Visible set unchanged, skipping state update"));

			// Still touch tiles for LRU (but don't change state)
			for (UFragmentTile* Tile : NewVisibleTiles)
			{
				TouchTile(Tile);
			}

			// Still check cache budget (tiles may have finished loading)
			if (bEnableTileCache)
			{
				EvictTilesToFitBudget();
			}

			UpdateSpawnProgress();
			return;  // Early exit - nothing changed!
		}
	}

	// Handle newly visible tiles
	for (UFragmentTile* Tile : NewVisibleTiles)
	{
		if (!Tile)
		{
			continue;
		}

		// Mark tile as recently used (for LRU tracking)
		TouchTile(Tile);

		// OPTIMIZATION: Skip if already in visible set AND already in Visible state
		if (OldVisibleSet.Contains(Tile) && Tile->State == ETileState::Visible)
		{
			continue;  // Already visible, no state change needed
		}

		if (Tile->State == ETileState::Unloaded)
		{
			// Start loading this tile
			StartLoadingTile(Tile);
			DrawDebugTileBounds(Tile, FColor::Yellow, 3.0f);  // Yellow = starting to load
		}
		else if (Tile->State == ETileState::Loaded)
		{
			// Already loaded, just show it (cache hit!)
			ShowTile(Tile);
			DrawDebugTileBounds(Tile, FColor::Green, 2.0f);  // Green = cache hit, now visible
			UE_LOG(LogFragmentTileManager, Verbose, TEXT("Tile %p made visible from cache"), Tile);
		}
		else if (Tile->State == ETileState::Unloading)
		{
			// Was unloading, but now visible again - cancel unload
			ShowTile(Tile);
			DrawDebugTileBounds(Tile, FColor::Green, 2.0f);  // Green = re-shown from unloading
		}
		else if (Tile->State == ETileState::Loading)
		{
			// Actively loading (fragments being spawned)
			DrawDebugTileBounds(Tile, FColor::Orange, 2.0f);  // Orange = actively loading
		}
		else if (Tile->State == ETileState::Visible)
		{
			// Already visible (no state change)
			DrawDebugTileBounds(Tile, FColor::Cyan, 1.0f);  // Cyan = already visible (no change)
		}
	}

	// Handle tiles that are no longer visible
	TArray<UFragmentTile*> TilesToHide;
	for (UFragmentTile* Tile : VisibleTiles)
	{
		if (!NewVisibleSet.Contains(Tile))
		{
			TilesToHide.Add(Tile);
		}
	}

	for (UFragmentTile* Tile : TilesToHide)
	{
		// Hide tile but DON'T destroy actors (kept in cache)
		HideTile(Tile);
		DrawDebugTileBounds(Tile, FColor::Red, 2.0f);  // Red = being culled/hidden
		UE_LOG(LogFragmentTileManager, Verbose, TEXT("Tile %p hidden (kept in cache)"), Tile);
	}

	// Update visible tiles list
	VisibleTiles = NewVisibleTiles;

	// OPTIMIZATION: Defer eviction on stationary camera to prevent unexpected pops
	if (bEnableTileCache)
	{
		// Only evict if camera moved recently OR memory critically over budget
		const bool bCriticallyOverBudget = CurrentCacheBytes > (MaxCachedBytes * 1.2f);  // 20% over limit
		const double TimeSinceMovement = FPlatformTime::Seconds() - LastCameraMovementTime;
		const bool bCameraMovedRecently = TimeSinceMovement < 5.0;  // Moved in last 5 seconds

		if (bCriticallyOverBudget || bCameraMovedRecently)
		{
			EvictTilesToFitBudget();

			if (bCriticallyOverBudget && !bCameraMovedRecently)
			{
				UE_LOG(LogFragmentTileManager, Warning, TEXT("Cache critically over budget (%.0f%%), forcing eviction on stationary camera"),
				       GetCacheUsagePercent());
			}
		}
		else
		{
			UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Deferring eviction: camera stationary for %.1fs, cache at %.0f%%"),
			       TimeSinceMovement, GetCacheUsagePercent());
		}
	}

	UpdateSpawnProgress();
}

void UFragmentTileManager::StartLoadingTile(UFragmentTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	// Get model wrapper to access hierarchy
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("StartLoadingTile: No model wrapper"));
		return;
	}

	// OPTIMIZATION: Only expand hierarchy if not already cached
	if (!Tile->bHierarchyExpanded)
	{
		// Expand tile fragments to include entire subtrees (hierarchy preservation)
		// This ensures parent-child relationships are maintained
		// Use TArray to preserve parent-first ordering (important for spawning)
		TSet<int32> VisitedSet; // Avoid duplicates
		TArray<int32> OrderedFragments; // Parent-first order
		for (int32 LocalID : Tile->FragmentLocalIDs)
		{
			AddFragmentSubtreeOrdered(LocalID, Wrapper, VisitedSet, OrderedFragments);
		}

		// Update tile's fragment list with expanded subtrees
		const int32 OriginalCount = Tile->FragmentLocalIDs.Num();
		Tile->FragmentLocalIDs = OrderedFragments;

		// Mark as expanded (cache for future loads)
		Tile->bHierarchyExpanded = true;

		UE_LOG(LogFragmentTileManager, Verbose, TEXT("Expanded tile hierarchy: %d → %d fragments (with children)"),
		       OriginalCount, Tile->FragmentLocalIDs.Num());
	}
	else
	{
		UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Tile hierarchy already expanded, using cached (%d fragments)"),
		       Tile->FragmentLocalIDs.Num());
	}

	Tile->State = ETileState::Loading;
	Tile->CurrentSpawnIndex = 0;
	Tile->TimeLeftFrustum = 0.0f;

	// Add to loaded tiles tracking
	LoadedTiles.AddUnique(Tile);

	// Update total fragments to spawn
	TotalFragmentsToSpawn += Tile->FragmentLocalIDs.Num();

	UE_LOG(LogFragmentTileManager, Log, TEXT("Started loading tile: %d fragments"),
	       Tile->FragmentLocalIDs.Num());

	UpdateSpawnProgress();
}

void UFragmentTileManager::AddFragmentSubtree(int32 LocalID, UFragmentModelWrapper* Wrapper, TSet<int32>& OutSet)
{
	// Legacy method - kept for header compatibility
	// Use AddFragmentSubtreeOrdered instead
	TArray<int32> Dummy;
	AddFragmentSubtreeOrdered(LocalID, Wrapper, OutSet, Dummy);
}

void UFragmentTileManager::AddFragmentSubtreeOrdered(int32 LocalID, UFragmentModelWrapper* Wrapper,
                                                      TSet<int32>& VisitedSet, TArray<int32>& OutOrderedList)
{
	// Avoid duplicates
	if (VisitedSet.Contains(LocalID))
	{
		return;
	}

	// Mark as visited
	VisitedSet.Add(LocalID);

	// Find fragment item in model hierarchy
	FFragmentItem* Item = nullptr;
	if (!Wrapper->GetModelItemRef().FindFragmentByLocalId(LocalID, Item) || !Item)
	{
		return;
	}

	// Add this fragment FIRST (parent-first order)
	OutOrderedList.Add(LocalID);

	// Then recursively add all children
	for (FFragmentItem* Child : Item->FragmentChildren)
	{
		if (Child && Child->LocalId >= 0)
		{
			AddFragmentSubtreeOrdered(Child->LocalId, Wrapper, VisitedSet, OutOrderedList);
		}
	}
}

void UFragmentTileManager::ShowTile(UFragmentTile* Tile)
{
	if (!Tile)
	{
		return;
	}

	Tile->State = ETileState::Visible;
	Tile->TimeLeftFrustum = 0.0f;

	// Show all spawned actors
	for (AFragment* Actor : Tile->SpawnedActors)
	{
		if (Actor)
		{
			Actor->SetActorHiddenInGame(false);
		}
	}

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Showing tile with %d actors"), Tile->SpawnedActors.Num());
}

void UFragmentTileManager::HideTile(UFragmentTile* Tile)
{
	if (!Tile)
	{
		return;
	}

	// Only hide if fully loaded (not still loading)
	if (Tile->State == ETileState::Visible || Tile->State == ETileState::Loaded)
	{
		Tile->State = ETileState::Loaded;

		// Hide all spawned actors
		for (AFragment* Actor : Tile->SpawnedActors)
		{
			if (Actor)
			{
				Actor->SetActorHiddenInGame(true);
			}
		}

		// Reset unload timer
		Tile->TimeLeftFrustum = 0.0f;

		UE_LOG(LogFragmentTileManager, Verbose, TEXT("Hid tile with %d actors"), Tile->SpawnedActors.Num());
	}
}

void UFragmentTileManager::UnloadTile(UFragmentTile* Tile)
{
	if (!Tile)
	{
		return;
	}

	// Calculate memory before destroying actors
	int64 TileMemory = CalculateTileMemoryUsage(Tile);

	UE_LOG(LogFragmentTileManager, Log, TEXT("Unloading tile with %d actors (%lld KB)"),
	       Tile->SpawnedActors.Num(), TileMemory / 1024);

	// Destroy all actors
	for (AFragment* Actor : Tile->SpawnedActors)
	{
		if (Actor)
		{
			Actor->Destroy();
		}
	}

	Tile->SpawnedActors.Empty();
	Tile->LocalIdToActor.Empty();
	Tile->State = ETileState::Unloaded;
	Tile->CurrentSpawnIndex = 0;
	Tile->TimeLeftFrustum = 0.0f;
	Tile->bHierarchyExpanded = false;  // Reset hierarchy cache

	// Remove from loaded tiles
	LoadedTiles.Remove(Tile);

	// Update cache tracking
	CurrentCacheBytes = FMath::Max((int64)0, CurrentCacheBytes - TileMemory);

	// Update spawn tracking
	TotalFragmentsToSpawn = FMath::Max(0, TotalFragmentsToSpawn - Tile->FragmentLocalIDs.Num());
	FragmentsSpawned = FMath::Max(0, FragmentsSpawned - Tile->FragmentLocalIDs.Num());

	UpdateSpawnProgress();
}

void UFragmentTileManager::ProcessSpawnChunk()
{
	if (!Importer || !Octree)
	{
		return;
	}

	// Find all tiles in Loading state
	TArray<UFragmentTile*> LoadingTiles;
	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (Tile && Tile->State == ETileState::Loading)
		{
			LoadingTiles.Add(Tile);
		}
	}

	if (LoadingTiles.Num() == 0)
	{
		// No tiles currently loading
		if (TotalFragmentsToSpawn > 0 && FragmentsSpawned >= TotalFragmentsToSpawn)
		{
			LoadingStage = TEXT("Complete");
			SpawnProgress = 1.0f;
		}
		else
		{
			LoadingStage = TEXT("Idle");
		}
		return;
	}

	// Sort loading tiles by priority (highest first)
	LoadingTiles.Sort([this](const UFragmentTile& A, const UFragmentTile& B)
	{
		const float PriorityA = CalculateTilePriority(&A, LastPriorityCameraLocation, LastPriorityFOV);
		const float PriorityB = CalculateTilePriority(&B, LastPriorityCameraLocation, LastPriorityFOV);
		return PriorityB < PriorityA; // Descending order (highest priority first)
	});

	// Time-based spawning - spawn from multiple tiles within budget
	const double StartTime = FPlatformTime::Seconds();
	const double MaxSpawnTimeSec = MaxSpawnTimeMs / 1000.0;
	int32 TilesProcessedThisFrame = 0;

	// Spawn from multiple tiles in parallel within time budget
	for (UFragmentTile* Tile : LoadingTiles)
	{
		// Check time budget
		const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
		if (ElapsedTime >= MaxSpawnTimeSec && TilesProcessedThisFrame > 0)
		{
			// Budget exhausted, stop for this frame
			UE_LOG(LogFragmentTileManager, VeryVerbose, TEXT("Frame budget exhausted: %.2fms, %d tiles processed"),
			       ElapsedTime * 1000.0, TilesProcessedThisFrame);
			break;
		}

		// Spawn one fragment from this tile
		if (Tile->CurrentSpawnIndex < Tile->FragmentLocalIDs.Num())
		{
			if (SpawnFragmentFromTile(Tile))
			{
				FragmentsSpawned++;
				TilesProcessedThisFrame++;
			}
			Tile->CurrentSpawnIndex++;
		}

		// Check if tile finished loading
		if (Tile->CurrentSpawnIndex >= Tile->FragmentLocalIDs.Num())
		{
			Tile->State = ETileState::Loaded;

			// Add to cache tracking
			int64 TileMemory = CalculateTileMemoryUsage(Tile);
			CurrentCacheBytes += TileMemory;

			UE_LOG(LogFragmentTileManager, Log, TEXT("Tile finished loading: %d fragments, %lld KB - Cache: %lld MB / %lld MB"),
			       Tile->FragmentLocalIDs.Num(),
			       TileMemory / 1024,
			       CurrentCacheBytes / (1024 * 1024),
			       MaxCachedBytes / (1024 * 1024));

			// If this tile is in the visible set, show it
			if (VisibleTiles.Contains(Tile))
			{
				ShowTile(Tile);
			}
		}
	}

	UpdateSpawnProgress();
}

bool UFragmentTileManager::SpawnFragmentFromTile(UFragmentTile* Tile)
{
	if (!Tile || !Importer)
	{
		return false;
	}

	// Get LocalId to spawn
	const int32 LocalId = Tile->FragmentLocalIDs[Tile->CurrentSpawnIndex];

	// Get model wrapper
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentFromTile: No model wrapper for %s"), *ModelGuid);
		return false;
	}

	// Get FFragmentItem from model hierarchy
	FFragmentItem* FragmentItem = nullptr;
	if (!Wrapper->GetModelItemRef().FindFragmentByLocalId(LocalId, FragmentItem))
	{
		UE_LOG(LogFragmentTileManager, Warning, TEXT("SpawnFragmentFromTile: Could not find fragment LocalId %d"), LocalId);
		return false;
	}

	// Get Meshes* reference from parsed model
	const Model* ParsedModel = Wrapper->GetParsedModel();
	if (!ParsedModel || !ParsedModel->meshes())
	{
		UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentFromTile: No meshes in model"));
		return false;
	}

	const Meshes* MeshesRef = ParsedModel->meshes();

	// Find the correct parent actor for this fragment (hierarchy preservation)
	AActor* ParentActor = nullptr;

	// First, find the parent FFragmentItem by traversing hierarchy
	FFragmentItem* ParentItem = FindParentFragmentItem(&Wrapper->GetModelItemRef(), FragmentItem);

	if (ParentItem && ParentItem->LocalId >= 0)
	{
		// Parent exists - check if it's already spawned in this tile
		AFragment** FoundInTile = Tile->LocalIdToActor.Find(ParentItem->LocalId);
		if (FoundInTile && *FoundInTile)
		{
			ParentActor = *FoundInTile;
			UE_LOG(LogFragmentTileManager, Verbose, TEXT("Found parent LocalId %d for child %d"),
			       ParentItem->LocalId, LocalId);
		}
		else
		{
			// Parent not in tile - try global lookup
			ParentActor = Importer->GetItemByLocalId(ParentItem->LocalId, ModelGuid);
			if (ParentActor)
			{
				UE_LOG(LogFragmentTileManager, Verbose, TEXT("Found parent globally: LocalId %d"),
				       ParentItem->LocalId);
			}
		}
	}

	// Fall back to owner actor if no parent found
	if (!ParentActor)
	{
		ParentActor = Importer->GetOwnerRef();
		if (!ParentActor)
		{
			UE_LOG(LogFragmentTileManager, Error, TEXT("SpawnFragmentFromTile: No parent or owner actor"));
			return false;
		}
	}

	// Spawn using existing FragmentsImporter method
	// CRITICAL: MeshesRef contains Material* pointers - these will be passed to mesh creation
	// This avoids the nullptr material bug from previous implementation!
	AFragment* SpawnedActor = Importer->SpawnSingleFragment(*FragmentItem, ParentActor, MeshesRef, false);

	if (SpawnedActor)
	{
		Tile->SpawnedActors.Add(SpawnedActor);
		Tile->LocalIdToActor.Add(LocalId, SpawnedActor);
		UE_LOG(LogFragmentTileManager, Verbose, TEXT("Spawned fragment LocalId %d with parent %s"),
		       LocalId, *ParentActor->GetName());
		return true;
	}

	return false;
}

FFragmentItem* UFragmentTileManager::FindParentFragmentItem(const FFragmentItem* Root, const FFragmentItem* Target)
{
	if (!Root || !Target)
	{
		return nullptr;
	}

	// Check if any of this item's children is the target
	for (FFragmentItem* Child : Root->FragmentChildren)
	{
		if (Child == Target)
		{
			// Found it - Root is the parent
			return const_cast<FFragmentItem*>(Root);
		}
	}

	// Recursively search children
	for (FFragmentItem* Child : Root->FragmentChildren)
	{
		FFragmentItem* Found = FindParentFragmentItem(Child, Target);
		if (Found)
		{
			return Found;
		}
	}

	return nullptr;
}

FConvexVolume UFragmentTileManager::BuildCameraFrustum(const FVector& CameraLocation, const FRotator& CameraRotation,
                                                        float FOV, float AspectRatio) const
{
	// Build view matrix from camera transform
	const FMatrix ViewMatrix = FInverseRotationMatrix(CameraRotation) * FTranslationMatrix(-CameraLocation);

	// Build projection matrix
	const float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	const float NearPlane = 10.0f; // 10cm
	const float FarPlane = 10000000.0f; // 100km

	FMatrix ProjectionMatrix;
	ProjectionMatrix = FReversedZPerspectiveMatrix(HalfFOVRadians, AspectRatio, 1.0f, NearPlane);

	// Combine into view-projection matrix
	const FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;

	// Build frustum from view-projection matrix
	FConvexVolume Frustum;
	GetViewFrustumBounds(Frustum, ViewProjectionMatrix, false);

	return Frustum;
}

float UFragmentTileManager::CalculateTileScreenSize(const UFragmentTile* Tile, const FVector& CameraLocation, float FOV) const
{
	if (!Tile)
	{
		return 0.0f;
	}

	// Get tile dimensions and distance
	const FVector TileCenter = Tile->Bounds.GetCenter();
	const float Distance = FVector::Dist(CameraLocation, TileCenter);
	const float TileSize = Tile->Bounds.GetSize().GetMax();

	// Avoid division by zero for very close distances
	if (Distance < 1.0f)
	{
		return 1.0f; // Treat as full screen if inside tile
	}

	// Calculate view dimension at tile distance
	const float HalfFOVRadians = FMath::DegreesToRadians(FOV * 0.5f);
	const float ViewDimension = Distance * FMath::Tan(HalfFOVRadians);

	// Screen ratio: tile size / view size
	const float ScreenRatio = TileSize / ViewDimension;

	return ScreenRatio;
}

float UFragmentTileManager::CalculateTilePriority(const UFragmentTile* Tile, const FVector& CameraLocation, float FOV) const
{
	if (!Tile)
	{
		return 0.0f;
	}

	// Priority factors:
	// 1. Screen-space size (larger on screen = higher priority)
	// 2. Distance (closer = higher priority)
	// 3. Fragment count (more fragments = higher priority for visual completeness)

	const float ScreenSize = CalculateTileScreenSize(Tile, CameraLocation, FOV);
	const FVector TileCenter = Tile->Bounds.GetCenter();
	const float Distance = FVector::Dist(CameraLocation, TileCenter);
	const float FragmentCount = static_cast<float>(Tile->FragmentLocalIDs.Num());

	// Screen size weight: 100.0 (most important)
	// Distance weight: inverse, normalized to 0-10 range (10000cm = priority 1.0)
	// Fragment count weight: 0.01 (minor influence, breaks ties)

	const float DistancePriority = FMath::Clamp(10000.0f / FMath::Max(Distance, 100.0f), 0.0f, 10.0f);
	const float FragmentPriority = FragmentCount * 0.01f;

	const float Priority = (ScreenSize * 100.0f) + DistancePriority + FragmentPriority;

	return Priority;
}

void UFragmentTileManager::DrawDebugTileBounds(UFragmentTile* Tile, const FColor& Color, float Thickness)
{
	if (!Tile || !Importer)
	{
		return;
	}

	// Only draw if debug enabled
	if (!Importer->bShowDebugTileBounds)
	{
		return;
	}

	// Get world through owner actor (importer may be owned by component)
	AActor* Owner = Importer->GetOwnerRef();
	if (!Owner)
	{
		return;
	}

	UWorld* World = Owner->GetWorld();
	if (!World)
	{
		return;
	}

	// Draw wireframe box (2 second lifetime for smooth updates)
	DrawDebugBox(
		World,
		Tile->Bounds.GetCenter(),
		Tile->Bounds.GetExtent(),
		Color,
		false,  // Not persistent
		2.0f,   // 2 second lifetime (refreshed every update)
		0,      // Depth priority
		Thickness
	);

	// Draw fragment count label at center
	const FString Label = FString::Printf(TEXT("%d frags"), Tile->FragmentLocalIDs.Num());
	DrawDebugString(
		World,
		Tile->Bounds.GetCenter(),
		Label,
		nullptr,
		Color,
		2.0f,
		true  // Draw shadow
	);
}

void UFragmentTileManager::UpdateSpawnProgress()
{
	if (TotalFragmentsToSpawn > 0)
	{
		SpawnProgress = (float)FragmentsSpawned / (float)TotalFragmentsToSpawn;
		LoadingStage = FString::Printf(TEXT("Loading: %d/%d (%.1f%%)"),
		                               FragmentsSpawned, TotalFragmentsToSpawn, SpawnProgress * 100.0f);
	}
	else
	{
		SpawnProgress = 0.0f;
		LoadingStage = TEXT("Idle");
	}
}

void UFragmentTileManager::ProcessUnloadTimers(float DeltaTime)
{
	TArray<UFragmentTile*> TilesToUnload;

	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (!Tile)
		{
			continue;
		}

		// Only process tiles that are loaded but not visible
		if (Tile->State == ETileState::Loaded && !VisibleTiles.Contains(Tile))
		{
			Tile->TimeLeftFrustum += DeltaTime;

			// Check if past hysteresis threshold
			if (Tile->TimeLeftFrustum >= UnloadHysteresis)
			{
				TilesToUnload.Add(Tile);
			}
		}
	}

	// Unload tiles that have been hidden long enough
	for (UFragmentTile* Tile : TilesToUnload)
	{
		UnloadTile(Tile);
	}
}

bool UFragmentTileManager::IsLoading() const
{
	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (Tile && Tile->State == ETileState::Loading)
		{
			return true;
		}
	}
	return false;
}

void UFragmentTileManager::TouchTile(UFragmentTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	UWorld* World = Importer->GetWorld();
	if (World)
	{
		TileLastUsedTime.Add(Tile, World->GetTimeSeconds());
	}
}

int64 UFragmentTileManager::CalculateTileMemoryUsage(UFragmentTile* Tile) const
{
	if (!Tile)
	{
		return 0;
	}

	int64 TotalBytes = 0;

	// Calculate memory from spawned actors
	for (AFragment* Actor : Tile->SpawnedActors)
	{
		if (!Actor)
		{
			continue;
		}

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
			// Note: This is approximate - includes vertex buffer, index buffer, etc.
			if (Mesh->GetRenderData())
			{
				for (const FStaticMeshLODResources& LOD : Mesh->GetRenderData()->LODResources)
				{
					// Vertex buffer
					TotalBytes += LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices() * sizeof(FVector);
					TotalBytes += LOD.VertexBuffers.StaticMeshVertexBuffer.GetResourceSize();
					TotalBytes += LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() * sizeof(FColor);

					// Index buffer (UE5 API)
					TotalBytes += LOD.IndexBuffer.GetAllocatedSize();
				}
			}

			// Material instances (rough estimate)
			TArray<UMaterialInterface*> Materials = MeshComp->GetMaterials();
			TotalBytes += Materials.Num() * 1024;  // ~1 KB per material instance
		}

		// Actor overhead (rough estimate)
		TotalBytes += 4096;  // ~4 KB per actor
	}

	return TotalBytes;
}

void UFragmentTileManager::EvictTilesToFitBudget()
{
	if (!Importer)
	{
		return;
	}

	UWorld* World = Importer->GetWorld();
	if (!World)
	{
		return;
	}

	double CurrentTime = World->GetTimeSeconds();

	// Recalculate current cache usage
	CurrentCacheBytes = 0;

	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (Tile && Tile->State == ETileState::Loaded)
		{
			CurrentCacheBytes += CalculateTileMemoryUsage(Tile);
		}
	}

	// Check if we're over budget
	if (CurrentCacheBytes <= MaxCachedBytes)
	{
		return;  // Within budget, no eviction needed
	}

	UE_LOG(LogFragmentTileManager, Warning, TEXT("Cache over budget: %lld MB / %lld MB - evicting tiles"),
	       CurrentCacheBytes / (1024 * 1024),
	       MaxCachedBytes / (1024 * 1024));

	// Build list of eviction candidates (Loaded tiles only, not Visible)
	TArray<UFragmentTile*> EvictionCandidates;

	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (!Tile || Tile->State != ETileState::Loaded)
		{
			continue;  // Skip unloaded or currently visible tiles
		}

		// Must have been out of frustum for minimum time
		double TimeSinceUsed = CurrentTime - Tile->TimeLeftFrustum;
		if (TimeSinceUsed < MinTimeBeforeUnload)
		{
			continue;  // Too recent, keep in cache
		}

		EvictionCandidates.Add(Tile);
	}

	// Sort by last used time (LRU first)
	EvictionCandidates.Sort([this](const UFragmentTile& A, const UFragmentTile& B)
	{
		double TimeA = TileLastUsedTime.FindRef(&A);
		double TimeB = TileLastUsedTime.FindRef(&B);
		return TimeA < TimeB;  // Oldest first
	});

	// Evict tiles until we're under budget
	for (UFragmentTile* Tile : EvictionCandidates)
	{
		if (CurrentCacheBytes <= MaxCachedBytes)
		{
			break;  // Under budget now
		}

		int64 TileMemory = CalculateTileMemoryUsage(Tile);

		// Unload this tile
		UnloadTile(Tile);

		CurrentCacheBytes -= TileMemory;

		UE_LOG(LogFragmentTileManager, Log, TEXT("Evicted tile %p (%lld KB) - Cache now: %lld MB"),
		       Tile,
		       TileMemory / 1024,
		       CurrentCacheBytes / (1024 * 1024));
	}
}
