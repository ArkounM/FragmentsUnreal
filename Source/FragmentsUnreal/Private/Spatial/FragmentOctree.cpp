#include "Spatial/FragmentOctree.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogFragmentOctree, Log, All);

UFragmentOctree::UFragmentOctree()
	: Root(nullptr)
{
}

UFragmentOctree::~UFragmentOctree()
{
	// Clean up root and all children
	if (Root)
	{
		delete Root;
		Root = nullptr;
	}

	// Tiles are UObjects - garbage collector will handle cleanup
	Tiles.Empty();
}

void UFragmentOctree::BuildFromModel(const UFragmentModelWrapper* ModelWrapper, const FString& ModelGuid)
{
	if (!ModelWrapper)
	{
		UE_LOG(LogFragmentOctree, Error, TEXT("BuildFromModel: Null ModelWrapper"));
		return;
	}

	const Model* ParsedModel = ModelWrapper->GetParsedModel();
	if (!ParsedModel)
	{
		UE_LOG(LogFragmentOctree, Error, TEXT("BuildFromModel: No parsed model"));
		return;
	}

	ModelGuidRef = ModelGuid;

	// Extract fragment bounding boxes from model hierarchy (not just positions!)
	TMap<int32, FBox> FragmentBounds;
	TArray<int32> AllFragmentIDs;

	const FFragmentItem& RootItem = ModelWrapper->GetModelItemRef();
	CollectFragmentBounds(RootItem, ParsedModel, FragmentBounds, AllFragmentIDs);

	if (AllFragmentIDs.Num() == 0)
	{
		UE_LOG(LogFragmentOctree, Warning, TEXT("BuildFromModel: No fragments found"));
		return;
	}

	// Calculate world bounds from actual mesh extents
	FBox WorldBounds = CalculateBounds(AllFragmentIDs, FragmentBounds);

	if (!WorldBounds.IsValid)
	{
		UE_LOG(LogFragmentOctree, Error, TEXT("BuildFromModel: Invalid world bounds"));
		return;
	}

	UE_LOG(LogFragmentOctree, Log, TEXT("Octree WorldBounds: Min=%s Max=%s Size=%s"),
	       *WorldBounds.Min.ToString(),
	       *WorldBounds.Max.ToString(),
	       *WorldBounds.GetSize().ToString());

	// Create root node
	Root = new FFragmentOctreeNode();
	Root->Bounds = WorldBounds;
	Root->Depth = 0;

	const double StartTime = FPlatformTime::Seconds();

	// Build tree recursively
	BuildNode(Root, AllFragmentIDs, FragmentBounds, 0);

	const double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	UE_LOG(LogFragmentOctree, Log, TEXT("Octree built in %.2f ms: %d fragments, %d tiles"),
	       ElapsedTime * 1000.0, AllFragmentIDs.Num(), Tiles.Num());
}

