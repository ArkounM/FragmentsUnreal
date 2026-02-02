

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Index/index_generated.h"
#include "FragmentsUtils.generated.h"

struct FFragmentEdge
{
	float A, B;

	FFragmentEdge(float InA, float InB)
	{
		A = InA;
		B = InB;
	}

	bool operator==(const FFragmentEdge& Other) const
	{
		return (A == Other.A && B == Other.B) || (A == Other.B && B == Other.A);
	}
};

struct FPlaneProjection
{
	FVector Origin;
	FVector AxisX;
	FVector AxisY;

	FVector2D Project(const FVector& Point) const
	{
		FVector Local = Point - Origin;
		return FVector2D(FVector::DotProduct(Local, AxisX), FVector::DotProduct(Local, AxisY));
	}

	FVector Unproject(const FVector2D& Point2D) const
	{
		return Origin + AxisX * Point2D.X + AxisY * Point2D.Y;
	}
};

struct FProjectionPlane
{
	FVector Origin;
	TArray<FVector> OriginalPoints;
	FVector U;
	FVector V;

	bool Initialize(const TArray<FVector>& Points)
	{
		if (Points.Num() < 3)
		{
			UE_LOG(LogTemp, Error, TEXT("[Projection] Not enoguht points (%d) to define a plane."), Points.Num());
			return false;
		}

		OriginalPoints = Points;
		Origin = Points[0];

		FVector A, B;
		bool bFound = false;
		for (int32 i = 1; i < Points.Num() - 1; i++)
		{
			A = Points[i] - Origin;
			B = Points[i + 1] - Origin;

			if (!A.IsNearlyZero() && !B.IsNearlyZero() && !A.Equals(B, KINDA_SMALL_NUMBER))
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			UE_LOG(LogTemp, Error, TEXT("[Projection] Could not find non-collinear points."));
			return false;
		}

		FVector Normal = FVector::CrossProduct(A, B).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			UE_LOG(LogTemp, Error, TEXT("[Projection] Cross product resulted in zero normal (points may be collinear)."));
			return false;
		}

		U = A.GetSafeNormal();
		V = FVector::CrossProduct(Normal, U);
		return true;
	}

	FVector2D Project(const FVector& P) const {
		FVector Local = P - Origin;
		return FVector2D(FVector::DotProduct(Local, U), FVector::DotProduct(Local, V));
	}
};

struct FTriangulationResult
{
	TArray<FVector> FlattenedPoints;
	TArray<int32> TriangleIndices;
};

/**
 * Pre-extracted geometry data for a fragment sample.
 * Contains all geometry data extracted from FlatBuffers at load time,
 * eliminating the need for FlatBuffer access during spawn phase.
 * This solves the crash issue where FlatBuffer pointers become invalid
 * when accessed via the async/TileManager path.
 *
 * Note: This is a plain C++ struct (not USTRUCT) because Unreal's reflection
 * system doesn't support nested TArrays. This is runtime-only data that
 * doesn't need serialization.
 */
struct FPreExtractedGeometry
{
	// Vertex data (already converted to Unreal coordinates: Z-up, cm units)
	TArray<FVector> Vertices;

	// Profile indices - each profile's vertex indices into the Vertices array
	TArray<TArray<int32>> ProfileIndices;

	// Holes per profile - ProfileHoles[i] contains holes for profile i
	TArray<TArray<TArray<int32>>> ProfileHoles;

	// Local transform for this sample
	FTransform LocalTransform;

	// Material data
	uint8 R = 255;
	uint8 G = 255;
	uint8 B = 255;
	uint8 A = 255;
	bool bIsGlass = false;

	// Geometry type
	bool bIsShell = true;  // true = Shell, false = CircleExtrusion

	// Validation flag - if false, geometry should be skipped during spawn
	bool bIsValid = false;

	// Track if extraction was attempted (to avoid repeated failures)
	bool bExtractionAttempted = false;

	// Representation ID (for debugging/logging)
	int32 RepresentationId = -1;

	FPreExtractedGeometry() = default;
};

USTRUCT(BlueprintType)
struct FFragmentLookup
{
	GENERATED_BODY()

public:

	UPROPERTY()
	TMap<int32, class AFragment*> Fragments;
};

USTRUCT(BlueprintType)
struct FFragmentSample
{
	GENERATED_BODY()

