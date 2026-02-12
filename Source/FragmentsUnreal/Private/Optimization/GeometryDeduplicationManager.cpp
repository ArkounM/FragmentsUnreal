#include "Optimization/GeometryDeduplicationManager.h"
#include "Utils/FragmentsLog.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshDescription.h"
#include "StaticMeshAttributes.h"
#include "MeshDescriptionBuilder.h"

UGeometryDeduplicationManager::UGeometryDeduplicationManager()
{

}

uint64 UGeometryDeduplicationManager::HashGeometry(const TArray<FVector>& Vertices, const TArray<int32>& Triangles)
{
	// Use CityHash64 for fast hashing
	uint64 Hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis

    // Hash vertex count
    Hash ^= static_cast<uint64>(Vertices.Num());
    Hash *= 0x100000001b3ULL; // FNV-1a prime

    // Hash triangle count
    Hash ^= static_cast<uint64>(Triangles.Num());
    Hash *= 0x100000001b3ULL;

    // Sample vertices (every 10th to balance performance vs accuracy)
    const int32 SampleInterval = FMath::Max(1, Vertices.Num() / 100);
    for (int32 i = 0; i < Vertices.Num(); i += SampleInterval)
    {
        const FVector& V = Vertices[i];

        // Hash X
        Hash ^= *reinterpret_cast<const uint32*>(&V.X);
        Hash *= 0x100000001b3ULL;

        // Hash Y
        Hash ^= *reinterpret_cast<const uint32*>(&V.Y);
        Hash *= 0x100000001b3ULL;

        // Hash Z
        Hash ^= *reinterpret_cast<const uint32*>(&V.Z);
        Hash *= 0x100000001b3ULL;
    }

    // Hash first and last triangles
    if (Triangles.Num() >= 3)
    {
        Hash ^= static_cast<uint64>(Triangles[0]);
        Hash *= 0x100000001b3ULL;
        Hash ^= static_cast<uint64>(Triangles[1]);
        Hash *= 0x100000001b3ULL;
        Hash ^= static_cast<uint64>(Triangles[2]);
        Hash *= 0x100000001b3ULL;

        if (Triangles.Num() > 3)
        {
            Hash ^= static_cast<uint64>(Triangles[Triangles.Num() - 3]);
            Hash *= 0x100000001b3ULL;
            Hash ^= static_cast<uint64>(Triangles[Triangles.Num() - 2]);
            Hash *= 0x100000001b3ULL;
            Hash ^= static_cast<uint64>(Triangles[Triangles.Num() - 1]);
            Hash *= 0x100000001b3ULL;
        }
    }

    return Hash;
}

void UGeometryDeduplicationManager::Clear()
{
    for (auto& Pair : GeometryTemplates)
    {
        delete Pair.Value;
    }
    GeometryTemplates.Empty();
}

void UGeometryDeduplicationManager::GetStats(int32& OutUniqueGeometries, int32& OutTotalInstances, float& OutDeduplicationRatio) const
{
    OutUniqueGeometries = GeometryTemplates.Num();
    OutTotalInstances = 0;

    for (const auto& Pair : GeometryTemplates)
    {
        OutTotalInstances += Pair.Value->ReferenceCount;
    }

    if (OutUniqueGeometries > 0)
    {
        OutDeduplicationRatio = static_cast<float>(OutTotalInstances) / static_cast<float>(OutUniqueGeometries);
    }
    else
    {
        OutDeduplicationRatio = 0.0f;
    }
}

FGeometryTemplate* UGeometryDeduplicationManager::GetOrCreateTemplate(
    const TArray<FVector>& Vertices,
    const TArray<int32>& Triangles,
    const TArray<FVector>& Normals,
    const TArray<FVector2D>& UVs,
    int32 MaterialIndex,
    const FString& MeshName,
    UObject* Outer)
{
    // Hash the geometry
    uint64 Hash = HashGeometry(Vertices, Triangles);

    // Check if template already exists
    if (FGeometryTemplate** FoundTemplate = GeometryTemplates.Find(Hash))
    {
        UE_LOG(LogFragments, Verbose, TEXT("Geometry template found (hash: %llu), reusing"), Hash);
        return *FoundTemplate;
    }

    // Create new template
    UE_LOG(LogFragments, Log, TEXT("Creating new geometry template (hash: %llu, verts: %d, tris: %d)"),
        Hash, Vertices.Num(), Triangles.Num() / 3);

    FGeometryTemplate* NewTemplate = new FGeometryTemplate();
    NewTemplate->GeometryHash = Hash;
    NewTemplate->ReferenceCount = 0;

    // Create the static mesh
    NewTemplate->SharedMesh = CreateStaticMeshFromData(
        Vertices, Triangles, Normals, UVs, MeshName, Outer
    );

    if (!NewTemplate->SharedMesh)
    {
        UE_LOG(LogFragments, Error, TEXT("Failed to create static mesh for template"));
        delete NewTemplate;
        return nullptr;
    }

    // Store Template
    GeometryTemplates.Add(Hash, NewTemplate);

    return NewTemplate;
}

