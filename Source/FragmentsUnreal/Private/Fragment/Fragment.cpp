


#include "Fragment/Fragment.h"


AFragment::AFragment()
{
	PrimaryActorTick.bCanEverTick = false;
}

AFragment* AFragment::FindFragmentByLocalId(int32 InLocalId)
{
	if (InLocalId == LocalId) return this;

	AFragment* FoundFragment = nullptr;
	for (AFragment*& F : FragmentChildren)
	{
		if (F->GetLocalId() == InLocalId) return F;

		FoundFragment = F->FindFragmentByLocalId(InLocalId);
	}
	return FoundFragment;
}

void AFragment::SetData(FFragmentItem InFragmentItem)
{
	Data = InFragmentItem;
	ModelGuid = InFragmentItem.ModelGuid;
	Guid = InFragmentItem.Guid;
	GlobalTransform = InFragmentItem.GlobalTransform;
	SetActorTransform(GlobalTransform);
	LocalId = InFragmentItem.LocalId;
	Category = InFragmentItem.Category;
	Samples = InFragmentItem.Samples;
}


