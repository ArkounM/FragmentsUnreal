#include "Spatial/FragmentTileManager.h"
#include "Spatial/FragmentOctree.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Fragment/Fragment.h"
#include "HAL/PlatformTime.h"
#include "SceneView.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentTileManager, Log, All);

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
	LastUpdateTime = 0.0;

	UE_LOG(LogFragmentTileManager, Log, TEXT("TileManager initialized for model: %s"), *ModelGuid);
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

	// Check if update needed (time threshold OR significant movement)
	const bool bTimeThresholdMet = TimeSinceUpdate >= CameraUpdateInterval;
	const float DistanceMoved = FVector::Dist(LastCameraPosition, CameraLocation);
	const bool bMovedSignificantly = DistanceMoved >= MinCameraMovement;

	if (!bTimeThresholdMet && !bMovedSignificantly)
	{
		// Process unload timers even if not updating visible set
		ProcessUnloadTimers(TimeSinceUpdate);
		return;
	}

	// Build camera frustum
	FConvexVolume Frustum = BuildCameraFrustum(CameraLocation, CameraRotation, FOV, AspectRatio);

	// Query octree for visible tiles
	TArray<UFragmentTile*> NewVisibleTiles;
	Octree->QueryVisibleTiles(Frustum, NewVisibleTiles);

	UE_LOG(LogFragmentTileManager, Verbose, TEXT("Camera update: %d visible tiles found"), NewVisibleTiles.Num());

	// Update tile states based on new visible set
	UpdateTileStates(NewVisibleTiles);

	// Update last update tracking
	LastCameraPosition = CameraLocation;
	LastUpdateTime = CurrentTime;

	// Process unload timers
	ProcessUnloadTimers(TimeSinceUpdate);
}

void UFragmentTileManager::UpdateTileStates(const TArray<UFragmentTile*>& NewVisibleTiles)
{
	// Build set of new visible tiles for fast lookup
	TSet<UFragmentTile*> NewVisibleSet(NewVisibleTiles);

	// Handle newly visible tiles
	for (UFragmentTile* Tile : NewVisibleTiles)
	{
		if (!Tile)
		{
			continue;
		}

		if (Tile->State == ETileState::Unloaded)
		{
			// Start loading this tile
			StartLoadingTile(Tile);
		}
		else if (Tile->State == ETileState::Loaded)
		{
			// Already loaded, just show it
			ShowTile(Tile);
		}
		else if (Tile->State == ETileState::Unloading)
		{
			// Was unloading, but now visible again - cancel unload
			ShowTile(Tile);
		}
		// Loading and Visible states don't need changes
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
		HideTile(Tile);
	}

	// Update visible tiles list
	VisibleTiles = NewVisibleTiles;

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

	Tile->State = ETileState::Loading;
	Tile->CurrentSpawnIndex = 0;
	Tile->TimeLeftFrustum = 0.0f;

	// Add to loaded tiles tracking
	LoadedTiles.AddUnique(Tile);

	// Update total fragments to spawn
	TotalFragmentsToSpawn += Tile->FragmentLocalIDs.Num();

	UE_LOG(LogFragmentTileManager, Log, TEXT("Started loading tile: %d fragments expanded to %d (with children)"),
	       OriginalCount, Tile->FragmentLocalIDs.Num());

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

	UE_LOG(LogFragmentTileManager, Log, TEXT("Unloading tile with %d actors"), Tile->SpawnedActors.Num());

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

	// Remove from loaded tiles
	LoadedTiles.Remove(Tile);

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

	// Find first tile in Loading state
	UFragmentTile* CurrentTile = nullptr;
	for (UFragmentTile* Tile : LoadedTiles)
	{
		if (Tile && Tile->State == ETileState::Loading)
		{
			CurrentTile = Tile;
			break;
		}
	}

	if (!CurrentTile)
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

	// Spawn fragments from this tile (up to FragmentsPerChunk)
	int32 SpawnedThisFrame = 0;
	while (SpawnedThisFrame < FragmentsPerChunk && CurrentTile->CurrentSpawnIndex < CurrentTile->FragmentLocalIDs.Num())
	{
		if (SpawnFragmentFromTile(CurrentTile))
		{
			SpawnedThisFrame++;
			FragmentsSpawned++;
		}
		else
		{
			// Spawn failed, skip this fragment
			UE_LOG(LogFragmentTileManager, Warning, TEXT("Failed to spawn fragment"));
		}

		CurrentTile->CurrentSpawnIndex++;
	}

	// Check if tile finished loading
	if (CurrentTile->CurrentSpawnIndex >= CurrentTile->FragmentLocalIDs.Num())
	{
		CurrentTile->State = ETileState::Loaded;

		// If this tile is in the visible set, show it
		if (VisibleTiles.Contains(CurrentTile))
		{
			ShowTile(CurrentTile);
		}

		UE_LOG(LogFragmentTileManager, Log, TEXT("Tile finished loading: %d fragments"), CurrentTile->FragmentLocalIDs.Num());
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
