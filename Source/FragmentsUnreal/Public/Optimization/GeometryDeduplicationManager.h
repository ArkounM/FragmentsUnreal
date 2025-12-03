#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GeometryDeduplicationManager.generated.h"

// Represents a single unique geometry shared by multiple instances
USTRUCT(BlueprintType)
struct FGeometryTemplate
{
	GENERATED_BODY()

	// The shared static mesh (created once)
	UPROPERTY()
	UStaticMesh* SharedMesh = nullptr;
	
	// Transform for each instance
	TArray<FTransform> InstanceTransforms;

	// LocalId for each instance (links to metadata)
	TArray<int32> InstanceLocalIds;

	// Material index for each instance
	TArray<int32> InstanceMaterialIndices;

	// Hash of the geometry (for debugging)
	uint64 GeometryHash = 0;

	// How many instances reference this template
	int32 ReferenceCount = 0;

	FGeometryTemplate()
	{
		SharedMesh = nullptr;
		GeometryHash = 0;
		ReferenceCount = 0;
	}
};

// Manage geometry deduplication by hashing meshes and creating shared templates
UCLASS()
class FRAGMENTSUNREAL_API UGeometryDeduplicationManager : public UObject
{
	GENERATED_BODY()

public: 
	UGeometryDeduplicationManager();

	/**
	* Hash geometry to get unique identifier
	* @param Vertices - Vertex Positions
	* @param Triangles - Triangle Indices
	* @return 64-bit hash of geometry
	*/
	static uint64 HashGeometry(const TArray<FVector>& Vertices, const TArray<int32>& Triangles);

	/**
	* Get or create a geometrytemplate
	* @param Vertices - Vertex positions
	* @param Triangles - Triangle Indices
	* @param Normals - Vertex normals
	* @param UVs - Texture coordinates
	* @param MaterialIndex - Material to use
	* @param MeshName - Name for the mesh asset
	* @param Outer - Outer object for mesh creation
	* @return Template
	*/
	FGeometryTemplate* GetOrCreateTemplate(
		const TArray<FVector>& Vertices,
		const TArray<int32>& Triangles,
		const TArray<FVector>& Normals,
		const TArray<FVector2D>& UVs,
		int32 MaterialIndex,
		const FString& MeshName,
		UObject* Outer
	);

	/**
	* Add an instance to an existing template 
	* @param Hash - Geometry hash
	* @param Transform - Instance transform
	* @param LocalId - LocalId for metadata linkage
	* @param MaterialIndex - Material index
	*/
	void AddInstance(uint64 Hash, const FTransform& Transform, int32 LocalId, int32 MaterialIndex);

	/**
	* Get all templates (for spawning ISMCs)
	* @return Map of hash to template
	*/
	const TMap<uint64, FGeometryTemplate*>& GetAllTemplate() const { return GeometryTemplates; }

	// Get stats
	UFUNCTION(BlueprintCallable, Category = "Fragments|Optimization")
	void GetStats(int32& OutUniqueGeometries, int32& OutTotalInstances, float& OutDeduplicationRatio) const;

	// Clear all templates
	void Clear();

private:
	// Map: Geometry Hash -> Template
	TMap<uint64, FGeometryTemplate*> GeometryTemplates;

	// Create a static mesh from geometry data
	UStaticMesh* CreateStaticMeshFromData(
		const TArray<FVector>& Vertices,
		const TArray<int32>& Triangles,
		const TArray<FVector>& Normals,
		const TArray<FVector2D>& UVs,
		const FString& MeshName,
		UObject* Outer
	);
};  // ← Added semicolon