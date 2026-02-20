


#include "EditorSubsystems/FragmentsImporterEditorSubsystem.h"
#include "Importer/FragmentsImporter.h"
#include "Utils/FragmentsLog.h"
#include "Editor/EditorEngine.h"

FString UFragmentsImporterEditorSubsystem::LoadFragment(const FString& FragPath)
{
    check(Importer);

    FString ModelGuid = Importer->LoadFragment(FragPath);
    FragmentModels = Importer->GetFragmentModels();

    return ModelGuid;
}

void UFragmentsImporterEditorSubsystem::UnloadFragment(const FString& ModelGuid)
{
    if (Importer)
        Importer->UnloadFragment(ModelGuid);

    if (UFragmentModelWrapper** Found = FragmentModels.Find(ModelGuid))
    {
        (*Found) = nullptr;
        FragmentModels.Remove(ModelGuid);
    }
}

FString UFragmentsImporterEditorSubsystem::ProcessFragment(AActor* OwnerActor, const FString& FragPath, TArray<class AFragment*>& OutFragments, bool bSaveMeshes,
    bool bUseDynamicMesh, bool bUseHISM, AFragment* BucketRoot)
{
    check(Importer);

    FString ModelGuid = Importer->Process(OwnerActor, FragPath, OutFragments, bSaveMeshes, bUseDynamicMesh, bUseHISM, BucketRoot);

    FragmentModels = Importer->GetFragmentModels();

    return ModelGuid;
}

void UFragmentsImporterEditorSubsystem::ProcessLoadedFragment(const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
    bool bUseDynamicMesh, bool bUseHISM, AFragment* BucketRoot)
{
    check(Importer);

    Importer->ProcessLoadedFragment(InModelGuid, InOwnerRef, bInSaveMesh, bUseDynamicMesh, bUseHISM, BucketRoot);
}

void UFragmentsImporterEditorSubsystem::ProcessLoadedFragmentItem(int64 InLocalId, const FString& InModelGuid, AActor* InOwnerRef, bool bInSaveMesh,
    bool bUseDynamicMesh, bool bUseHISM, AFragment* BucketRoot)
{
    check(Importer);

    Importer->ProcessLoadedFragmentItem(InLocalId, InModelGuid, InOwnerRef, bInSaveMesh, bUseDynamicMesh, bUseHISM, BucketRoot);
}

TArray<int64> UFragmentsImporterEditorSubsystem::GetElementsByCategory(const FString& InCategory, const FString& ModelGuid)
{
    check(Importer);

    return Importer->GetElementsByCategory(InCategory, ModelGuid);
}

AFragment* UFragmentsImporterEditorSubsystem::GetItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemByLocalId(InLocalId, InModelGuid);
}

FFragmentItem* UFragmentsImporterEditorSubsystem::GetFragmentItemByLocalId(int64 InLocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetFragmentItemByLocalId(InLocalId, InModelGuid);
}

void UFragmentsImporterEditorSubsystem::GetItemData(FFragmentItem* InFragmentItem)
{
    check(Importer);
    Importer->GetItemData(InFragmentItem);
}

TArray<FItemAttribute> UFragmentsImporterEditorSubsystem::GetItemPropertySets(int64 LocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemPropertySets(LocalId, InModelGuid);
}

TArray<FItemAttribute> UFragmentsImporterEditorSubsystem::GetItemAttributes(int64 LocalId, const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetItemAttributes(LocalId, InModelGuid);
}

AFragment* UFragmentsImporterEditorSubsystem::GetModelFragment(const FString& InModelGuid)
{
    check(Importer);
    return Importer->GetModelFragment(InModelGuid);
}

FTransform UFragmentsImporterEditorSubsystem::GetBaseCoordinates()
{
    check(Importer);
    return Importer->GetBaseCoordinates();
}

void UFragmentsImporterEditorSubsystem::ResetBaseCoordinates()
{
    check(Importer);
    Importer->ResetBaseCoordinates();
}

bool UFragmentsImporterEditorSubsystem::IsFragmentLoaded(const FString& InModelGuid)
{
    check(Importer);
    return Importer->IsFragmentLoaded(InModelGuid);
}

void UFragmentsImporterEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    Importer = NewObject<UFragmentsImporter>(this);
    FWorldDelegates::OnWorldCleanup.AddUObject(this, &UFragmentsImporterEditorSubsystem::OnWorldCleanup);
    FEditorDelegates::OnMapOpened.AddUObject(this, &UFragmentsImporterEditorSubsystem::OnMapOpened);
}

void UFragmentsImporterEditorSubsystem::OnWorldCleanup(UWorld* World, bool, bool)
{
    if (Importer) Importer->ReleaseRefToWorld(World);
}

void UFragmentsImporterEditorSubsystem::OnMapOpened(const FString&, bool)
{
    if (Importer) Importer->ResetAll();
    FragmentModels.Empty();
}

void UFragmentsImporterEditorSubsystem::Deinitialize()
{
    if (Importer)
    {
        Importer->ResetAll();
        Importer = nullptr;
    }
    FragmentModels.Empty();
    Super::Deinitialize();
}
