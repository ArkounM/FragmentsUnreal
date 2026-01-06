#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "FragmentTile.generated.h"

/**
 * Tile state for streaming system
 */
UENUM(BlueprintType)
enum class ETileState : uint8
{
	Unloaded    UMETA(DisplayName = "Unloaded"),
	Loading     UMETA(DisplayName = "Loading"),
	Loaded      UMETA(DisplayName = "Loaded"),
	Visible     UMETA(DisplayName = "Visible"),
	Unloading   UMETA(DisplayName = "Unloading")
};

/**
 * Represents a spatial tile containing a subset of fragments within a bounding box.
 * This is the fundamental unit of streaming in the tile-based system.
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentTile : public UObject
{
	GENERATED_BODY()

public:
	// Tile bounds in world space
	UPROPERTY()
	FBox Bounds;

	// Fragment LocalIDs in this tile
	UPROPERTY()
	TArray<int32> FragmentLocalIDs;

	// Spawned actors (when Loaded/Visible)
	UPROPERTY()
	TArray<class AFragment*> SpawnedActors;

	// Mapping of LocalID → spawned actor (for parent lookup during spawning)
	UPROPERTY()
	TMap<int32, class AFragment*> LocalIdToActor;

	// Current state
	UPROPERTY()
	ETileState State;

	// Time when tile left frustum (for unload hysteresis)
	UPROPERTY()
	float TimeLeftFrustum;

	// Spawn queue index (for chunked spawning)
	int32 CurrentSpawnIndex;

	UFragmentTile()
		: Bounds(ForceInit)
		, State(ETileState::Unloaded)
		, TimeLeftFrustum(0.0f)
		, CurrentSpawnIndex(0)
	{
	}

	void Initialize(const FBox& InBounds)
	{
		Bounds = InBounds;
		State = ETileState::Unloaded;
		TimeLeftFrustum = 0.0f;
		CurrentSpawnIndex = 0;
	}
};
