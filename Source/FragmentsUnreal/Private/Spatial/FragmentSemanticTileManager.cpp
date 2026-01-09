#include "Spatial/FragmentSemanticTileManager.h"
#include "Importer/FragmentsImporter.h"
#include "Importer/FragmentModelWrapper.h"
#include "Utils/FragmentsUtils.h"
#include "DrawDebugHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSemanticTiles, Log, All);

UFragmentSemanticTileManager::UFragmentSemanticTileManager()
{
}

void UFragmentSemanticTileManager::Initialize(const FString& InModelGuid, UFragmentsImporter* InImporter)
{
	if (!InImporter)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("Initialize: Null importer"));
		return;
	}

	ModelGuid = InModelGuid;
	Importer = InImporter;

	UE_LOG(LogSemanticTiles, Log, TEXT("Initialized Semantic Tile Manager for model: %s"), *ModelGuid);
}

void UFragmentSemanticTileManager::BuildSemanticTiles()
{
	if (!Importer)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("BuildSemanticTiles: No importer"));
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// Get model wrapper
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		UE_LOG(LogSemanticTiles, Error, TEXT("BuildSemanticTiles: Model wrapper not found for GUID %s"), *ModelGuid);
		return;
	}

	// Get root item
	const FFragmentItem& RootItem = Wrapper->GetModelItemRef();

	// Clear existing tiles
	SemanticTileMap.Empty();
	AllSemanticTiles.Empty();
	for (int32 i = 0; i < 4; i++)
	{
		TilesByPriority[i].Empty();
	}

	// Recursively collect all fragments and group by IFC class
	TArray<const FFragmentItem*> AllFragments;
	TFunction<void(const FFragmentItem&)> CollectFragments = [&](const FFragmentItem& Item)
	{
		AllFragments.Add(&Item);
		for (const FFragmentItem* Child : Item.FragmentChildren)
		{
			if (Child)
			{
				CollectFragments(*Child);
			}
		}
	};
	CollectFragments(RootItem);

	if (Config.bEnableDebugLogging)
	{
		UE_LOG(LogSemanticTiles, Log, TEXT("Collected %d total fragments"), AllFragments.Num());
	}

	// Group fragments by IFC class
	for (const FFragmentItem* Item : AllFragments)
	{
		FString IFCClass = ExtractIFCClass(Item->LocalId);

		// Get or create semantic tile for this IFC class
		USemanticTile** TilePtr = SemanticTileMap.Find(IFCClass);
		USemanticTile* Tile = nullptr;

		if (!TilePtr)
		{
			// Create new semantic tile
			Tile = NewObject<USemanticTile>(this);
			Tile->IFCClassName = IFCClass;
			Tile->Priority = DeterminePriority(IFCClass);
			Tile->RepresentativeColor = GetRepresentativeColor(IFCClass);
			Tile->CombinedBounds = FBox(ForceInit);

			SemanticTileMap.Add(IFCClass, Tile);
			AllSemanticTiles.Add(Tile);
			TilesByPriority[static_cast<int32>(Tile->Priority)].Add(Tile);

			if (Config.bEnableDebugLogging)
			{
				UE_LOG(LogSemanticTiles, Verbose, TEXT("Created semantic tile for IFC class: %s (Priority: %d)"),
				       *IFCClass, static_cast<int32>(Tile->Priority));
			}
		}
		else
		{
			Tile = *TilePtr;
		}

		// Add fragment to tile
		Tile->FragmentIDs.Add(Item->LocalId);
		Tile->Count++;
	}

	// Calculate combined bounds for each tile
	for (USemanticTile* Tile : AllSemanticTiles)
	{
		CalculateCombinedBounds(Tile);
	}

	const double ElapsedTime = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	UE_LOG(LogSemanticTiles, Log, TEXT("Built %d semantic tiles from %d fragments in %.2f ms"),
	       AllSemanticTiles.Num(), AllFragments.Num(), ElapsedTime);

	// Log summary by priority
	for (int32 i = 0; i < 4; i++)
	{
		int32 TileCount = TilesByPriority[i].Num();
		int32 FragmentCount = 0;
		for (USemanticTile* Tile : TilesByPriority[i])
		{
			FragmentCount += Tile->Count;
		}

		const TCHAR* PriorityNames[] = { TEXT("Structural"), TEXT("Openings"), TEXT("Furnishings"), TEXT("Details") };
		UE_LOG(LogSemanticTiles, Log, TEXT("  Priority %d (%s): %d tiles, %d fragments"),
		       i, PriorityNames[i], TileCount, FragmentCount);
	}
}

