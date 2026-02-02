

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Utils/FragmentsUtils.h"
#include "FragmentsComponent.generated.h"


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class FRAGMENTSUNREAL_API UFragmentsComponent : public UActorComponent
{
	GENERATED_BODY()

public:	
	// Sets default values for this component's properties
	UFragmentsComponent();

protected:
	// Called when the game starts
	virtual void BeginPlay() override;

	UPROPERTY()
	class UFragmentsImporter* FragmentsImporter = nullptr;

public:	
	// Called every frame
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	FString TestImportFragmentFile(const FString& Path, TArray<AFragment*>& OutFragments, bool bSaveMeshes);
	
	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	FString ProcessFragment(const FString& Path, TArray<AFragment*>& OutFragments, bool bSaveMeshes);

	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	TArray<class AFragment*> GetFragmentActors();
		
	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	TArray<FItemAttribute> GetItemPropertySets(AFragment* InFragment);

	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	AFragment* GetItemByLocalId(int32 LocalId, const FString& ModelGuid);

	/**
	 * Load fragment asynchronously with automatic tile streaming
	 * @param Path Path to .frag file
	 * @param OnComplete Callback when loading finishes
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Importer")
	void ProcessFragmentAsync(const FString& Path, FOnFragmentLoadComplete OnComplete);

	/**
	 * Start tile-based streaming (call after loading fragment)
	 * Updates visible tiles based on camera frustum
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	void StartTileStreaming();

	/**
	 * Stop tile-based streaming
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Streaming")
	void StopTileStreaming();

	/**
	 * Toggle debug LOD color overlay on fragments.
	 * Red = BoundingBox LOD, Yellow = Simplified LOD, Green = FullDetail LOD.
	 * @param bShow True to show LOD color overlay, false to restore original materials
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Debug")
	void SetShowDebugLodColors(bool bShow);

	/**
	 * Get current LOD debug visualization state
	 */
	UFUNCTION(BlueprintCallable, Category = "Fragments|Debug")
	bool GetShowDebugLodColors() const;

private:
	// Timer handle for camera update
	FTimerHandle CameraUpdateTimerHandle;

	// Update camera streaming (called by timer)
	void UpdateCameraStreaming();
};

