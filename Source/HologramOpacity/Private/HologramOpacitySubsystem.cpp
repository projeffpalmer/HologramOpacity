#include "HologramOpacitySubsystem.h"
#include "HologramOpacityModule.h"

#include "Components/MeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/GameInstance.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Variant.h"

#include "FGGameUserSettings.h"
#include "Hologram/FGHologram.h"
#include "Settings/FGUserSettingApplyType.h"

const TCHAR* UHologramOpacitySubsystem::SettingId = TEXT("HologramOpacity.Opacity");
const TCHAR* UHologramOpacitySubsystem::ScanlinesSettingId = TEXT("HologramOpacity.Scanlines");

namespace HologramOpacity
{
	// Post process material that draws the hologram from the capture's buffers (this mod's content)
	static const TCHAR* CompositeMaterialPath = TEXT("/HologramOpacity/Materials/M_HologramOpacityComposite.M_HologramOpacityComposite");
	// The captured hologram (lit colour in RGB, depth in A), read by the composite material (this mod's content)
	static const TCHAR* CaptureTargetPath = TEXT("/HologramOpacity/Materials/RT_HologramOpacityCapture.RT_HologramOpacityCapture");
	// Materials of the build gun's helper meshes (guidelines, clearance boxes, arrows, connection markers)
	static const TCHAR* BuildGunMaterialFolder = TEXT("/Game/FactoryGame/Equipment/BuildGun/");
	// Custom depth stencil values the game writes on hologram bodies (PPI_OutlineHologram bitmask values)
	static constexpr int32 StencilSoftClearance = 3;
	static constexpr int32 StencilValid = 5;
	static constexpr int32 StencilInvalid = 6;
	// Above the game's own post process volumes
	static constexpr float VolumePriority = 1000.f;

	static TWeakObjectPtr<UHologramOpacitySubsystem> GActive;

	static FLinearColor ToLinearColor(const FVector& V)
	{
		return FLinearColor(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z), 1.f);
	}

	static int32 StateIndex(uint8 HologramMaterialState)
	{
		return HologramMaterialState == static_cast<uint8>(EHologramMaterialState::HMS_ERROR) ? 2
			: HologramMaterialState == static_cast<uint8>(EHologramMaterialState::HMS_WARNING) ? 1
			: 0;
	}

	static FString Describe(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("None"));
	}

	/** Transient actors only: Satisfactory saves actors that implement its save interface, and these don't */
	static FActorSpawnParameters TransientSpawnParameters()
	{
		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transient;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Parameters;
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

static FAutoConsoleCommand GHoloOpacityParamCmd(
	TEXT("HoloOpacity.Param"),
	TEXT("HoloOpacity.Param <parameter> <value>  Tune a scalar parameter of the composite material until the game restarts."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive(); Subsystem && Args.Num() > 1)
		{
			Subsystem->SetCompositeParameter(FName(*Args[0]), FCString::Atof(*Args[1]));
		}
	}));

static FAutoConsoleCommand GHoloOpacityDumpCmd(
	TEXT("HoloOpacity.Dump"),
	TEXT("HoloOpacity.Dump  Log the settings, the composite and every hologram mesh."),
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
	UE_LOG(LogHologramOpacity, Display, TEXT("Hologram Opacity subsystem initialized"));
}