void UFragmentOctree::CollectFragmentBounds(const FFragmentItem& Item,
                                             const Model* ParsedModel,
                                             TMap<int32, FBox>& OutBounds,
                                             TArray<int32>& OutFragmentIDs)
{
	// Only process fragments with valid LocalId and at least one sample (representation)
	if (Item.LocalId >= 0 && Item.Samples.Num() > 0)
	{
		FBox CombinedBounds(ForceInit);
		bool bHasValidBounds = false;

		// Iterate through all samples (representations) for this fragment
		for (const FFragmentSample& Sample : Item.Samples)
		{
			if (Sample.RepresentationIndex < 0 || !ParsedModel)
			{
				continue;
			}

			// Get meshes from model
			const Meshes* MeshesRef = ParsedModel->meshes();
			if (!MeshesRef)
			{
				continue;
			}

			// Get representations from meshes
			const auto* Representations = MeshesRef->representations();
			if (!Representations || Sample.RepresentationIndex >= static_cast<int32>(Representations->size()))
			{
				continue;
			}

			const Representation* Rep = Representations->Get(Sample.RepresentationIndex);
			if (!Rep)
			{
				continue;
			}

			// Get bounding box from representation (returns reference, not pointer)
			const BoundingBox& bbox = Rep->bbox();

			// Get min/max vectors (returns references, not pointers)
			const FloatVector& min = bbox.min();
			const FloatVector& max = bbox.max();

			// Convert FlatBuffers bbox to Unreal FBox
			// Note: Coordinate transform (x,y,z) → (x*100, z*100, y*100)
			FVector Min(
				min.x() * 100.0f,
				min.z() * 100.0f,
				min.y() * 100.0f
			);
			FVector Max(
				max.x() * 100.0f,
				max.z() * 100.0f,
				max.y() * 100.0f
			);

			FBox RepBounds(Min, Max);

			// Apply fragment's global transform to bounding box
			FTransform GlobalTransform = Item.GlobalTransform;
			FBox TransformedBounds = RepBounds.TransformBy(GlobalTransform);

			// Combine with other representations
			if (bHasValidBounds)
			{
				CombinedBounds += TransformedBounds;
			}
			else
			{
				CombinedBounds = TransformedBounds;
				bHasValidBounds = true;
			}
		}

		// Store combined bounds for this fragment
		if (bHasValidBounds)
		{
			OutBounds.Add(Item.LocalId, CombinedBounds);
			OutFragmentIDs.Add(Item.LocalId);
		}
		else
		{
			// Fallback to position-based point if no bbox available
			FVector Position = Item.GlobalTransform.GetLocation();
			FBox PointBounds(Position, Position);
			OutBounds.Add(Item.LocalId, PointBounds.ExpandBy(50.0f)); // 50cm safety margin
			OutFragmentIDs.Add(Item.LocalId);

			UE_LOG(LogFragmentOctree, Warning, TEXT("Fragment %d has no valid bbox, using position fallback"), Item.LocalId);
		}
	}

	// Recurse to children
	for (FFragmentItem* Child : Item.FragmentChildren)
	{
		if (Child)
		{
			CollectFragmentBounds(*Child, ParsedModel, OutBounds, OutFragmentIDs);
		}
	}
}

FBox UFragmentOctree::CalculateBounds(const TArray<int32>& FragmentIDs,
                                       const TMap<int32, FBox>& FragmentBounds)
{
	FBox Bounds(ForceInit);

	for (int32 ID : FragmentIDs)
	{
		const FBox* FragBounds = FragmentBounds.Find(ID);
		if (FragBounds && FragBounds->IsValid)
		{
			// Combine actual mesh bounds (not point cloud!)
			Bounds += *FragBounds;
		}
	}

	// No artificial padding needed - bounds are already accurate
	return Bounds;
}

void UFragmentOctree::BuildNode(FFragmentOctreeNode* Node, const TArray<int32>& FragmentIDs,
                                 const TMap<int32, FBox>& FragmentBounds, int32 CurrentDepth)
{
	if (!Node)
	{
		return;
	}

	// Leaf condition: depth limit, fragment count, or min size
	const bool bMaxDepthReached = CurrentDepth >= MaxDepth;
	const bool bFewFragments = FragmentIDs.Num() <= MaxFragmentsPerTile;
	const float NodeSize = Node->Bounds.GetSize().GetMax();
	const bool bMinSizeReached = NodeSize <= MinTileSize;

	if (bMaxDepthReached || bFewFragments || bMinSizeReached)
	{
		// Create leaf tile
		UFragmentTile* Tile = NewObject<UFragmentTile>(this);
		Tile->Initialize(Node->Bounds);
		Tile->FragmentLocalIDs = FragmentIDs;
		Node->Tile = Tile;
		Tiles.Add(Tile); // For memory management

		UE_LOG(LogFragmentOctree, Verbose, TEXT("Created tile %d: %d fragments, bounds=%s"),
		       Tiles.Num() - 1, FragmentIDs.Num(), *Node->Bounds.ToString());
		return;
	}

	// Subdivide into 8 octants
	const FVector Center = Node->Bounds.GetCenter();

	Node->Children.Reserve(8);

	for (int32 Octant = 0; Octant < 8; Octant++)
	{
		// Calculate octant bounds
		FVector ChildMin, ChildMax;

		if (Octant & 1) // X bit
		{
			ChildMin.X = Center.X;
			ChildMax.X = Node->Bounds.Max.X;
		}
		else
		{
			ChildMin.X = Node->Bounds.Min.X;
			ChildMax.X = Center.X;
		}

		if (Octant & 2) // Y bit
		{
			ChildMin.Y = Center.Y;
			ChildMax.Y = Node->Bounds.Max.Y;
		}
		else
		{
			ChildMin.Y = Node->Bounds.Min.Y;
			ChildMax.Y = Center.Y;
		}

		if (Octant & 4) // Z bit
		{
			ChildMin.Z = Center.Z;
			ChildMax.Z = Node->Bounds.Max.Z;
		}
		else
		{
			ChildMin.Z = Node->Bounds.Min.Z;
			ChildMax.Z = Center.Z;
		}

		const FBox ChildBounds(ChildMin, ChildMax);

		// Assign fragments to this octant based on bounding box CENTER
		TArray<int32> ChildFragments;
		for (int32 ID : FragmentIDs)
		{
			const FBox* FragBounds = FragmentBounds.Find(ID);
			if (!FragBounds)
			{
				continue;
			}

			// Use fragment bounds CENTER to determine octant
			FVector FragCenter = FragBounds->GetCenter();

			if (ChildBounds.IsInsideOrOn(FragCenter))
			{
				ChildFragments.Add(ID);
			}
		}

		// Skip empty octants
		if (ChildFragments.Num() == 0)
		{
			continue;
		}

		// Create child node
		FFragmentOctreeNode* Child = new FFragmentOctreeNode();
		Child->Bounds = ChildBounds;
		Child->Depth = CurrentDepth + 1;
		Node->Children.Add(Child);

		// Recurse
		BuildNode(Child, ChildFragments, FragmentBounds, CurrentDepth + 1);
	}
}