	// Original indices (kept for debugging and backward compatibility)
	UPROPERTY()
	int32 SampleIndex = -1;
	UPROPERTY()
	int32 LocalTransformIndex = -1;
	UPROPERTY()
	int32 RepresentationIndex = -1;
	UPROPERTY()
	int32 MaterialIndex = -1;

	// Pre-extracted geometry data (populated at load time)
	// This eliminates FlatBuffer access during spawn phase
	// Note: Not UPROPERTY because FPreExtractedGeometry contains nested TArrays
	FPreExtractedGeometry ExtractedGeometry;
};


USTRUCT(BlueprintType)
struct FItemAttribute
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FString Key;

	UPROPERTY(BlueprintReadOnly)
	FString Value;

	UPROPERTY(BlueprintReadOnly)
	int64 TypeHash;

	FItemAttribute() : Key(TEXT("")), Value(TEXT("")), TypeHash(0) {}
	FItemAttribute(const FString& InKey, const FString& InValue, int64 InTypeHash)
		:Key(InKey), Value(InValue), TypeHash(InTypeHash) {}
};

USTRUCT()
struct FFragmentItem
{

	GENERATED_BODY()

	FString ModelGuid;
	int32 LocalId;
	FString Category;
	FString Guid;
	TArray<FItemAttribute> Attributes;  // List of attributes for the fragment
	TArray <FFragmentItem*> FragmentChildren;  // Use TObjectPtr for nested recursion
	TArray<FFragmentSample> Samples;
	FTransform GlobalTransform;

	bool FindFragmentByLocalId(int32 InLocalId, FFragmentItem*& OutItem)
	{
		// Check if the current item matches the LocalId
		if (LocalId == InLocalId)
		{
			OutItem = this;
			return true;
		}

		// If not, search through all children recursively
		for (FFragmentItem* Child : FragmentChildren)
		{
			if (Child->FindFragmentByLocalId(InLocalId, OutItem))
			{
				return true;
			}
		}

		// If no match is found, return nullptr
		return false;
	}

	// Const version
	bool FindFragmentByLocalId(int32 InLocalId, FFragmentItem*& OutItem) const
	{
		// Check if the current item matches the LocalId
		if (LocalId == InLocalId)
		{
			OutItem = const_cast<FFragmentItem*>(this);
			return true;
		}

		// If not, search through all children recursively
		for (FFragmentItem* Child : FragmentChildren)
		{
			if (Child->FindFragmentByLocalId(InLocalId, OutItem))
			{
				return true;
			}
		}

		// If no match is found, return nullptr
		return false;
	}
};

// Forward declaration for FFindResult
class AFragment;

/**
 * Lightweight proxy for instanced BIM elements.
 * ~200 bytes vs ~5KB for AFragment actor.
 * Used for fragments rendered via UHierarchicalInstancedStaticMeshComponent.
 */
USTRUCT(BlueprintType)
struct FFragmentProxy
{
	GENERATED_BODY()

	/** Weak reference to the HISMC containing this instance */
	UPROPERTY()
	TWeakObjectPtr<class UHierarchicalInstancedStaticMeshComponent> ISMC;

	/** Index of this instance within the ISMC */
	UPROPERTY()
	int32 InstanceIndex = INDEX_NONE;

	/** LocalId of the fragment in the BIM model */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	int32 LocalId = INDEX_NONE;

	/** Global unique ID (GUID) of the fragment */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString GlobalId;

	/** IFC category (e.g., IfcDoor, IfcWindow) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString Category;

	/** Model GUID this fragment belongs to */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FString ModelGuid;

	/** Attributes/properties of the BIM element */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	TArray<FItemAttribute> Attributes;

	/** Parent fragment LocalId (INDEX_NONE if root) */
	UPROPERTY()
	int32 ParentLocalId = INDEX_NONE;

	/** Child fragment LocalIds */
	UPROPERTY()
	TArray<int32> ChildLocalIds;

	/** Cached world transform of this instance */
	UPROPERTY()
	FTransform WorldTransform;

	FFragmentProxy() = default;
};

/**
 * Pending instance data collected during spawn phase.
 * Used for batch instance addition after spawning completes.
 */
struct FPendingInstanceData
{
	FTransform WorldTransform;
	int32 LocalId = INDEX_NONE;
	FString GlobalId;
	FString Category;
	FString ModelGuid;
	TArray<FItemAttribute> Attributes;