void UGeometryDeduplicationManager::AddInstance(uint64 Hash, const FTransform& Transform, int32 LocalId, int32 MaterialIndex)
{
    if (FGeometryTemplate** FoundTemplate = GeometryTemplates.Find(Hash))
    {
        FGeometryTemplate* Template = *FoundTemplate;
        Template->InstanceTransforms.Add(Transform);
        Template->InstanceLocalIds.Add(LocalId);
        Template->InstanceMaterialIndices.Add(MaterialIndex);
        Template->ReferenceCount++;
    }
    else
    {
        UE_LOG(LogFragments, Warning, TEXT("Attempted to add instance to non-existent template (hash: %llu)"), Hash);
    }
}

UStaticMesh* UGeometryDeduplicationManager::CreateStaticMeshFromData(
    const TArray<FVector>& Vertices,
    const TArray<int32>& Triangles,
    const TArray<FVector>& Normals,
    const TArray<FVector2D>& UVs,
    const FString& MeshName,
    UObject* Outer)
{
    // Create static mesh object
    UStaticMesh* StaticMesh = NewObject<UStaticMesh>(Outer, FName(*MeshName), RF_Public | RF_Standalone);

    if (!StaticMesh)
    {
        return nullptr;
    }

    StaticMesh->InitResources();
    StaticMesh->SetLightingGuid();

    // Create mesh description
    FMeshDescription MeshDescription;
    FStaticMeshAttributes Attributes(MeshDescription);
    Attributes.Register();

    FMeshDescriptionBuilder MeshDescBuilder;
    MeshDescBuilder.SetMeshDescription(&MeshDescription);

    // Enable all attributes
    MeshDescBuilder.EnablePolyGroups();
    MeshDescBuilder.SetNumUVLayers(1);

    // Create a polygon group (required before adding triangles)
    FPolygonGroupID PolygonGroupID = MeshDescription.CreatePolygonGroup();

    // Add vertices
    TArray<FVertexID> VertexIDs;
    VertexIDs.Reserve(Vertices.Num());

    for (int32 i = 0; i < Vertices.Num(); i++)
    {
        FVertexID VertexID = MeshDescBuilder.AppendVertex(Vertices[i]);
        VertexIDs.Add(VertexID);
    }

    // Add triangles
    const int32 NumTriangles = Triangles.Num() / 3;

    for (int32 TriIdx = 0; TriIdx < NumTriangles; TriIdx++)
    {
        int32 BaseIdx = TriIdx * 3;

        if (BaseIdx + 2 >= Triangles.Num())
        {
            break;
        }

        int32 Idx0 = Triangles[BaseIdx + 0];
        int32 Idx1 = Triangles[BaseIdx + 1];
        int32 Idx2 = Triangles[BaseIdx + 2];

        // Validate indices
        if (Idx0 < 0 || Idx0 >= VertexIDs.Num() ||
            Idx1 < 0 || Idx1 >= VertexIDs.Num() ||
            Idx2 < 0 || Idx2 >= VertexIDs.Num())
        {
            continue;
        }

        // Create triangle
        FVertexInstanceID VertexInstanceID0 = MeshDescBuilder.AppendInstance(VertexIDs[Idx0]);
        FVertexInstanceID VertexInstanceID1 = MeshDescBuilder.AppendInstance(VertexIDs[Idx1]);
        FVertexInstanceID VertexInstanceID2 = MeshDescBuilder.AppendInstance(VertexIDs[Idx2]);

        // Set normals if available
        if (Normals.Num() == Vertices.Num())
        {
            MeshDescBuilder.SetInstanceNormal(VertexInstanceID0, Normals[Idx0]);
            MeshDescBuilder.SetInstanceNormal(VertexInstanceID1, Normals[Idx1]);
            MeshDescBuilder.SetInstanceNormal(VertexInstanceID2, Normals[Idx2]);
        }

        // Set UVs if available
        if (UVs.Num() == Vertices.Num())
        {
            MeshDescBuilder.SetInstanceUV(VertexInstanceID0, UVs[Idx0], 0);
            MeshDescBuilder.SetInstanceUV(VertexInstanceID1, UVs[Idx1], 0);
            MeshDescBuilder.SetInstanceUV(VertexInstanceID2, UVs[Idx2], 0);
        }

        // Append triangle (use the PolygonGroupID we created earlier)
        MeshDescBuilder.AppendTriangle(VertexInstanceID0, VertexInstanceID1, VertexInstanceID2, PolygonGroupID);
    }

    // Build the static mesh
    TArray<const FMeshDescription*> MeshDescriptions;
    MeshDescriptions.Add(&MeshDescription);

    StaticMesh->BuildFromMeshDescriptions(MeshDescriptions);

    return StaticMesh;
}
