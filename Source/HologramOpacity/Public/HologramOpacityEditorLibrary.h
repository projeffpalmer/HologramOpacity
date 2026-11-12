#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Settings/FGUserSettingCategory.h"
#include "Templates/SubclassOf.h"
#include "HologramOpacityEditorLibrary.generated.h"

class UFGUserSetting;
class UGameFeatureData;

/**
 * Editor-only helpers used by Scripts/create_assets.py to author this mod's content.
 * (Python can't set EditDefaultsOnly properties on instanced sub-objects such as the slider value selector.)
 */
UCLASS()
class HOLOGRAMOPACITY_API UHologramOpacityEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	/** Fills in the Hologram Opacity slider setting (Options > User Interface > Holograms). */
	UFUNCTION(BlueprintCallable, Category = "Hologram Opacity|Editor")
	static bool SetupOpacitySetting(UFGUserSetting* Setting, TSubclassOf<UFGUserSettingCategory> Category,
		TSubclassOf<UFGUserSettingCategory> SubCategory, float MenuPriority);

	/** Fills in the Show Scanlines checkbox setting (placed right under the opacity slider). */
	UFUNCTION(BlueprintCallable, Category = "Hologram Opacity|Editor")
	static bool SetupScanlinesSetting(UFGUserSetting* Setting, TSubclassOf<UFGUserSettingCategory> Category,
		TSubclassOf<UFGUserSettingCategory> SubCategory, float MenuPriority);

	/** Adds an asset manager scan rule (FPrimaryAssetTypeInfo in text form) to a game feature data asset, if missing. */
	UFUNCTION(BlueprintCallable, Category = "Hologram Opacity|Editor")
	static bool AddPrimaryAssetTypeToScan(UGameFeatureData* GameFeatureData, const FString& PrimaryAssetTypeInfoText);
#endif
};