void UFragmentSemanticTileManager::Tick(float DeltaTime, const FVector& CameraLocation,
                                         const FRotator& CameraRotation, float FOV, float ViewportHeight)
{
	// Phase 1: Placeholder - no LOD logic yet
	// Phase 2+ will implement:
	// - Calculate screen coverage for each semantic tile
	// - Determine target LOD (Wireframe, SimpleBox, HighDetail)
	// - Trigger LOD generation/loading

	// Draw debug bounds if enabled
	if (Config.bDrawDebugBounds && Importer)
	{
		AActor* Owner = Importer->GetOwnerRef();
		if (Owner)
		{
			UWorld* World = Owner->GetWorld();
			if (World)
			{
				for (USemanticTile* Tile : AllSemanticTiles)
				{
					FColor Color = Tile->RepresentativeColor.ToFColor(true);
					DrawDebugBox(World, Tile->CombinedBounds.GetCenter(),
					            Tile->CombinedBounds.GetExtent(), Color, false, -1.0f, 0, 5.0f);
				}
			}
		}
	}
}

USemanticTile* UFragmentSemanticTileManager::GetSemanticTile(const FString& IFCClassName) const
{
	USemanticTile* const* TilePtr = SemanticTileMap.Find(IFCClassName);
	return TilePtr ? *TilePtr : nullptr;
}

TArray<USemanticTile*> UFragmentSemanticTileManager::GetTilesByPriority(EFragmentPriority Priority) const
{
	int32 Index = static_cast<int32>(Priority);
	if (Index >= 0 && Index < 4)
	{
		return TilesByPriority[Index];
	}
	return TArray<USemanticTile*>();
}

int32 UFragmentSemanticTileManager::GetTotalFragmentCount() const
{
	int32 TotalCount = 0;
	for (USemanticTile* Tile : AllSemanticTiles)
	{
		TotalCount += Tile->Count;
	}
	return TotalCount;
}

FString UFragmentSemanticTileManager::ExtractIFCClass(int32 LocalID)
{
	if (!Importer)
	{
		return TEXT("Unknown");
	}

	// Get fragment item
	FFragmentItem* Item = Importer->GetFragmentItemByLocalId(LocalID, ModelGuid);
	if (!Item)
	{
		return TEXT("Unknown");
	}

	// Extract IFC class from category string
	// Category format is typically "IfcWall", "IfcWindow", etc.
	FString Category = Item->Category;
	if (Category.IsEmpty())
	{
		return TEXT("Unknown");
	}

	// Return the category as the IFC class name
	return Category;
}

EFragmentPriority UFragmentSemanticTileManager::DeterminePriority(const FString& IFCClassName)
{
	// Structural elements (highest priority - load first)
	if (IFCClassName.Contains(TEXT("Wall")) ||
	    IFCClassName.Contains(TEXT("Floor")) ||
	    IFCClassName.Contains(TEXT("Roof")) ||
	    IFCClassName.Contains(TEXT("Slab")) ||
	    IFCClassName.Contains(TEXT("Beam")) ||
	    IFCClassName.Contains(TEXT("Column")))
	{
		return EFragmentPriority::STRUCTURAL;
	}

	// Openings (second priority)
	if (IFCClassName.Contains(TEXT("Window")) ||
	    IFCClassName.Contains(TEXT("Door")) ||
	    IFCClassName.Contains(TEXT("Opening")) ||
	    IFCClassName.Contains(TEXT("CurtainWall")))
	{
		return EFragmentPriority::OPENINGS;
	}

	// Furnishings (third priority)
	if (IFCClassName.Contains(TEXT("Furniture")) ||
	    IFCClassName.Contains(TEXT("Fixture")) ||
	    IFCClassName.Contains(TEXT("Equipment")) ||
	    IFCClassName.Contains(TEXT("FurnishingElement")))
	{
		return EFragmentPriority::FURNISHINGS;
	}

	// Details (lowest priority - load last)
	return EFragmentPriority::DETAILS;
}

