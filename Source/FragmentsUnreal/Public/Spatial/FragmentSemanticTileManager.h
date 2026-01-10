#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "FragmentSemanticTileManager.generated.h"

// Forward declarations
class UFragmentsImporter;
class UFragmentOctree;

/**
 * Priority tiers for IFC class loading order
 * Lower values load first
 */
UENUM(BlueprintType)
enum class EFragmentPriority : uint8
{
	STRUCTURAL = 0  UMETA(DisplayName = "Structural"),     // Walls, Floors, Roofs, Beams, Columns
	OPENINGS = 1    UMETA(DisplayName = "Openings"),       // Windows, Doors
	FURNISHINGS = 2 UMETA(DisplayName = "Furnishings"),   // Furniture, Fixtures
	DETAILS = 3     UMETA(DisplayName = "Details")         // Railings, Fasteners, Details
};

/**
 * LOD levels for semantic tiles
 */
UENUM(BlueprintType)
enum class ESemanticLOD : uint8
{
	Unloaded    UMETA(DisplayName = "Unloaded"),      // Not visible
	Wireframe   UMETA(DisplayName = "Wireframe"),     // LOD 0: Bounding box wireframe
	SimpleBox   UMETA(DisplayName = "SimpleBox"),     // LOD 1: Solid colored box
	HighDetail  UMETA(DisplayName = "HighDetail")     // LOD 2: Full mesh detail
};

/**
 * Semantic tile representing all fragments of a specific IFC class
 * Groups elements by their IFC type (e.g., IfcWall, IfcWindow) for priority-based loading
 */
UCLASS()
class FRAGMENTSUNREAL_API USemanticTile : public UObject
{
	GENERATED_BODY()

public:
	/** IFC class name (e.g., "IfcWall", "IfcWindow", "IfcDoor") */
	UPROPERTY()
	FString IFCClassName;

	/** Loading priority tier */
	UPROPERTY()
	EFragmentPriority Priority;

	/** Combined bounding box of all elements in this class */
	UPROPERTY()
	FBox CombinedBounds;

	/** All fragment LocalIDs belonging to this IFC class */
	UPROPERTY()
	TArray<int32> FragmentIDs;

	/** Count of fragments in this class */
	UPROPERTY()
	int32 Count;

	/** Is this tile currently loaded? */
	UPROPERTY()
	bool bIsLoaded;

	/** Representative color for this IFC class (for wireframe LOD) */
	UPROPERTY()
	FLinearColor RepresentativeColor;

	/** Current LOD level */
	UPROPERTY()
	ESemanticLOD CurrentLOD;

	/** Target LOD level (for transitions) */
	UPROPERTY()
	ESemanticLOD TargetLOD;

	/** Simple box mesh component (LOD 1) */
	UPROPERTY()
	class UProceduralMeshComponent* SimpleBoxMesh;

	/** Current screen coverage percentage (0.0 to 1.0) */
	float ScreenCoverage;

	/** Last update time for this tile */
	double LastUpdateTime;

	USemanticTile()
		: IFCClassName(TEXT(""))
		, Priority(EFragmentPriority::DETAILS)
		, CombinedBounds(ForceInit)
		, Count(0)
		, bIsLoaded(false)
		, RepresentativeColor(FLinearColor::Gray)
		, CurrentLOD(ESemanticLOD::Unloaded)
		, TargetLOD(ESemanticLOD::Unloaded)
		, SimpleBoxMesh(nullptr)
		, ScreenCoverage(0.0f)
		, LastUpdateTime(0.0)
	{
	}
};

/**
 * Configuration for semantic tile system
 */
USTRUCT(BlueprintType)
struct FSemanticTileConfig
{
	GENERATED_BODY()

	/** Time budget per frame in milliseconds (default: 16ms for 60 FPS) */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "1.0", ClampMax = "33.0"))
	float FrameBudgetMs = 16.0f;

	/** Minimum number of tiles to process per frame (regardless of time budget) */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "1", ClampMax = "100"))
	int32 MinTilesPerFrame = 8;

	/** Enable debug logging for semantic tile operations */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bEnableDebugLogging = false;

	/** Draw debug bounds for semantic tiles */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bDrawDebugBounds = false;

	/** Screen coverage threshold for LOD 0 → LOD 1 transition (default: 1%) */
	UPROPERTY(EditAnywhere, Category = "LOD", meta = (ClampMin = "0.001", ClampMax = "0.1"))
	float LOD0ToLOD1Threshold = 0.01f;

	/** Screen coverage threshold for LOD 1 → LOD 2 transition (default: 5%) */
	UPROPERTY(EditAnywhere, Category = "LOD", meta = (ClampMin = "0.01", ClampMax = "0.5"))
	float LOD1ToLOD2Threshold = 0.05f;

	/** Enable LOD system (if false, always use high detail) */
	UPROPERTY(EditAnywhere, Category = "LOD")
	bool bEnableLOD = true;
};

/**
 * Manages semantic tile-based loading of fragments grouped by IFC class
 *
 * Phase 1 Implementation:
 * - Extracts IFC classes from loaded model
 * - Groups fragments by IFC class (semantic tiles)
 * - Assigns priority tiers based on element importance
 * - Provides foundation for LOD system (Phase 2+)
 *
 * Future Phases:
 * - Phase 2: LOD generation (wireframe, simple box, full mesh)
 * - Phase 3: Progressive LOD loading
 * - Phase 4: Octree spatial subdivision
 */
