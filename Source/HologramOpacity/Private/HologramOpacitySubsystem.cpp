#include "HologramOpacitySubsystem.h"
#include "HologramOpacityModule.h"

#include "Components/MeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Variant.h"
#include "UObject/UObjectIterator.h"

#include "FGGameUserSettings.h"
#include "Hologram/FGHologram.h"
#include "Settings/FGUserSettingApplyType.h"

const TCHAR* UHologramOpacitySubsystem::SettingId = TEXT("HologramOpacity.Opacity");
const TCHAR* UHologramOpacitySubsystem::ScanlinesSettingId = TEXT("HologramOpacity.Scanlines");

namespace HologramOpacity
{
	// The global hologram post process (a blendable on the main map's GlobalPostProcess volume)
	static const TCHAR* PostProcessPath = TEXT("/Game/FactoryGame/PostProcess/Materials/PPI_OutlineHologram.PPI_OutlineHologram");
	// Translucent hologram body material (this mod's content)
	static const TCHAR* BodyMaterialPath = TEXT("/HologramOpacity/Materials/M_HologramOpacity.M_HologramOpacity");
	// Materials of the build gun's helper meshes (guidelines, clearance boxes, arrows, connection markers)
	static const TCHAR* BuildGunMaterialFolder = TEXT("/Game/FactoryGame/Equipment/BuildGun/");
	// Custom depth stencil values the game writes on hologram bodies (PPI_OutlineHologram bitmask values)
	static constexpr int32 StencilSoftClearance = 3;
	static constexpr int32 StencilValid = 5;
	static constexpr int32 StencilInvalid = 6;

	static const TCHAR* BodyKeys[3] = {TEXT("custom_ok"), TEXT("custom_warn"), TEXT("custom_error")};

	static TWeakObjectPtr<UHologramOpacitySubsystem> GActive;

