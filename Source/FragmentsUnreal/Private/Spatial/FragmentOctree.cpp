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

	ModelGuidRef = ModelGuid;

	// Extract fragment positions from model hierarchy
	TMap<int32, FVector> FragmentPositions;
	TArray<int32> AllFragmentIDs;

	const FFragmentItem& RootItem = ModelWrapper->GetModelItemRef();
	CollectFragmentPositions(RootItem, FragmentPositions, AllFragmentIDs);

	if (AllFragmentIDs.Num() == 0)
	{
		UE_LOG(LogFragmentOctree, Warning, TEXT("BuildFromModel: No fragments found"));
		return;
	}

	// Calculate world bounds
	FBox WorldBounds = CalculateBounds(AllFragmentIDs, FragmentPositions);

	if (!WorldBounds.IsValid)
	{
		UE_LOG(LogFragmentOctree, Error, TEXT("BuildFromModel: Invalid world bounds"));
		return;
	}

	// Create root node
	Root = new FFragmentOctreeNode();
	Root->Bounds = WorldBounds;
	Root->Depth = 0;

	const double StartTime = FPlatformTime::Seconds();

	// Build tree recursively
	BuildNode(Root, AllFragmentIDs, FragmentPositions, 0);

	const double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	UE_LOG(LogFragmentOctree, Log, TEXT("Octree built in %.2f ms: %d fragments, %d tiles"),
	       ElapsedTime * 1000.0, AllFragmentIDs.Num(), Tiles.Num());
}

void UFragmentOctree::CollectFragmentPositions(const FFragmentItem& Item,
                                                TMap<int32, FVector>& OutPositions,
                                                TArray<int32>& OutFragmentIDs)
{
	// Only store fragments with valid LocalId (not structural nodes)
	if (Item.LocalId >= 0)
	{
		OutPositions.Add(Item.LocalId, Item.GlobalTransform.GetLocation());
		OutFragmentIDs.Add(Item.LocalId);
	}

	// Recurse to children
	for (FFragmentItem* Child : Item.FragmentChildren)
	{
		if (Child)
		{
			CollectFragmentPositions(*Child, OutPositions, OutFragmentIDs);
		}
	}
}

FBox UFragmentOctree::CalculateBounds(const TArray<int32>& FragmentIDs,
                                       const TMap<int32, FVector>& FragmentPositions)
{
	FBox Bounds(ForceInit);

	for (int32 ID : FragmentIDs)
	{
		const FVector* Pos = FragmentPositions.Find(ID);
		if (Pos)
		{
			Bounds += *Pos;
		}
	}

	// Add 10% padding
	if (Bounds.IsValid)
	{
		const FVector Padding = Bounds.GetSize() * 0.1f;
		Bounds = Bounds.ExpandBy(Padding);
	}

	return Bounds;
}

void UFragmentOctree::BuildNode(FFragmentOctreeNode* Node, const TArray<int32>& FragmentIDs,
                                 const TMap<int32, FVector>& FragmentPositions, int32 CurrentDepth)
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

		UE_LOG(LogFragmentOctree, Verbose, TEXT("Leaf tile at depth %d with %d fragments"),
		       CurrentDepth, FragmentIDs.Num());
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

		// Assign fragments to this octant
		TArray<int32> ChildFragments;
		for (int32 ID : FragmentIDs)
		{
			const FVector* Pos = FragmentPositions.Find(ID);
			if (Pos && ChildBounds.IsInsideOrOn(*Pos))
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
		BuildNode(Child, ChildFragments, FragmentPositions, CurrentDepth + 1);
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