void UHologramOpacitySubsystem::Deinitialize()
{
	StopComposite();
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
// Settings
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
	// The slider stores 0..1 (MinValue..MaxValue); its 0..100 range is display only.
	// GetDisplayValue() returns the stored value (logged 0.32 at 32% and 0.09 at 9% in game).
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

void UHologramOpacitySubsystem::ReadColors()
{
	// The colours from the Holograms options, per hologram state
	const UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	Colors[0] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramBuildColor()) : FLinearColor(0.f, 0.653f, 1.f);
	Colors[1] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramSoftClearanceColor()) : FLinearColor(1.f, 0.753f, 0.f);
	Colors[2] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramInvalidColor()) : FLinearColor(1.f, 0.161f, 0.161f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Composite
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

bool UHologramOpacitySubsystem::EnsureComposite(UWorld* World)
{
	if (bCompositeFailed)
	{
		return false;
	}

	if (!CompositeMaterial)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, HologramOpacity::CompositeMaterialPath);
		CaptureTarget = LoadObject<UTextureRenderTarget2D>(nullptr, HologramOpacity::CaptureTargetPath);
		if (!Material || !CaptureTarget)
		{
			UE_LOG(LogHologramOpacity, Error, TEXT("Could not load %s or %s; holograms stay vanilla"), HologramOpacity::CompositeMaterialPath, HologramOpacity::CaptureTargetPath);
			bCompositeFailed = true;
			return false;
		}
		CompositeMaterial = UMaterialInstanceDynamic::Create(Material, this, TEXT("MID_HologramOpacityComposite"));
		ReadColors();
		bParametersDirty = true;
	}

	if (!Capture.IsValid() || Capture->GetWorld() != World)
	{
		FActorSpawnParameters Parameters = HologramOpacity::TransientSpawnParameters();
		Parameters.bDeferConstruction = true;
		ASceneCapture2D* NewCapture = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), FTransform::Identity, Parameters);
		if (!NewCapture)
		{
			UE_LOG(LogHologramOpacity, Error, TEXT("Could not spawn the hologram scene capture; holograms stay vanilla"));
			bCompositeFailed = true;
			return false;
		}
		// Drawn on request (CaptureSceneDeferred) with the main view's camera, resolution and jitter, so it lines up with
		// the frame it is composited into. Only the hologram bodies are drawn, lit, with their own materials.
		USceneCaptureComponent2D* Component = NewCapture->GetCaptureComponent2D();
		Component->bCaptureEveryFrame = false;
		Component->bCaptureOnMovement = false;
		Component->bMainViewCamera = true;
		// The main view's temporal upscaler (TSR, DLSS, ...) comes with the camera and needs a persistent view state
		Component->bAlwaysPersistRenderingState = true;
		Component->CaptureSource = ESceneCaptureSource::SCS_SceneColorSceneDepth;
		Component->TextureTarget = CaptureTarget;
		Component->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		// The hologram's own look needs none of the expensive or screen-space extras
		Component->ShowFlags.SetLumenGlobalIllumination(false);
		Component->ShowFlags.SetLumenReflections(false);
		Component->ShowFlags.SetAmbientOcclusion(false);
		Component->ShowFlags.SetScreenSpaceReflections(false);
		Component->ShowFlags.SetFog(false);
		Component->ShowFlags.SetVolumetricFog(false);
		Component->ShowFlags.SetMotionBlur(false);
		NewCapture->FinishSpawning(FTransform::Identity);
		Capture = NewCapture;
	}

	if (!Volume.IsValid() || Volume->GetWorld() != World)
	{
		APostProcessVolume* NewVolume = World->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, HologramOpacity::TransientSpawnParameters());
		if (!NewVolume)
		{
			UE_LOG(LogHologramOpacity, Error, TEXT("Could not spawn the hologram post process volume; holograms stay vanilla"));
			bCompositeFailed = true;
			return false;
		}
		NewVolume->bUnbound = true;
		NewVolume->bEnabled = false;
		NewVolume->Priority = HologramOpacity::VolumePriority;
		NewVolume->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, CompositeMaterial));
		Volume = NewVolume;
		bCompositeEnabled = false;
	}
	return true;
}

void UHologramOpacitySubsystem::UpdateComposite(UWorld* World, const TArray<UMeshComponent*>& Bodies, int32 HologramState)
{
	const TSet<UMeshComponent*> BodySet(Bodies);

	// New hologram bodies leave the main view (the game's post process stops drawing them); the capture still sees them
	for (UMeshComponent* Mesh : Bodies)
	{
		if (!HiddenMeshes.Contains(Mesh))
		{
			HiddenMeshes.Add(Mesh, static_cast<bool>(Mesh->bVisibleInSceneCaptureOnly));
			Mesh->SetVisibleInSceneCaptureOnly(true);
		}
	}
	// Meshes that are gone or no longer hologram bodies get their own flag back
	for (auto It = HiddenMeshes.CreateIterator(); It; ++It)
	{
		UMeshComponent* Mesh = It.Key().Get();
		if (!Mesh)
		{
			It.RemoveCurrent();
		}
		else if (!BodySet.Contains(Mesh))
		{
			Mesh->SetVisibleInSceneCaptureOnly(It.Value());
			It.RemoveCurrent();
		}
	}

	if (HologramState != StateIndex)
	{
		StateIndex = HologramState;
		bParametersDirty = true;
	}

	USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
	Component->ShowOnlyComponents.Reset(Bodies.Num());
	for (UMeshComponent* Mesh : Bodies)
	{
		Component->ShowOnlyComponents.Add(Mesh);
	}
	SetCompositeEnabled(Bodies.Num() > 0);
	if (Bodies.Num() > 0)
	{
		Component->CaptureSceneDeferred();
	}
}