void UFragmentOctree::QueryVisibleTiles(const FConvexVolume& Frustum, TArray<UFragmentTile*>& OutTiles)
{
	OutTiles.Empty();

	if (!Root)
	{
		return;
	}

	QueryNodeFrustum(Root, Frustum, OutTiles);
}

void UFragmentOctree::QueryNodeFrustum(FFragmentOctreeNode* Node, const FConvexVolume& Frustum,
                                        TArray<UFragmentTile*>& OutTiles)
{
	if (!Node)
	{
		return;
	}

	// Test bounds against frustum
	if (!Frustum.IntersectBox(Node->Bounds.GetCenter(), Node->Bounds.GetExtent()))
	{
		return; // Early out - entire node outside frustum
	}

	// Leaf node - add tile
	if (Node->IsLeaf())
	{
		if (Node->Tile)
		{
			OutTiles.Add(Node->Tile);
		}
		return;
	}

	// Recurse to children
	for (FFragmentOctreeNode* Child : Node->Children)
	{
		QueryNodeFrustum(Child, Frustum, OutTiles);
	}
}

void UFragmentOctree::QueryTilesInRange(const FVector& Location, float Range, TArray<UFragmentTile*>& OutTiles)
{
	OutTiles.Empty();

	if (!Root)
	{
		return;
	}

	QueryNodeRange(Root, Location, Range, OutTiles);
}

void UFragmentOctree::QueryNodeRange(FFragmentOctreeNode* Node, const FVector& Location, float Range,
                                      TArray<UFragmentTile*>& OutTiles)
{
	if (!Node)
	{
		return;
	}

	// Test bounds against sphere
	const float DistSq = Node->Bounds.ComputeSquaredDistanceToPoint(Location);
	if (DistSq > Range * Range)
	{
		return; // Outside range
	}

	// Leaf node - add tile
	if (Node->IsLeaf())
	{
		if (Node->Tile)
		{
			OutTiles.Add(Node->Tile);
		}
		return;
	}

	// Recurse to children
	for (FFragmentOctreeNode* Child : Node->Children)
	{
		QueryNodeRange(Child, Location, Range, OutTiles);
	}
}

void UFragmentOctree::GetAllTiles(TArray<UFragmentTile*>& OutTiles)
{
	OutTiles.Empty();

	if (!Root)
	{
		return;
	}

	CollectAllTilesRecursive(Root, OutTiles);
}

void UFragmentOctree::CollectAllTilesRecursive(FFragmentOctreeNode* Node, TArray<UFragmentTile*>& OutTiles)
{
	if (!Node)
	{
		return;
	}

	// Leaf node - add tile
	if (Node->IsLeaf())
	{
		if (Node->Tile)
		{
			OutTiles.Add(Node->Tile);
		}
		return;
	}

	// Recurse to children
	for (FFragmentOctreeNode* Child : Node->Children)
	{
		CollectAllTilesRecursive(Child, OutTiles);
	}
}