	FPendingInstanceData() = default;
	FPendingInstanceData(const FTransform& InTransform, int32 InLocalId, const FString& InGlobalId,
		const FString& InCategory, const FString& InModelGuid, const TArray<FItemAttribute>& InAttributes)
		: WorldTransform(InTransform), LocalId(InLocalId), GlobalId(InGlobalId),
		  Category(InCategory), ModelGuid(InModelGuid), Attributes(InAttributes) {}
};

/**
 * HISMC group for a RepresentationId + Material combination.
 * Each unique geometry+material pair gets one HISMC containing all instances.
 * Uses Hierarchical ISM for per-cluster culling performance.
 */
USTRUCT()
struct FInstancedMeshGroup
{
	GENERATED_BODY()

	/** The HierarchicalInstancedStaticMeshComponent for this group */
	UPROPERTY()
	class UHierarchicalInstancedStaticMeshComponent* ISMC = nullptr;

	/** RepresentationId from FlatBuffers (unique geometry ID) */
	UPROPERTY()
	int32 RepresentationId = INDEX_NONE;

	/** Hash of material properties (color + glass flag) */
	UPROPERTY()
	uint32 MaterialHash = 0;

	/** Number of instances in this ISMC */
	UPROPERTY()
	int32 InstanceCount = 0;

	/** Map from ISMC instance index to BIM LocalId */
	TMap<int32, int32> InstanceToLocalId;

	/** Map from BIM LocalId to ISMC instance index */
	TMap<int32, int32> LocalIdToInstance;

	/** Pending instances to be batch-added (collected during spawn phase) */
	TArray<FPendingInstanceData> PendingInstances;

	/** Cached mesh for batch creation */
	UPROPERTY()
	class UStaticMesh* CachedMesh = nullptr;

	/** Cached material for batch creation */
	UPROPERTY()
	class UMaterialInstanceDynamic* CachedMaterial = nullptr;

	/** Category from the first instance (for occlusion classification) */
	FString FirstCategory;

	/** Material alpha from the first instance (for occlusion classification, 0-255) */
	uint8 FirstMaterialAlpha = 255;

	FInstancedMeshGroup() = default;
};

/**
 * Unified lookup result for both actors and instanced proxies.
 * Allows querying fragments regardless of whether they are
 * spawned as individual actors or GPU-instanced.
 */
USTRUCT(BlueprintType)
struct FRAGMENTSUNREAL_API FFindResult
{
	GENERATED_BODY()

	/** Whether a fragment was found */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bFound = false;

	/** Whether the fragment is GPU-instanced (true) or an actor (false) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	bool bIsInstanced = false;

	/** Fragment actor (valid if bIsInstanced == false) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	AFragment* Fragment = nullptr;

	/** Proxy data (valid if bIsInstanced == true) */
	UPROPERTY(BlueprintReadOnly, Category = "Fragment")
	FFragmentProxy Proxy;

	/** Get LocalId regardless of instancing */
	int32 GetLocalId() const;

	/** Get Category regardless of instancing */
	FString GetCategory() const;

	/** Get world transform regardless of instancing */
	FTransform GetWorldTransform() const;

	/** Create a not-found result */
	static FFindResult NotFound();

	/** Create a result from an actor */
	static FFindResult FromActor(AFragment* Actor);

	/** Create a result from a proxy */
	static FFindResult FromProxy(const FFragmentProxy& InProxy);

	FFindResult() = default;
};

/**
 *
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentsUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	//UFUNCTION(BlueprintCallable, Category = "Fragments")
	static FTransform MakeTransform(const Transform* FragmentsTransform, bool bIsLocalTransform = false);
	static FPlaneProjection BuildProjectionPlane(const TArray<FVector>& Points, const TArray<int32>& Profile);
	static bool IsClockwise(const TArray<FVector2D>& Points);
	static TArray<FItemAttribute> ParseItemAttribute(const Attribute* Attr);
	static class AFragment* MapModelStructure(const SpatialStructure* InS, AFragment*& ParentActor, TMap<int32, AFragment*>& FragmentLookupMapRef, const FString& InheritedCategory);
	static void MapModelStructureToData(const SpatialStructure* InS, FFragmentItem& ParentItem, const FString& InheritedCategory);
	static FString GetIfcCategory(const int64 InTypeHash);
	static float SafeComponent(float Value);
	static FVector SafeVector(const FVector& Vec);
	static FRotator SafeRotator(const FRotator& Rot);
	static int32 GetIndexForLocalId(const Model* InModelRef, int32 LocalId);

private:

	//void MapSpatialStructureRecursive(const SpatialStructure* Node, int32 ParentId, TArray<FSpatialStructure>& OutList);

};