void UHologramOpacitySubsystem::StopComposite()
{
	for (const TPair<TWeakObjectPtr<UMeshComponent>, bool>& Pair : HiddenMeshes)
	{
		if (UMeshComponent* Mesh = Pair.Key.Get())
		{
			Mesh->SetVisibleInSceneCaptureOnly(Pair.Value);
		}
	}
	HiddenMeshes.Reset();
	if (ASceneCapture2D* SceneCapture = Capture.Get())
	{
		SceneCapture->GetCaptureComponent2D()->ShowOnlyComponents.Reset();
	}
	SetCompositeEnabled(false);
}

void UHologramOpacitySubsystem::SetCompositeEnabled(bool bEnabled)
{
	if (APostProcessVolume* PostProcessVolume = Volume.Get())
	{
		PostProcessVolume->bEnabled = bEnabled;
	}
	bCompositeEnabled = bEnabled;
}

void UHologramOpacitySubsystem::RefreshCompositeParameters()
{
	if (!CompositeMaterial)
	{
		return;
	}
	CompositeMaterial->SetScalarParameterValue(TEXT("Opacity"), CurrentOpacity);
	CompositeMaterial->SetScalarParameterValue(TEXT("ShowScanlines"), bShowScanlines ? 1.f : 0.f);
	CompositeMaterial->SetVectorParameterValue(TEXT("HoloColor"), Colors[FMath::Clamp(StateIndex, 0, 2)]);
	for (const TPair<FName, float>& Override : ParameterOverrides)
	{
		CompositeMaterial->SetScalarParameterValue(Override.Key, Override.Value);
	}
}

void UHologramOpacitySubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetTickableGameObjectWorld();
	if (!World || !World->IsGameWorld() || !World->HasBegunPlay())
	{
		return;
	}

	const float NewOpacity = ReadOpacitySetting();
	const bool bNewScanlines = ReadScanlinesSetting();
	if (!FMath::IsNearlyEqual(NewOpacity, CurrentOpacity, 0.0005f) || bNewScanlines != bShowScanlines)
	{
		UE_LOG(LogHologramOpacity, Verbose, TEXT("Hologram opacity %.3f -> %.3f, scanlines %d -> %d"), CurrentOpacity, NewOpacity,
			bShowScanlines ? 1 : 0, bNewScanlines ? 1 : 0);
		CurrentOpacity = NewOpacity;
		bShowScanlines = bNewScanlines;
		bParametersDirty = true;
	}

	if (!WantsComposite())
	{
		if (bCompositeEnabled || HiddenMeshes.Num() > 0)
		{
			StopComposite();
		}
		return;
	}

	// The hologram bodies to draw, and the most severe state among their holograms
	TArray<UMeshComponent*> Bodies;
	int32 HologramState = 0;
	for (TActorIterator<AFGHologram> It(World); It; ++It)
	{
		AFGHologram* Hologram = *It;
		if (!IsValid(Hologram))
		{
			continue;
		}
		const int32 NumBefore = Bodies.Num();
		TInlineComponentArray<UMeshComponent*> Meshes(Hologram);
		for (UMeshComponent* Mesh : Meshes)
		{
			// Meshes already hidden were classified before; the full check reads material paths
			if (IsValid(Mesh) && (HiddenMeshes.Contains(Mesh) ? static_cast<bool>(Mesh->bRenderCustomDepth) : IsHologramBodyMesh(Mesh)))
			{
				Bodies.Add(Mesh);
			}
		}
		if (Bodies.Num() > NumBefore)
		{
			HologramState = FMath::Max(HologramState, HologramOpacity::StateIndex(static_cast<uint8>(Hologram->GetHologramMaterialState())));
		}
	}

	if (Bodies.Num() == 0 && HiddenMeshes.Num() == 0)
	{
		SetCompositeEnabled(false);
		return;
	}
	if (!EnsureComposite(World))
	{
		StopComposite();
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastColorCheck > 1.0)
	{
		LastColorCheck = Now;
		const FLinearColor Previous[3] = {Colors[0], Colors[1], Colors[2]};
		ReadColors();
		if (!Previous[0].Equals(Colors[0]) || !Previous[1].Equals(Colors[1]) || !Previous[2].Equals(Colors[2]))
		{
			bParametersDirty = true;
		}
	}

	UpdateComposite(World, Bodies, HologramState);
	if (bParametersDirty)
	{
		RefreshCompositeParameters();
		bParametersDirty = false;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------------------------------------------------

void UHologramOpacitySubsystem::SetDebugOpacityOverride(float InOpacity)
{
	DebugOpacityOverride = InOpacity;
	bParametersDirty = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Debug opacity override: %.3f"), InOpacity);
}

void UHologramOpacitySubsystem::SetDebugScanlinesOverride(int32 InValue)
{
	DebugScanlinesOverride = InValue < 0 ? -1 : (InValue != 0 ? 1 : 0);
	bParametersDirty = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Debug scanlines override: %d"), DebugScanlinesOverride);
}

void UHologramOpacitySubsystem::SetCompositeParameter(FName Parameter, float Value)
{
	ParameterOverrides.Add(Parameter, Value);
	bParametersDirty = true;
	UE_LOG(LogHologramOpacity, Display, TEXT("Composite %s = %.4f"), *Parameter.ToString(), Value);
}

void UHologramOpacitySubsystem::DumpState()
{
	UE_LOG(LogHologramOpacity, Display, TEXT("==== HologramOpacity dump ===="));
	UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	for (const TCHAR* Id : {SettingId, ScanlinesSettingId})
	{
		const UFGUserSettingApplyType* Setting = GameUserSettings && GameUserSettings->HasInitialized() ? GameUserSettings->FindUserSetting(Id) : nullptr;
		if (Setting)
		{
			const FVariant Value = Setting->GetDisplayValue();
			UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s: type %d, value '%s'"), Id, static_cast<int32>(Value.GetType()), *UFGUserSettingApplyType::VariantAsString(Value));
		}
		else
		{
			UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s NOT registered"), Id);
		}
	}

	const int32 bLines = bShowScanlines ? 1 : 0;
	const int32 bEnabled = bCompositeEnabled ? 1 : 0;
	const int32 bFailed = bCompositeFailed ? 1 : 0;
	UE_LOG(LogHologramOpacity, Display, TEXT("Opacity %.3f (override %.3f), scanlines %d (override %d), composite enabled %d (failed %d), state %d, hidden meshes %d"),
		CurrentOpacity, DebugOpacityOverride, bLines, DebugScanlinesOverride, bEnabled, bFailed, StateIndex, HiddenMeshes.Num());
	const ASceneCapture2D* SceneCapture = Capture.Get();
	const APostProcessVolume* PostProcessVolume = Volume.Get();
	UE_LOG(LogHologramOpacity, Display, TEXT("Capture %s (show-only %d), volume %s (enabled %d), material %s, capture target %s %dx%d"),
		*HologramOpacity::Describe(SceneCapture), SceneCapture ? SceneCapture->GetCaptureComponent2D()->ShowOnlyComponents.Num() : 0,
		*HologramOpacity::Describe(PostProcessVolume), PostProcessVolume ? static_cast<int32>(PostProcessVolume->bEnabled) : 0,
		*HologramOpacity::Describe(CompositeMaterial), *HologramOpacity::Describe(CaptureTarget),
		CaptureTarget ? CaptureTarget->SizeX : 0, CaptureTarget ? CaptureTarget->SizeY : 0);
	for (const TPair<FName, float>& Override : ParameterOverrides)
	{
		UE_LOG(LogHologramOpacity, Display, TEXT("  param %s = %.4f"), *Override.Key.ToString(), Override.Value);
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
			const int32 bBody = IsHologramBodyMesh(Mesh) ? 1 : 0;
			const int32 bHidden = HiddenMeshes.Contains(Mesh) ? 1 : 0;
			const int32 bCaptureOnly = Mesh->bVisibleInSceneCaptureOnly ? 1 : 0;
			const int32 bCustomDepth = Mesh->bRenderCustomDepth ? 1 : 0;
			UE_LOG(LogHologramOpacity, Display, TEXT("    comp %s (%s) body %d hidden %d captureOnly %d customDepth %d stencil %d slots %d"), *Mesh->GetName(),
				*Mesh->GetClass()->GetName(), bBody, bHidden, bCaptureOnly, bCustomDepth, Mesh->CustomDepthStencilValue, Mesh->GetNumMaterials());
		}
	}
}