	static FLinearColor ToLinearColor(const FVector& V)
	{
		return FLinearColor(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z), 1.f);
	}

	static FString DescribeMaterial(const UMaterialInterface* Material)
	{
		if (!Material)
		{
			return TEXT("None");
		}
		const UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Material);
		return Dynamic && Dynamic->Parent
			? FString::Printf(TEXT("%s(MID of %s)"), *Material->GetName(), *Dynamic->Parent->GetPathName())
			: Material->GetPathName();
	}

	static int32 StateIndex(uint8 HologramMaterialState)
	{
		return HologramMaterialState == static_cast<uint8>(EHologramMaterialState::HMS_ERROR) ? 2
			: HologramMaterialState == static_cast<uint8>(EHologramMaterialState::HMS_WARNING) ? 1
			: 0;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Console commands (debug / tuning). Usable from the in-game console.
// ---------------------------------------------------------------------------------------------------------------------

static FAutoConsoleCommand GHoloOpacitySetCmd(
	TEXT("HoloOpacity.Set"),
	TEXT("HoloOpacity.Set <0..1 | -1>  Override the hologram opacity for testing. -1 goes back to the Options value."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 0)
		{
			Subsystem->SetDebugOpacityOverride(FCString::Atof(*Args[0]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityScanlinesCmd(
	TEXT("HoloOpacity.Scanlines"),
	TEXT("HoloOpacity.Scanlines <1 | 0 | -1>  Override Show Scanlines for testing (1 on, 0 off). -1 goes back to the Options value."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 0)
		{
			Subsystem->SetDebugScanlinesOverride(FCString::Atoi(*Args[0]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityModeCmd(
	TEXT("HoloOpacity.Mode"),
	TEXT("HoloOpacity.Mode <0..3>  0 vanilla meshes, 1 translucent body under the post process, 2 translucent body (default), 3 ghost (post process only)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 0)
		{
			Subsystem->SetMode(FCString::Atoi(*Args[0]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityStencilCmd(
	TEXT("HoloOpacity.Stencil"),
	TEXT("HoloOpacity.Stencil <ok> <warn> <error>  Debug: force custom depth stencil values on hologram bodies (-1 leaves the game's)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 2)
		{
			Subsystem->SetStencilOverrides(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityRuleCmd(
	TEXT("HoloOpacity.Rule"),
	TEXT("HoloOpacity.Rule <key> <parameter> <valueAtZero>  Make a scalar parameter follow the slider. Keys: pp, custom_ok, custom_warn, custom_error."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 2)
		{
			Subsystem->SetScalarRule(Args[0], FName(*Args[1]), FCString::Atof(*Args[2]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityParamCmd(
	TEXT("HoloOpacity.Param"),
	TEXT("HoloOpacity.Param <key|custom> <parameter> <value>  Set a scalar parameter on the mod's dynamic materials (custom = all three body colours)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 2)
		{
			Subsystem->SetScalarDirect(Args[0], FName(*Args[1]), FCString::Atof(*Args[2]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityDumpCmd(
	TEXT("HoloOpacity.Dump"),
	TEXT("HoloOpacity.Dump  Log the setting, the mod's materials, live holograms and post process blendables."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive())
		{
			Subsystem->DumpState();
		}
		else
		{
			UE_LOG(LogHologramOpacity, Warning, TEXT("No active HologramOpacity subsystem"));
		}
	}));

// ---------------------------------------------------------------------------------------------------------------------
// Subsystem lifetime
// ---------------------------------------------------------------------------------------------------------------------

bool UHologramOpacitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Purely visual: nothing to do on a dedicated server
	return !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UHologramOpacitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	HologramOpacity::GActive = this;
	bInitialized = true;
	bForceRefresh = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Hologram Opacity subsystem initialized"));
}

void UHologramOpacitySubsystem::Deinitialize()
{
	RestoreAllMeshes();
	bInitialized = false;
	if (HologramOpacity::GActive.Get() == this)
	{
		HologramOpacity::GActive.Reset();
	}
	Super::Deinitialize();
}

UHologramOpacitySubsystem* UHologramOpacitySubsystem::GetActive()
{
	return HologramOpacity::GActive.Get();
}

TStatId UHologramOpacitySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UHologramOpacitySubsystem, STATGROUP_Tickables);
}

ETickableTickType UHologramOpacitySubsystem::GetTickableTickType() const
{
	return HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) ? ETickableTickType::Never : ETickableTickType::Conditional;
}

bool UHologramOpacitySubsystem::IsTickable() const
{
	return bInitialized;
}

UWorld* UHologramOpacitySubsystem::GetTickableGameObjectWorld() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetWorld() : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Setting
// ---------------------------------------------------------------------------------------------------------------------

float UHologramOpacitySubsystem::ReadOpacitySetting() const
{
	if (DebugOpacityOverride >= 0.f)
	{
		return FMath::Clamp(DebugOpacityOverride, 0.f, 1.f);
	}

	UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	if (!GameUserSettings || !GameUserSettings->HasInitialized())
	{
		return 1.f;
	}
	const UFGUserSettingApplyType* Setting = GameUserSettings->FindUserSetting(SettingId);
	if (!Setting)
	{
		return 1.f;
	}

	const FVariant Value = Setting->GetDisplayValue();
	float Opacity = 1.f;
	switch (Value.GetType())
	{
	case EVariantTypes::Float:
		Opacity = Value.GetValue<float>();
		break;
	case EVariantTypes::Double:
		Opacity = static_cast<float>(Value.GetValue<double>());
		break;
	case EVariantTypes::Int32:
		Opacity = static_cast<float>(Value.GetValue<int32>());
		break;
	default:
		return 1.f;
	}
	// The slider stores 0..1. Be tolerant of a percentage in case the value selector stores display units.
	if (Opacity > 1.001f)
	{
		Opacity /= 100.f;
	}
	return FMath::Clamp(Opacity, 0.f, 1.f);
}

bool UHologramOpacitySubsystem::ReadScanlinesSetting() const
{
	if (DebugScanlinesOverride >= 0)
	{
		return DebugScanlinesOverride != 0;
	}

	UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	if (!GameUserSettings || !GameUserSettings->HasInitialized())
	{
		return true;
	}
	const UFGUserSettingApplyType* Setting = GameUserSettings->FindUserSetting(ScanlinesSettingId);
	if (!Setting)
	{
		return true;
	}

	// Checkboxes are bools, but saved values come back from the ini as integers
	const FVariant Value = Setting->GetDisplayValue();
	switch (Value.GetType())
	{
	case EVariantTypes::Bool:
		return Value.GetValue<bool>();
	case EVariantTypes::Int32:
		return Value.GetValue<int32>() != 0;
	case EVariantTypes::Float:
		return Value.GetValue<float>() > 0.5f;
	default:
		return true;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------------------------------------------------

FHologramOpacityMaterial* UHologramOpacitySubsystem::FindEntry(const FString& Key)
{
	return Materials.FindByPredicate([&Key](const FHologramOpacityMaterial& Entry) { return Entry.Key.Equals(Key, ESearchCase::IgnoreCase); });
}

const FHologramOpacityMaterial* UHologramOpacitySubsystem::FindEntry(const FString& Key) const
{
	return Materials.FindByPredicate([&Key](const FHologramOpacityMaterial& Entry) { return Entry.Key.Equals(Key, ESearchCase::IgnoreCase); });
}

FHologramOpacityMaterial& UHologramOpacitySubsystem::AddEntry(const FString& Key, UMaterialInterface* Original, UMaterialInterface* Parent)
{
	FHologramOpacityMaterial& Entry = Materials.AddDefaulted_GetRef();
	Entry.Key = Key;
	Entry.Original = Original;
	Entry.Dynamic = UMaterialInstanceDynamic::Create(Parent, this, FName(*FString::Printf(TEXT("MID_HologramOpacity_%s"), *Key)));
	return Entry;
}

void UHologramOpacitySubsystem::AddRule(FHologramOpacityMaterial& Entry, FName Parameter, float ValueAtZero)
{
	if (!Entry.Dynamic)
	{
		return;
	}
	if (FHologramOpacityScalarRule* Existing = Entry.Rules.FindByPredicate([Parameter](const FHologramOpacityScalarRule& Rule) { return Rule.Parameter == Parameter; }))
	{
		Existing->ValueAtZero = ValueAtZero;
		return;
	}

	float ValueAtOne = 0.f;
	const UMaterialInterface* Source = Entry.Dynamic->Parent ? Entry.Dynamic->Parent.Get() : Entry.Dynamic.Get();
	if (!Source->GetScalarParameterValue(FHashedMaterialParameterInfo(Parameter), ValueAtOne))
	{
		UE_LOG(LogHologramOpacity, Warning, TEXT("%s has no scalar parameter '%s'; using 1.0 as its 100%% value"), *Source->GetPathName(), *Parameter.ToString());
		ValueAtOne = 1.f;
	}

	FHologramOpacityScalarRule& Rule = Entry.Rules.AddDefaulted_GetRef();
	Rule.Parameter = Parameter;
	Rule.ValueAtZero = ValueAtZero;
	Rule.ValueAtOne = ValueAtOne;
}

bool UHologramOpacitySubsystem::EnsureMaterials()
{
	if (bMaterialsReady || bMaterialsFailed)
	{
		return bMaterialsReady;
	}

	UMaterialInterface* PostProcess = LoadObject<UMaterialInterface>(nullptr, HologramOpacity::PostProcessPath);
	UMaterialInterface* Body = LoadObject<UMaterialInterface>(nullptr, HologramOpacity::BodyMaterialPath);
	if (!Body)
	{
		UE_LOG(LogHologramOpacity, Error, TEXT("Could not load %s; the mod stays inactive"), HologramOpacity::BodyMaterialPath);
		bMaterialsFailed = true;
		return false;
	}

	// Remember the material's own scan line settings, to put back when "Show Scanlines" is turned on again
	Body->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("BaseIntensity")), DefaultBaseIntensity);
	Body->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("LineIntensity")), DefaultLineIntensity);
	Body->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("LineOpacityBoost")), DefaultLineOpacityBoost);
	Body->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("LineDuty")), DefaultLineDuty);

	for (const TCHAR* Key : HologramOpacity::BodyKeys)
	{
		AddEntry(Key, nullptr, Body);
	}
	if (PostProcess)
	{
		// No rules by default: the post process is skipped on translucent bodies. Tune with HoloOpacity.Rule pp ...
		AddEntry(TEXT("pp"), PostProcess, PostProcess);
	}
	else
	{
		UE_LOG(LogHologramOpacity, Warning, TEXT("Hologram post process %s not found"), HologramOpacity::PostProcessPath);
	}

	bMaterialsReady = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Hologram materials ready (%d dynamic instances)"), Materials.Num());
	return true;
}

void UHologramOpacitySubsystem::RefreshDynamicParameters()
{
	if (!bMaterialsReady)
	{
		return;
	}
	for (FHologramOpacityMaterial& Entry : Materials)
	{
		if (!Entry.Dynamic)
		{
			continue;
		}
		for (const FHologramOpacityScalarRule& Rule : Entry.Rules)
		{
			Entry.Dynamic->SetScalarParameterValue(Rule.Parameter, FMath::Lerp(Rule.ValueAtZero, Rule.ValueAtOne, CurrentOpacity));
		}
	}

	// The body material takes the colours from the Holograms options
	const UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	LastColors[0] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramBuildColor()) : FLinearColor(0.f, 0.653f, 1.f);
	LastColors[1] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramSoftClearanceColor()) : FLinearColor(1.f, 0.753f, 0.f);
	LastColors[2] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramInvalidColor()) : FLinearColor(1.f, 0.161f, 0.161f);
	// Scan lines off: no line brightness or opacity, and the base takes the lines' average brightness so the colour
	// stays the same
	const float LineIntensity = bShowScanlines ? DefaultLineIntensity : 0.f;
	const float LineOpacityBoost = bShowScanlines ? DefaultLineOpacityBoost : 0.f;
	const float BaseIntensity = bShowScanlines ? DefaultBaseIntensity : DefaultBaseIntensity + DefaultLineIntensity * (1.f - DefaultLineDuty);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (FHologramOpacityMaterial* Entry = FindEntry(HologramOpacity::BodyKeys[Index]); Entry && Entry->Dynamic)
		{
			Entry->Dynamic->SetVectorParameterValue(TEXT("HoloColor"), LastColors[Index]);
			Entry->Dynamic->SetScalarParameterValue(TEXT("Opacity"), CurrentOpacity);
			Entry->Dynamic->SetScalarParameterValue(TEXT("LineIntensity"), LineIntensity);
			Entry->Dynamic->SetScalarParameterValue(TEXT("LineOpacityBoost"), LineOpacityBoost);
			Entry->Dynamic->SetScalarParameterValue(TEXT("BaseIntensity"), BaseIntensity);
		}
	}
}

UMaterialInterface* UHologramOpacitySubsystem::GetBodyMaterial(uint8 HologramMaterialState) const
{
	const FHologramOpacityMaterial* Entry = FindEntry(HologramOpacity::BodyKeys[HologramOpacity::StateIndex(HologramMaterialState)]);
	return Entry ? Entry->Dynamic.Get() : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Applying to holograms
// ---------------------------------------------------------------------------------------------------------------------

bool UHologramOpacitySubsystem::IsHologramBodyMesh(const UMeshComponent* Mesh)
{
	if (!Mesh->bRenderCustomDepth)
	{
		return false;
	}
	const int32 Stencil = Mesh->CustomDepthStencilValue;
	if (Stencil != HologramOpacity::StencilValid && Stencil != HologramOpacity::StencilSoftClearance && Stencil != HologramOpacity::StencilInvalid)
	{
		return false;
	}
	for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
	{
		const UMaterialInterface* Material = Mesh->GetMaterial(Index);
		if (Material && Material->GetPathName().StartsWith(HologramOpacity::BuildGunMaterialFolder))
		{
			return false;
		}
	}
	return Mesh->GetNumMaterials() > 0;
}

void UHologramOpacitySubsystem::ApplyToHologram(AFGHologram* Hologram)
{
	const uint8 State = static_cast<uint8>(Hologram->GetHologramMaterialState());
	const int32 StateIndex = HologramOpacity::StateIndex(State);
	UMaterialInterface* BodyMaterial = GetBodyMaterial(State);

	TInlineComponentArray<UMeshComponent*> Meshes(Hologram);
	for (UMeshComponent* Mesh : Meshes)
	{
		if (!IsValid(Mesh))
		{
			continue;
		}
		FHologramOpacitySavedMesh* Saved = ModifiedMeshes.Find(Mesh);
		if (!Saved)
		{
			if (!IsHologramBodyMesh(Mesh))
			{
				continue;
			}
			FHologramOpacitySavedMesh NewSaved;
			NewSaved.bRenderCustomDepth = Mesh->bRenderCustomDepth;
			NewSaved.bRenderInMainPass = Mesh->bRenderInMainPass;
			NewSaved.Stencil = Mesh->CustomDepthStencilValue;
			for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
			{
				NewSaved.Materials.Add(Mesh->GetMaterial(Index));
			}
			Saved = &ModifiedMeshes.Add(Mesh, MoveTemp(NewSaved));
		}

		const bool bTranslucent = Mode == Mode_Translucent || Mode == Mode_TranslucentWithPostProcess;
		if (bTranslucent && BodyMaterial)
		{
			const int32 NumMaterials = Mesh->GetNumMaterials();
			for (int32 Index = 0; Index < NumMaterials; ++Index)
			{
				if (Mesh->GetMaterial(Index) != BodyMaterial)
				{
					Mesh->SetMaterial(Index, BodyMaterial);
				}
			}
		}

		const bool bWantCustomDepth = Mode != Mode_Translucent || StencilOverride[StateIndex] >= 0;
		if (static_cast<bool>(Mesh->bRenderCustomDepth) != bWantCustomDepth)
		{
			Mesh->SetRenderCustomDepth(bWantCustomDepth);
		}
		if (StencilOverride[StateIndex] >= 0 && Mesh->CustomDepthStencilValue != StencilOverride[StateIndex])
		{
			Mesh->SetCustomDepthStencilValue(StencilOverride[StateIndex]);
		}

		const bool bWantMainPass = Mode != Mode_Ghost;
		if (static_cast<bool>(Mesh->bRenderInMainPass) != bWantMainPass)
		{
			Mesh->SetRenderInMainPass(bWantMainPass);
		}
	}
}

void UHologramOpacitySubsystem::ApplyToHolograms(UWorld* World)
{
	for (TActorIterator<AFGHologram> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			ApplyToHologram(*It);
		}
	}
}

void UHologramOpacitySubsystem::RestoreAllMeshes()
{
	for (TPair<TWeakObjectPtr<UMeshComponent>, FHologramOpacitySavedMesh>& Pair : ModifiedMeshes)
	{
		UMeshComponent* Mesh = Pair.Key.Get();
		if (!IsValid(Mesh))
		{
			continue;
		}
		const FHologramOpacitySavedMesh& Saved = Pair.Value;
		for (int32 Index = 0; Index < Saved.Materials.Num() && Index < Mesh->GetNumMaterials(); ++Index)
		{
			UMaterialInterface* Original = Saved.Materials[Index].Get();
			if (Original && Mesh->GetMaterial(Index) != Original)
			{
				Mesh->SetMaterial(Index, Original);
			}
		}
		if (static_cast<bool>(Mesh->bRenderCustomDepth) != Saved.bRenderCustomDepth)
		{
			Mesh->SetRenderCustomDepth(Saved.bRenderCustomDepth);
		}
		if (static_cast<bool>(Mesh->bRenderInMainPass) != Saved.bRenderInMainPass)
		{
			Mesh->SetRenderInMainPass(Saved.bRenderInMainPass);
		}
		// Only debug stencil overrides change the value; otherwise the game's own value is still in place
		if (StencilOverride[0] >= 0 || StencilOverride[1] >= 0 || StencilOverride[2] >= 0)
		{
			Mesh->SetCustomDepthStencilValue(Saved.Stencil);
		}
	}
	ModifiedMeshes.Reset();
}

void UHologramOpacitySubsystem::ApplyToPostProcess(UWorld* World)
{
	const FHologramOpacityMaterial* PostProcess = FindEntry(TEXT("pp"));
	if (!PostProcess || !PostProcess->Dynamic || !PostProcess->Original)
	{
		return;
	}
	// Only swap the blendable when something is tuned on it; otherwise the vanilla instance is identical.
	const bool bWantDynamic = !IsVanilla() && PostProcess->Rules.Num() > 0;
	UObject* OriginalObject = PostProcess->Original.Get();
	UObject* DynamicObject = PostProcess->Dynamic.Get();
	UObject* From = bWantDynamic ? OriginalObject : DynamicObject;
	UObject* To = bWantDynamic ? DynamicObject : OriginalObject;

	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		for (FWeightedBlendable& Blendable : It->Settings.WeightedBlendables.Array)
		{
			if (Blendable.Object == From)
			{
				Blendable.Object = To;
			}
		}
	}
}

void UHologramOpacitySubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetTickableGameObjectWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	const float NewOpacity = ReadOpacitySetting();
	const bool bNewScanlines = ReadScanlinesSetting();
	const bool bOpacityChanged = !FMath::IsNearlyEqual(NewOpacity, CurrentOpacity, 0.0005f) || bNewScanlines != bShowScanlines;
	if (bOpacityChanged)
	{
		UE_LOG(LogHologramOpacity, Verbose, TEXT("Hologram opacity %.3f -> %.3f, scanlines %d -> %d"), CurrentOpacity, NewOpacity,
			bShowScanlines ? 1 : 0, bNewScanlines ? 1 : 0);
		CurrentOpacity = NewOpacity;
		bShowScanlines = bNewScanlines;
	}

	const bool bWantMeshChanges = WantsMeshChanges();
	if (bLastApplied && !bWantMeshChanges)
	{
		RestoreAllMeshes();
	}
	bLastApplied = bWantMeshChanges;

	// Nothing to do while holograms are vanilla and nothing of ours was ever created
	if (!bWantMeshChanges && !bMaterialsReady)
	{
		return;
	}
	if (!EnsureMaterials())
	{
		return;
	}

	const double Now = World->GetRealTimeSeconds();
	const bool bSlowCheck = Now - LastSlowCheck > 1.0;
	bool bColorsChanged = false;
	if (bSlowCheck)
	{
		LastSlowCheck = Now;
		// Pick up colour changes from the Holograms options and forget destroyed hologram meshes
		if (const UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings())
		{
			bColorsChanged = !LastColors[0].Equals(HologramOpacity::ToLinearColor(GameUserSettings->GetHologramBuildColor()))
				|| !LastColors[1].Equals(HologramOpacity::ToLinearColor(GameUserSettings->GetHologramSoftClearanceColor()))
				|| !LastColors[2].Equals(HologramOpacity::ToLinearColor(GameUserSettings->GetHologramInvalidColor()));
		}
		for (auto It = ModifiedMeshes.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}

	if (bOpacityChanged || bForceRefresh || bColorsChanged)
	{
		RefreshDynamicParameters();
	}
	if (bWantMeshChanges)
	{
		ApplyToHolograms(World);
	}
	if (bOpacityChanged || bForceRefresh || bSlowCheck)
	{
		ApplyToPostProcess(World);
	}
	bForceRefresh = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------------------------------------------------

void UHologramOpacitySubsystem::SetDebugOpacityOverride(float InOpacity)
{
	DebugOpacityOverride = InOpacity;
	bForceRefresh = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Debug opacity override: %.3f"), InOpacity);
}

void UHologramOpacitySubsystem::SetDebugScanlinesOverride(int32 InValue)
{
	DebugScanlinesOverride = InValue < 0 ? -1 : (InValue != 0 ? 1 : 0);
	bForceRefresh = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Debug scanlines override: %d"), DebugScanlinesOverride);
}

void UHologramOpacitySubsystem::SetMode(int32 InMode)
{
	RestoreAllMeshes();
	Mode = FMath::Clamp(InMode, 0, 3);
	bLastApplied = false;
	bForceRefresh = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Hologram mode: %d"), Mode);
}

void UHologramOpacitySubsystem::SetStencilOverrides(int32 Ok, int32 Warn, int32 Error)
{
	RestoreAllMeshes();
	StencilOverride[0] = Ok;
	StencilOverride[1] = Warn;
	StencilOverride[2] = Error;
	bLastApplied = false;
	bForceRefresh = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Stencil overrides: ok %d, warn %d, error %d"), Ok, Warn, Error);
}

void UHologramOpacitySubsystem::SetScalarRule(const FString& Key, FName Parameter, float ValueAtZero)
{
	EnsureMaterials();
	if (FHologramOpacityMaterial* Entry = FindEntry(Key))
	{
		AddRule(*Entry, Parameter, ValueAtZero);
		bForceRefresh = true;
		UE_LOG(LogHologramOpacity, Display, TEXT("Rule %s.%s: %.4f at 0%%"), *Key, *Parameter.ToString(), ValueAtZero);
	}
	else
	{
		UE_LOG(LogHologramOpacity, Warning, TEXT("Unknown material key '%s'"), *Key);
	}
}

void UHologramOpacitySubsystem::SetScalarDirect(const FString& Key, FName Parameter, float Value)
{
	EnsureMaterials();
	int32 Count = 0;
	for (FHologramOpacityMaterial& Entry : Materials)
	{
		const bool bMatch = Entry.Key.Equals(Key, ESearchCase::IgnoreCase) || (Key.Equals(TEXT("custom"), ESearchCase::IgnoreCase) && Entry.Key.StartsWith(TEXT("custom_")));
		if (bMatch && Entry.Dynamic)
		{
			Entry.Dynamic->SetScalarParameterValue(Parameter, Value);
			++Count;
		}
	}
	UE_LOG(LogHologramOpacity, Display, TEXT("%s.%s = %.4f (%d materials)"), *Key, *Parameter.ToString(), Value, Count);
}

void UHologramOpacitySubsystem::DumpState()
{
	UE_LOG(LogHologramOpacity, Display, TEXT("==== HologramOpacity dump ===="));
	UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	const UFGUserSettingApplyType* Setting = GameUserSettings && GameUserSettings->HasInitialized() ? GameUserSettings->FindUserSetting(SettingId) : nullptr;
	if (Setting)
	{
		const FVariant Value = Setting->GetDisplayValue();
		UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s found: type %d, value '%s'"), SettingId, static_cast<int32>(Value.GetType()), *UFGUserSettingApplyType::VariantAsString(Value));
	}
	else
	{
		const int32 bSettingsInitialized = GameUserSettings && GameUserSettings->HasInitialized() ? 1 : 0;
		UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s NOT registered (game user settings initialized: %d)"), SettingId, bSettingsInitialized);
	}
	const UFGUserSettingApplyType* ScanlinesSetting = GameUserSettings && GameUserSettings->HasInitialized() ? GameUserSettings->FindUserSetting(ScanlinesSettingId) : nullptr;
	if (ScanlinesSetting)
	{
		const FVariant Value = ScanlinesSetting->GetDisplayValue();
		UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s found: type %d, value '%s'"), ScanlinesSettingId, static_cast<int32>(Value.GetType()), *UFGUserSettingApplyType::VariantAsString(Value));
	}
	else
	{
		UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s NOT registered"), ScanlinesSettingId);
	}
	const int32 bReady = bMaterialsReady ? 1 : 0;
	const int32 bLines = bShowScanlines ? 1 : 0;
	UE_LOG(LogHologramOpacity, Display, TEXT("Opacity %.3f (override %.3f), scanlines %d (override %d), mode %d, stencil overrides %d/%d/%d, materials ready %d, modified meshes %d"),
		CurrentOpacity, DebugOpacityOverride, bLines, DebugScanlinesOverride, Mode, StencilOverride[0], StencilOverride[1], StencilOverride[2], bReady, ModifiedMeshes.Num());
	UE_LOG(LogHologramOpacity, Display, TEXT("Material line defaults: base %.3f, lines %.3f, line opacity %.3f, duty %.3f"),
		DefaultBaseIntensity, DefaultLineIntensity, DefaultLineOpacityBoost, DefaultLineDuty);

	for (const FHologramOpacityMaterial& Entry : Materials)
	{
		FString Rules;
		for (const FHologramOpacityScalarRule& Rule : Entry.Rules)
		{
			Rules += FString::Printf(TEXT(" %s[%.3f..%.3f]"), *Rule.Parameter.ToString(), Rule.ValueAtZero, Rule.ValueAtOne);
		}
		UE_LOG(LogHologramOpacity, Display, TEXT("  %s: %s -> %s%s"), *Entry.Key, *HologramOpacity::DescribeMaterial(Entry.Original), *HologramOpacity::DescribeMaterial(Entry.Dynamic), *Rules);
	}

	UWorld* World = GetTickableGameObjectWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AFGHologram> It(World); It; ++It)
	{
		AFGHologram* Hologram = *It;
		TInlineComponentArray<UMeshComponent*> Meshes(Hologram);
		UE_LOG(LogHologramOpacity, Display, TEXT("Hologram %s (%s) state %d, %d mesh components"), *Hologram->GetName(), *Hologram->GetClass()->GetName(),
			static_cast<int32>(Hologram->GetHologramMaterialState()), Meshes.Num());
		for (UMeshComponent* Mesh : Meshes)
		{
			const int32 bCustomDepth = Mesh->bRenderCustomDepth ? 1 : 0;
			const int32 Stencil = Mesh->CustomDepthStencilValue;
			const int32 bMainPass = Mesh->bRenderInMainPass ? 1 : 0;
			const int32 bTracked = ModifiedMeshes.Contains(Mesh) ? 1 : 0;
			const FString FirstMaterial = Mesh->GetNumMaterials() > 0 ? HologramOpacity::DescribeMaterial(Mesh->GetMaterial(0)) : FString(TEXT("-"));
			UE_LOG(LogHologramOpacity, Display, TEXT("    comp %s (%s) customDepth %d stencil %d mainPass %d tracked %d slots %d mat0 %s"), *Mesh->GetName(),
				*Mesh->GetClass()->GetName(), bCustomDepth, Stencil, bMainPass, bTracked, Mesh->GetNumMaterials(), *FirstMaterial);
		}
	}
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		const int32 bUnbound = It->bUnbound ? 1 : 0;
		for (const FWeightedBlendable& Blendable : It->Settings.WeightedBlendables.Array)
		{
			const FString BlendableName = Blendable.Object ? Blendable.Object->GetPathName() : FString(TEXT("None"));
			UE_LOG(LogHologramOpacity, Display, TEXT("PostProcessVolume %s (unbound %d): blendable %s weight %.2f"), *It->GetName(), bUnbound,
				*BlendableName, Blendable.Weight);
		}
	}
}
