


#include "Importer/FragmentsComponent.h"
#include "Importer/FragmentsImporter.h"
#include "Interfaces/IPluginManager.h"


// Sets default values for this component's properties
UFragmentsComponent::UFragmentsComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;

	// ...
}


// Called when the game starts
void UFragmentsComponent::BeginPlay()
{
	Super::BeginPlay();
	FragmentsImporter = NewObject<UFragmentsImporter>(this);

	// ...
	
}


// Called every frame
void UFragmentsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// ...
}

FString UFragmentsComponent::TestImportFragmentFile(const FString& Path, TArray<AFragment*>& OutFragments, bool bSaveMeshes)
{
	if (FragmentsImporter)
	{
#if PLATFORM_ANDROID
		const FString DownloadDir = TEXT("/storage/emulated/0/Download");
		const FString AndroidFilePath = FPaths::Combine(DownloadDir, TEXT("small_test.frag"));

		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*AndroidFilePath))
		{
			FString FileContents;
			if (FFileHelper::LoadFileToString(FileContents, *AndroidFilePath))
			{
				UE_LOG(LogFragments, Log, TEXT("Loaded %s from Download"), *AndroidFilePath);
				return FragmentsImporter->Process(GetOwner(), AndroidFilePath, OutFragments, bSaveMeshes);
			}
			else
			{
				UE_LOG(LogFragments, Error, TEXT("Failed to read %s"), *AndroidFilePath);
				// fall through to plugin‐Content fallback
			}
		}
		else
		{
			UE_LOG(LogFragments, Warning, TEXT("File not found in Download: %s"), *AndroidFilePath);
			// fall through to plugin‐Content fallback
		}

#elif PLATFORM_WINDOWS
		// Find our plugin by name
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FragmentsUnreal"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogFragments, Error, TEXT("Could not find FragmentsUnreal plugin!"));
			return FString();
		}

		// GetContentDir() will point to
		// <Project>/Plugins/FragmentsUnreal/Content  (or Engine/Plugins/..., as appropriate)
		const FString PluginContentDir = Plugin->GetContentDir();

		// Build the full path to our file
		const FString FilePath = FPaths::Combine(PluginContentDir, TEXT("Resources/small_test.frag"));
		
		return FragmentsImporter->Process(GetOwner(), FilePath, OutFragments, bSaveMeshes);

#endif
	}
	return FString();
}

FString UFragmentsComponent::ProcessFragment(const FString& Path, TArray<AFragment*>& OutFragments, bool bSaveMeshes)
{
	if (FragmentsImporter)
		return FragmentsImporter->Process(GetOwner(), Path, OutFragments, bSaveMeshes,
			/*bUseDynamicMesh=*/false, /*bUseHISM=*/false, /*BucketRoot=*/nullptr);

	return FString();
}

TArray<class AFragment*> UFragmentsComponent::GetFragmentActors()
{
	if (FragmentsImporter)
		return FragmentsImporter->FragmentActors;

	return TArray<class AFragment*>();
}

TArray<FItemAttribute> UFragmentsComponent::GetItemPropertySets(AFragment* InFragment)
{
	if (FragmentsImporter)
		return FragmentsImporter->GetItemPropertySets(InFragment);
	return TArray<FItemAttribute>();
}

AFragment* UFragmentsComponent::GetItemByLocalId(int64 LocalId, const FString& ModelGuid)
{
	if (FragmentsImporter) return FragmentsImporter->GetItemByLocalId(LocalId, ModelGuid);
	return nullptr;
}

void UFragmentsComponent::ProcessFragmentAsync(const FString& Path, FOnFragmentLoadComplete OnComplete)
{
	if (!FragmentsImporter)
	{
		UE_LOG(LogFragments, Error, TEXT("ProcessFragmentAsync: No FragmentsImporter"));
		OnComplete.ExecuteIfBound(false, TEXT("No importer"), TEXT(""));
		return;
	}

	FragmentsImporter->ProcessFragmentAsync(Path, GetOwner(), OnComplete);
}

void UFragmentsComponent::StartTileStreaming()
{
	if (!FragmentsImporter)
	{
		UE_LOG(LogFragments, Warning, TEXT("StartTileStreaming: No FragmentsImporter"));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogFragments, Warning, TEXT("StartTileStreaming: No World"));
		return;
	}

	// Start camera update timer (5 times per second)
	World->GetTimerManager().SetTimer(
		CameraUpdateTimerHandle,
		this,
		&UFragmentsComponent::UpdateCameraStreaming,
		0.2f, // Every 200ms
		true  // Loop
	);

	UE_LOG(LogFragments, Log, TEXT("Tile streaming started"));
}

void UFragmentsComponent::StopTileStreaming()
{
	UWorld* World = GetWorld();
	if (World && World->GetTimerManager().IsTimerActive(CameraUpdateTimerHandle))
	{
		World->GetTimerManager().ClearTimer(CameraUpdateTimerHandle);
		UE_LOG(LogFragments, Log, TEXT("Tile streaming stopped"));
	}
}

void UFragmentsComponent::UpdateCameraStreaming()
{
	if (!FragmentsImporter)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Get first player controller
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}

	// Get camera location and rotation
	FVector CameraLocation;
	FRotator CameraRotation;
	PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

	// Get FOV (default to 90 if no camera manager)
	float FOV = 90.0f;
	if (PC->PlayerCameraManager)
	{
		FOV = PC->PlayerCameraManager->GetFOVAngle();
	}

	// Get viewport dimensions (needed for SSE calculation)
	float ViewportHeight = 1080.0f; // Default
	float AspectRatio = 16.0f / 9.0f;
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize;
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		ViewportHeight = ViewportSize.Y;
		if (ViewportSize.Y > 0)
		{
			AspectRatio = ViewportSize.X / ViewportSize.Y;
		}
	}

	// Update tile streaming in importer (pass viewport height for SSE calculation)
	FragmentsImporter->UpdateTileStreaming(CameraLocation, CameraRotation, FOV, AspectRatio, ViewportHeight);
}

void UFragmentsComponent::SetShowDebugTileBounds(bool bShow)
{
	if (FragmentsImporter)
	{
		FragmentsImporter->bShowDebugTileBounds = bShow;
		UE_LOG(LogFragments, Log, TEXT("Debug tile bounds: %s"), bShow ? TEXT("Enabled") : TEXT("Disabled"));
	}
}

bool UFragmentsComponent::GetShowDebugTileBounds() const
{
	if (FragmentsImporter)
	{
		return FragmentsImporter->bShowDebugTileBounds;
	}
	return false;
}