FLinearColor UFragmentSemanticTileManager::GetRepresentativeColor(const FString& IFCClassName)
{
	// Assign colors based on IFC class for visual distinction in wireframe LOD

	// Structural - gray tones
	if (IFCClassName.Contains(TEXT("Wall"))) return FLinearColor(0.7f, 0.7f, 0.7f);
	if (IFCClassName.Contains(TEXT("Floor"))) return FLinearColor(0.5f, 0.5f, 0.5f);
	if (IFCClassName.Contains(TEXT("Roof"))) return FLinearColor(0.6f, 0.4f, 0.4f);
	if (IFCClassName.Contains(TEXT("Slab"))) return FLinearColor(0.5f, 0.5f, 0.5f);
	if (IFCClassName.Contains(TEXT("Beam"))) return FLinearColor(0.8f, 0.5f, 0.3f);
	if (IFCClassName.Contains(TEXT("Column"))) return FLinearColor(0.8f, 0.5f, 0.3f);

	// Openings - blue tones
	if (IFCClassName.Contains(TEXT("Window"))) return FLinearColor(0.3f, 0.6f, 0.9f);
	if (IFCClassName.Contains(TEXT("Door"))) return FLinearColor(0.5f, 0.4f, 0.7f);
	if (IFCClassName.Contains(TEXT("CurtainWall"))) return FLinearColor(0.4f, 0.7f, 1.0f);

	// Furnishings - green/yellow tones
	if (IFCClassName.Contains(TEXT("Furniture"))) return FLinearColor(0.5f, 0.7f, 0.3f);
	if (IFCClassName.Contains(TEXT("Fixture"))) return FLinearColor(0.7f, 0.7f, 0.4f);
	if (IFCClassName.Contains(TEXT("Equipment"))) return FLinearColor(0.6f, 0.6f, 0.3f);

	// Details - light gray
	if (IFCClassName.Contains(TEXT("Railing"))) return FLinearColor(0.6f, 0.6f, 0.6f);
	if (IFCClassName.Contains(TEXT("Fastener"))) return FLinearColor(0.4f, 0.4f, 0.4f);

	// Default - gray
	return FLinearColor::Gray;
}

void UFragmentSemanticTileManager::CalculateCombinedBounds(USemanticTile* Tile)
{
	if (!Tile || !Importer)
	{
		return;
	}

	Tile->CombinedBounds = FBox(ForceInit);

	// Get model wrapper for bounds access
	UFragmentModelWrapper* Wrapper = Importer->GetFragmentModel(ModelGuid);
	if (!Wrapper)
	{
		return;
	}

	const Model* ParsedModel = Wrapper->GetParsedModel();
	if (!ParsedModel || !ParsedModel->meshes())
	{
		return;
	}

	const Meshes* MeshesRef = ParsedModel->meshes();

	// Accumulate bounds from all fragments in this tile
	for (int32 LocalID : Tile->FragmentIDs)
	{
		FFragmentItem* Item = Importer->GetFragmentItemByLocalId(LocalID, ModelGuid);
		if (Item && Item->Samples.Num() > 0)
		{
			FBox FragmentBounds(ForceInit);
			bool bHasValidBounds = false;

			// Calculate bounds from all samples (representations)
			for (const FFragmentSample& Sample : Item->Samples)
			{
				if (Sample.RepresentationIndex < 0 ||
				    Sample.RepresentationIndex >= static_cast<int32>(MeshesRef->representations()->size()))
				{
					continue;
				}

				const Representation* Rep = MeshesRef->representations()->Get(Sample.RepresentationIndex);
				if (!Rep)
				{
					continue;
				}

				// Get bounding box from representation (returns reference, not pointer)
				const BoundingBox& bbox = Rep->bbox();

				// Get min/max vectors (returns references, not pointers)
				const FloatVector& minVec = bbox.min();
				const FloatVector& maxVec = bbox.max();

				// Convert to Unreal coordinates (cm)
				// Note: Coordinate transform (x,y,z) → (x*100, z*100, y*100)
				FVector Min(
					minVec.x() * 100.0f,
					minVec.z() * 100.0f,
					minVec.y() * 100.0f
				);
				FVector Max(
					maxVec.x() * 100.0f,
					maxVec.z() * 100.0f,
					maxVec.y() * 100.0f
				);

				FBox RepBounds(Min, Max);

				// Transform by global transform
				FBox TransformedBounds = RepBounds.TransformBy(Item->GlobalTransform);

				if (bHasValidBounds)
				{
					FragmentBounds += TransformedBounds;
				}
				else
				{
					FragmentBounds = TransformedBounds;
					bHasValidBounds = true;
				}
			}

			// Add fragment bounds to tile combined bounds
			if (bHasValidBounds)
			{
				Tile->CombinedBounds += FragmentBounds;
			}
			else
			{
				// Fallback to position if no valid bounds
				FVector Position = Item->GlobalTransform.GetLocation();
				FBox PointBounds(Position, Position);
				Tile->CombinedBounds += PointBounds.ExpandBy(50.0f);
			}
		}
	}

	if (Config.bEnableDebugLogging)
	{
		FVector Size = Tile->CombinedBounds.GetSize();
		UE_LOG(LogSemanticTiles, Verbose, TEXT("  %s: Bounds size = (%.2f, %.2f, %.2f)"),
		       *Tile->IFCClassName, Size.X, Size.Y, Size.Z);
	}
}