UCLASS()
class FRAGMENTSUNREAL_API UFragmentSemanticTileManager : public UObject
{
	GENERATED_BODY()

public:
	UFragmentSemanticTileManager();

	/**
	 * Initialize the semantic tile manager
	 * @param InModelGuid Model identifier
	 * @param InImporter Reference to importer for fragment data access
	 */
	void Initialize(const FString& InModelGuid, UFragmentsImporter* InImporter);

	/**
	 * Build semantic tiles from the loaded model
	 * Extracts IFC classes, groups fragments, calculates bounds
	 */
	void BuildSemanticTiles();

	/**
	 * Update semantic tiles based on camera state (Phase 1: placeholder for future LOD logic)
	 * @param CameraLocation Camera position in world space
	 * @param CameraRotation Camera orientation
	 * @param FOV Field of view in degrees
	 * @param ViewportHeight Viewport height in pixels
	 */
	void Tick(float DeltaTime, const FVector& CameraLocation, const FRotator& CameraRotation,
	          float FOV, float ViewportHeight);

	/**
	 * Get all semantic tiles
	 */
	const TArray<USemanticTile*>& GetSemanticTiles() const { return AllSemanticTiles; }

	/**
	 * Get semantic tile by IFC class name
	 * @param IFCClassName IFC class to lookup (e.g., "IfcWall")
	 * @return Semantic tile or nullptr if not found
	 */
	USemanticTile* GetSemanticTile(const FString& IFCClassName) const;

	/**
	 * Get tiles by priority tier
	 * @param Priority Priority tier to filter by
	 * @return Array of semantic tiles with specified priority
	 */
	TArray<USemanticTile*> GetTilesByPriority(EFragmentPriority Priority) const;

	/**
	 * Get total number of fragments across all semantic tiles
	 */
	int32 GetTotalFragmentCount() const;

	/**
	 * Get configuration
	 */
	FSemanticTileConfig& GetConfig() { return Config; }

private:
	// --- LOD Management Methods (Phase 2) ---

	/**
	 * Calculate screen coverage percentage for a tile
	 * @param Tile Semantic tile to evaluate
	 * @param CameraLocation Camera position
	 * @param CameraRotation Camera rotation
	 * @param FOV Field of view in degrees
	 * @param ViewportHeight Viewport height in pixels
	 * @return Screen coverage as percentage (0.0 to 1.0)
	 */
	float CalculateScreenCoverage(USemanticTile* Tile, const FVector& CameraLocation,
	                               const FRotator& CameraRotation, float FOV, float ViewportHeight);

	/**
	 * Determine target LOD level based on screen coverage
	 * @param ScreenCoverage Screen coverage percentage (0.0 to 1.0)
	 * @return Target LOD level
	 */
	ESemanticLOD DetermineLODLevel(float ScreenCoverage);

	/**
	 * Transition a tile to target LOD level
	 * @param Tile Tile to transition
	 * @param TargetLOD Target LOD level
	 */
	void TransitionToLOD(USemanticTile* Tile, ESemanticLOD TargetLOD);

	/**
	 * Show wireframe visualization for LOD 0
	 * @param Tile Tile to visualize
	 */
	void ShowWireframe(USemanticTile* Tile);

	/**
	 * Show simple box visualization for LOD 1
	 * @param Tile Tile to visualize
	 */
	void ShowSimpleBox(USemanticTile* Tile);

	/**
	 * Show high detail meshes for LOD 2
	 * @param Tile Tile to load
	 */
	void ShowHighDetail(USemanticTile* Tile);

	/**
	 * Hide current LOD visualization for a tile
	 * @param Tile Tile to hide
	 */
	void HideLOD(USemanticTile* Tile);

	// --- Existing Helper Methods ---

	/**
	 * Extract IFC class from fragment item
	 * @param LocalID Fragment LocalID
	 * @return IFC class name (e.g., "IfcWall") or "Unknown" if not found
	 */
	FString ExtractIFCClass(int32 LocalID);

	/**
	 * Determine priority tier for IFC class
	 * @param IFCClassName IFC class name
	 * @return Priority tier (Structural, Openings, Furnishings, Details)
	 */
	EFragmentPriority DeterminePriority(const FString& IFCClassName);

	/**
	 * Get representative color for IFC class
	 * @param IFCClassName IFC class name
	 * @return Color for visualization (wireframe LOD)
	 */
	FLinearColor GetRepresentativeColor(const FString& IFCClassName);

	/**
	 * Calculate combined bounding box for fragments in semantic tile
	 * @param Tile Semantic tile to calculate bounds for
	 */
	void CalculateCombinedBounds(USemanticTile* Tile);

	/** Model GUID */
	FString ModelGuid;

	/** Importer reference */
	UPROPERTY()
	UFragmentsImporter* Importer = nullptr;

	/** Root actor for attaching LOD visualization components */
	UPROPERTY()
	AActor* RootActor = nullptr;

	/** Map: IFC Class Name → Semantic Tile */
	UPROPERTY()
	TMap<FString, USemanticTile*> SemanticTileMap;

	/** All semantic tiles (for iteration) */
	UPROPERTY()
	TArray<USemanticTile*> AllSemanticTiles;

	/** Tiles organized by priority tier (for priority-based updates) */
	TArray<USemanticTile*> TilesByPriority[4];  // One array per EFragmentPriority (not serialized, rebuilt on load)

	/** Configuration */
	UPROPERTY()
	FSemanticTileConfig Config;
};
