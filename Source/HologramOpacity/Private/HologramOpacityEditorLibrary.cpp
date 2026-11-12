#include "HologramOpacityEditorLibrary.h"
#include "HologramOpacityModule.h"

#if WITH_EDITOR
#include "Engine/AssetManagerTypes.h"
#include "FGGameUserSettings.h"
#include "GameFeatureData.h"
#include "HologramOpacitySubsystem.h"
#include "Settings/FGUserSetting.h"
#include "Settings/FGUserSettingApplyType.h"
#include "Settings/FGUserSettingCategory.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HologramOpacity"

namespace HologramOpacityEditor
{
	/** Fills the parts every one of this mod's Options menu settings shares; the caller sets up the value selector. */
	static bool SetupCommon(UFGUserSetting* Setting, const TCHAR* StrId, const FText& DisplayName, const FText& ToolTip,
		TSubclassOf<UFGUserSettingCategory> Category, TSubclassOf<UFGUserSettingCategory> SubCategory, float MenuPriority)
	{
		if (!Setting || !Category || !SubCategory)
		{
			UE_LOG(LogHologramOpacity, Error, TEXT("Setup %s: missing setting or category classes"), StrId);
			return false;
		}
		Setting->Modify();

		Setting->StrId = StrId;
		Setting->UseCVar = false;
		Setting->DisplayName = DisplayName;
		Setting->ToolTip = ToolTip;

		Setting->WidgetsToCreate.Reset();
		Setting->WidgetsToCreate.Add(FSettingsWidgetLocationDescriptor(Category, SubCategory, nullptr, MenuPriority));

		Setting->IsSettingSessionWide = false;
		Setting->ApplyType = TSoftClassPtr<UFGUserSettingApplyType>(UFGUserSettingApplyType_UpdateInstantly::StaticClass());
		Setting->ManagerTypeAvailability = UFGGameUserSettings::StaticClass();
		Setting->VisibilityDisqualifiers = 0;
		Setting->EditabilityDisqualifiers = 0;

		// ShowInBuilds is protected and defaults to development-only; the options menu skips it in shipping otherwise.
		FProperty* ShowInBuilds = UFGUserSetting::StaticClass()->FindPropertyByName(TEXT("ShowInBuilds"));
		if (!ShowInBuilds)
		{
			UE_LOG(LogHologramOpacity, Error, TEXT("Setup %s: ShowInBuilds property not found"), StrId);
			return false;
		}
		ShowInBuilds->ImportText_InContainer(TEXT("IIB_PublicBuilds"), Setting, Setting, PPF_None);
		return true;
	}

	/** Returns the setting's value selector of the wanted class, replacing any other selector. */
	template <typename TSelector>
	static TSelector* GetOrCreateSelector(UFGUserSetting* Setting)
	{
		TSelector* Selector = Cast<TSelector>(Setting->ValueSelector);
		if (!Selector)
		{
			Selector = NewObject<TSelector>(Setting, TSelector::StaticClass(), NAME_None, RF_Public | RF_Transactional);
			Setting->ValueSelector = Selector;
		}
		Selector->Modify();
		return Selector;
	}
}

bool UHologramOpacityEditorLibrary::SetupOpacitySetting(UFGUserSetting* Setting, TSubclassOf<UFGUserSettingCategory> Category,
	TSubclassOf<UFGUserSettingCategory> SubCategory, float MenuPriority)
{
	const bool bOk = HologramOpacityEditor::SetupCommon(Setting, UHologramOpacitySubsystem::SettingId,
		LOCTEXT("HologramOpacity_Name", "Hologram Opacity"),
		LOCTEXT("HologramOpacity_Tooltip",
			"How solid build mode holograms look. Lower it to see through holograms while placing buildings. 100% is the normal hologram."),
		Category, SubCategory, MenuPriority);
	if (!bOk)
	{
		return false;
	}

	UFGUserSetting_Slider* Slider = HologramOpacityEditor::GetOrCreateSelector<UFGUserSetting_Slider>(Setting);
	Slider->MinValue = 0.f;
	Slider->MaxValue = 1.f;
	Slider->MinDisplayValue = 0.f;
	Slider->MaxDisplayValue = 100.f;
	Slider->MaxFractionalDigits = 0;
	Slider->ShowZeroAsOff = false;
	Slider->DefaultSliderValue = 100.f;

	Setting->MarkPackageDirty();
	return true;
}

bool UHologramOpacityEditorLibrary::SetupScanlinesSetting(UFGUserSetting* Setting, TSubclassOf<UFGUserSettingCategory> Category,
	TSubclassOf<UFGUserSettingCategory> SubCategory, float MenuPriority)
{
	const bool bOk = HologramOpacityEditor::SetupCommon(Setting, UHologramOpacitySubsystem::ScanlinesSettingId,
		LOCTEXT("HologramScanlines_Name", "Show Scanlines"),
		LOCTEXT("HologramScanlines_Tooltip",
			"Show the scan lines across build mode holograms. Uncheck for clean holograms without lines, at any opacity."),
		Category, SubCategory, MenuPriority);
	if (!bOk)
	{
		return false;
	}

	UFGUserSetting_CheckBox* CheckBox = HologramOpacityEditor::GetOrCreateSelector<UFGUserSetting_CheckBox>(Setting);
	CheckBox->DefaultCheckBoxValue = true;
	CheckBox->PlatformDefaultCheckBoxValues.Reset();

	Setting->MarkPackageDirty();
	return true;
}

bool UHologramOpacityEditorLibrary::AddPrimaryAssetTypeToScan(UGameFeatureData* GameFeatureData, const FString& PrimaryAssetTypeInfoText)
{
	if (!GameFeatureData)
	{
		return false;
	}
	FPrimaryAssetTypeInfo Info;
	FPrimaryAssetTypeInfo::StaticStruct()->ImportText(*PrimaryAssetTypeInfoText, &Info, nullptr, PPF_None, GWarn, TEXT("PrimaryAssetTypeInfo"));
	if (!Info.PrimaryAssetType.IsValid())
	{
		UE_LOG(LogHologramOpacity, Error, TEXT("AddPrimaryAssetTypeToScan: could not parse '%s'"), *PrimaryAssetTypeInfoText);
		return false;
	}

	TArray<FPrimaryAssetTypeInfo>& Rules = GameFeatureData->GetPrimaryAssetTypesToScan();
	if (Rules.ContainsByPredicate([&Info](const FPrimaryAssetTypeInfo& Existing) { return Existing.PrimaryAssetType == Info.PrimaryAssetType; }))
	{
		return true;
	}
	GameFeatureData->Modify();
	Rules.Add(Info);
	GameFeatureData->MarkPackageDirty();
	return true;
}

#undef LOCTEXT_NAMESPACE
#endif
