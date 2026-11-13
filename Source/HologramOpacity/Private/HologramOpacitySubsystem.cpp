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

#include "AbstractInstanceInterface.h"
#include "AbstractInstanceManager.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGBackgroundThread.h"
#include "FGColoredInstanceManager.h"
#include "FGColoredInstanceMeshProxy.h"
#include "FGGameUserSettings.h"
#include "FGOutlineComponent.h"
#include "GameFramework/PlayerController.h"
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
	// The dismantle highlight's stencil value (EOutlineColor::OC_DISMANTLE)
	static constexpr int32 StencilDismantle = 8;
	// Composite colour index of dismantle highlights (after the three hologram states)
	static constexpr int32 DismantleStateIndex = 3;
	// Contour brightness of dismantle highlights (the game's contour is a brighter dismantle colour)
	static constexpr float DismantleEdgeIntensity = 1.5f;
	// How far (cm) a pooled fog plane may be from where its building says it is (seen up to 19 cm on a Constructor,
	// whose openings are metres apart)
	static constexpr float FogPlaneTolerance = 50.f;
	// Scale of a hidden instance, like FInstanceOwnershipHandle::HideInstance
	static constexpr float HiddenScale = 0.001f;
	// Scale this mod shrinks coloured instances to: below the hidden threshold, but unlike any other hide, so an instance
	// is only restored while this mod's shrink is still on it
	static constexpr float OwnHiddenScale = 0.000731f;
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

	/** True for a material of the build gun's helper meshes. This runs every frame for hologram meshes the mod hasn't
	 *  taken over, so it reads the package name into a stack buffer instead of building the material's path string. */
	static bool IsBuildGunMaterial(const UMaterialInterface* Material)
	{
		const FNameBuilder Package(Material->GetOutermost()->GetFName());
		return Package.ToView().StartsWith(BuildGunMaterialFolder);
	}

	static FString Describe(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("None"));
	}

	// Console command help, also logged when an argument is missing or isn't a number
	static const TCHAR* const SetUsage = TEXT("HoloOpacity.Set <0..1 | -1>  Override the hologram opacity for testing. -1 goes back to the Options value.");
	static const TCHAR* const ScanlinesUsage = TEXT("HoloOpacity.Scanlines <1 | 0 | -1>  Override Show Scanlines for testing (1 on, 0 off). -1 goes back to the Options value.");
	static const TCHAR* const ParamUsage = TEXT("HoloOpacity.Param <parameter> <value>  Tune a scalar parameter of the composite material until the game restarts.");

	static void LogUsage(const TCHAR* Usage)
	{
		UE_LOG(LogHologramOpacity, Warning, TEXT("Usage: %s"), Usage);
	}

	/** Reads console argument Index as a number, or logs the command's usage. The whole argument must be numeric:
	 *  FCString::Atof reads "off" as 0 and "50%" as 50. */
	template <typename TNumber>
	static bool ParseArgument(const TArray<FString>& Args, int32 Index, TNumber& OutValue, const TCHAR* Usage)
	{
		if (Args.IsValidIndex(Index) && FCString::IsNumeric(*Args[Index]) && LexTryParseString(OutValue, *Args[Index]))
		{
			return true;
		}
		LogUsage(Usage);
		return false;
	}

	/** The running subsystem for a console command, or null with a warning */
	static UHologramOpacitySubsystem* CommandTarget()
	{
		UHologramOpacitySubsystem* Subsystem = UHologramOpacitySubsystem::GetActive();
		if (!Subsystem)
		{
			UE_LOG(LogHologramOpacity, Warning, TEXT("No active HologramOpacity subsystem"));
		}
		return Subsystem;
	}

	/** Transient actors only: Satisfactory saves actors that implement its save interface, and these don't */
	static FActorSpawnParameters TransientSpawnParameters()
	{
		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transient;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Parameters;
	}

	/** True for an instance shrunk out of sight (by the game's HideInstance or by this mod) */
	static bool IsHiddenScale(const FTransform& Transform)
	{
		return Transform.GetScale3D().GetAbsMax() < HiddenScale * 10.f;
	}

	/** True for an instance still shrunk by this mod (OwnHiddenScale on every axis) */
	static bool IsOwnHiddenScale(const FTransform& Transform)
	{
		return Transform.GetScale3D().Equals(FVector(OwnHiddenScale), OwnHiddenScale * 0.05f);
	}

	/** The instanced component a coloured instance manager draws its instances with (a reflected private property) */
	static UInstancedStaticMeshComponent* ColoredInstanceComponent(const UFGColoredInstanceMeshProxy* Mesh)
	{
		static const FObjectProperty* Property = FindFProperty<FObjectProperty>(UFGColoredInstanceManager::StaticClass(), TEXT("mInstanceComponent"));
		const UFGColoredInstanceManager* Manager = Mesh->mInstanceManager;
		return Property && Manager ? Cast<UInstancedStaticMeshComponent>(Property->GetObjectPropertyValue_InContainer(Manager)) : nullptr;
	}

	/** An instanced component that draws like Source (mesh, materials, per-instance data), for the capture only */
	static UInstancedStaticMeshComponent* CreateInstanceCopy(const UInstancedStaticMeshComponent* Source, AActor* Owner)
	{
		UInstancedStaticMeshComponent* Copy = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None, RF_Transient);
		Copy->SetMobility(EComponentMobility::Movable);
		Copy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Copy->SetGenerateOverlapEvents(false);
		Copy->SetCanEverAffectNavigation(false);
		Copy->SetStaticMesh(Source->GetStaticMesh());
		for (int32 Slot = 0; Slot < Source->GetNumMaterials(); ++Slot)
		{
			Copy->SetMaterial(Slot, Source->GetMaterial(Slot));
		}
		Copy->SetNumCustomDataFloats(Source->NumCustomDataFloats);
		Copy->CastShadow = Source->CastShadow;
		Copy->bCastDynamicShadow = Source->bCastDynamicShadow;
		// Out of the main view (hidden component list), but it still casts the hidden building's shadow there
		Copy->bCastHiddenShadow = true;
		Copy->LightingChannels = Source->LightingChannels;
		Copy->bReceivesDecals = Source->bReceivesDecals;
		Copy->bEvaluateWorldPositionOffset = Source->bEvaluateWorldPositionOffset;
		Copy->WorldPositionOffsetDisableDistance = Source->WorldPositionOffsetDisableDistance;
		Copy->RegisterComponent();
		return Copy;
	}

	/** One of the outline component's reflected (UPROPERTY) arrays of FActorOutlineState, or null if its layout differs */
	static const FArrayProperty* FindOutlineStatesProperty(const TCHAR* PropertyName)
	{
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(UFGOutlineComponent::StaticClass(), PropertyName);
		const FStructProperty* Inner = Property ? CastField<FStructProperty>(Property->Inner) : nullptr;
		if (!Inner || Inner->Struct->GetFName() != TEXT("ActorOutlineState") || Inner->Struct->GetStructureSize() != sizeof(FActorOutlineState))
		{
			return nullptr;
		}
		return Property;
	}

	/** Whole-actor outline states (ShowOutline), read only */
	static const TArray<FActorOutlineState>* ActorOutlineStates(const UFGOutlineComponent* Outline)
	{
		static const FArrayProperty* Property = FindOutlineStatesProperty(TEXT("mActorOutlineStates"));
		return Property ? Property->ContainerPtrToValuePtr<TArray<FActorOutlineState>>(Outline) : nullptr;
	}

	/** Outline states of single meshes of an actor (ShowOutlineForStaticMeshComponent), read only */
	static const TArray<FActorOutlineState>* ComponentOutlineStates(const UFGOutlineComponent* Outline)
	{
		static const FArrayProperty* Property = FindOutlineStatesProperty(TEXT("mComponentSpecifiedActorOutlineStates"));
		return Property ? Property->ContainerPtrToValuePtr<TArray<FActorOutlineState>>(Outline) : nullptr;
	}

	static void DumpPrimitive(const TCHAR* Kind, const FName Key, const UPrimitiveComponent* Primitive)
	{
		if (!Primitive)
		{
			UE_LOG(LogHologramOpacity, Display, TEXT("      %s %s None"), Kind, *Key.ToString());
			return;
		}
		const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(Primitive);
		const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Primitive);
		const UMaterialInterface* Material = Primitive->GetNumMaterials() > 0 ? Primitive->GetMaterial(0) : nullptr;
		UE_LOG(LogHologramOpacity, Display,
			TEXT("      %s %s %s (%s) at %s owner %s parent %s mesh %s instances %d visible %d hiddenInGame %d mainPass %d captureOnly %d customDepth %d stencil %d slots %d mat0 %s"),
			Kind, *Key.ToString(), *Primitive->GetName(), *Primitive->GetClass()->GetName(), *Primitive->GetComponentLocation().ToCompactString(),
			Primitive->GetOwner() ? *Primitive->GetOwner()->GetName() : TEXT("None"),
			Primitive->GetAttachParent() ? *Primitive->GetAttachParent()->GetName() : TEXT("None"),
			StaticMesh && StaticMesh->GetStaticMesh() ? *StaticMesh->GetStaticMesh()->GetName() : TEXT("None"), Instanced ? Instanced->GetInstanceCount() : -1,
			Primitive->IsVisible() ? 1 : 0, Primitive->bHiddenInGame ? 1 : 0, Primitive->bRenderInMainPass ? 1 : 0, Primitive->bVisibleInSceneCaptureOnly ? 1 : 0,
			Primitive->bRenderCustomDepth ? 1 : 0, Primitive->CustomDepthStencilValue, Primitive->GetNumMaterials(), Material ? *Material->GetPathName() : TEXT("None"));
	}

	/** One outlined actor: its proxies, its own primitives and its abstract (shared, instanced) meshes */
	static void DumpOutlineState(const FActorOutlineState& State)
	{
		const AActor* Actor = State.Actor;
		const AFGBuildable* Buildable = Cast<AFGBuildable>(Actor);
		UE_LOG(LogHologramOpacity, Display, TEXT("    actor %s (%s) color %d onlyProxies %d proxies %d instancedProxies %d lightweightTemporary %d"),
			Actor ? *Actor->GetName() : TEXT("None"), Actor ? *Actor->GetClass()->GetName() : TEXT("None"), static_cast<int32>(State.OutlineColor),
			State.OnlyHighlightProxies ? 1 : 0, State.OutlineProxies.Num(), State.InstancedOutlineProxies.Num(), Buildable && Buildable->GetIsLightweightTemporary() ? 1 : 0);
		for (const TPair<FName, TObjectPtr<UStaticMeshComponent>>& Pair : State.OutlineProxies)
		{
			DumpPrimitive(TEXT("proxy"), Pair.Key, Pair.Value);
		}
		for (const TPair<FName, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : State.InstancedOutlineProxies)
		{
			DumpPrimitive(TEXT("instancedProxy"), Pair.Key, Pair.Value);
		}
		if (!Actor)
		{
			return;
		}
		if (const UFGBuildableSparseDataObject* SparseData = Buildable ? Buildable->GetBuildableSparseData() : nullptr)
		{
			for (const FTransform& Relative : SparseData->mFogPlaneLocations)
			{
				UE_LOG(LogHologramOpacity, Display, TEXT("      fog plane %s at %s"), *Describe(SparseData->FogPlaneMesh),
					*(Relative * Actor->GetActorTransform()).GetLocation().ToCompactString());
			}
		}
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (const UPrimitiveComponent* Primitive : Primitives)
		{
			DumpPrimitive(TEXT("own"), NAME_None, Primitive);
			if (const UFGColoredInstanceMeshProxy* Colored = Cast<UFGColoredInstanceMeshProxy>(Primitive); Colored && Colored->mInstanceHandle.IsInstanced())
			{
				const UInstancedStaticMeshComponent* Source = ColoredInstanceComponent(Colored);
				FTransform AtIndex;
				const bool bAtIndex = Source && Source->GetInstanceTransform(Colored->mInstanceHandle.GetHandleID(), AtIndex, true);
				UE_LOG(LogHologramOpacity, Display, TEXT("        coloured instance %d in %s (%d instances, %d floats) at %s scale %s | component at %s"),
					Colored->mInstanceHandle.GetHandleID(), Source ? *Source->GetName() : TEXT("None"), Source ? Source->GetInstanceCount() : -1,
					Source ? Source->NumCustomDataFloats : -1, bAtIndex ? *AtIndex.GetLocation().ToCompactString() : TEXT("-"),
					bAtIndex ? *AtIndex.GetScale3D().ToCompactString() : TEXT("-"), *Colored->GetComponentLocation().ToCompactString());
			}
		}
		const IAbstractInstanceInterface* Instances = Cast<IAbstractInstanceInterface>(Actor);
		if (!Instances)
		{
			return;
		}
		const FInstanceHandleArray Handles = Instances->GetLightweightInstanceHandles();
		for (const FInstanceHandlePtr& Handle : Handles.HandleArray)
		{
			if (!Handle.IsValid())
			{
				continue;
			}
			// The handle ID is the instance's index in its component; a hidden instance is shrunk there
			const ULightweightHierarchicalInstancedStaticMeshComponent* Component = Handle->GetInstanceComponent();
			const int32 Index = static_cast<int32>(Handle->GetHandleID());
			FTransform AtIndex;
			const bool bAtIndex = Component && Handle->IsInstanced() && Component->GetInstanceTransform(Index, AtIndex, true);
			UE_LOG(LogHologramOpacity, Display, TEXT("      instance %d instanced %d in %s (owner %s, %d instances, %d floats) mesh %s at %s scale %s"),
				Index, Handle->IsInstanced() ? 1 : 0, Component ? *Component->GetName() : TEXT("None"),
				Component && Component->GetOwner() ? *Component->GetOwner()->GetName() : TEXT("None"), Component ? Component->GetInstanceCount() : -1,
				Component ? Component->NumCustomDataFloats : -1, Component && Component->GetStaticMesh() ? *Component->GetStaticMesh()->GetName() : TEXT("None"),
				bAtIndex ? *AtIndex.GetLocation().ToCompactString() : TEXT("-"), bAtIndex ? *AtIndex.GetScale3D().ToCompactString() : TEXT("-"));
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Console commands (debug / tuning). Usable from the in-game console.
// ---------------------------------------------------------------------------------------------------------------------

static FAutoConsoleCommand GHoloOpacitySetCmd(
	TEXT("HoloOpacity.Set"),
	HologramOpacity::SetUsage,
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		float Opacity = 0.f;
		if (!HologramOpacity::ParseArgument(Args, 0, Opacity, HologramOpacity::SetUsage))
		{
			return;
		}
		// A fraction, not a percentage: 50 would only clamp to 100%
		if (Opacity > 1.f)
		{
			HologramOpacity::LogUsage(HologramOpacity::SetUsage);
			return;
		}
		if (UHologramOpacitySubsystem* Subsystem = HologramOpacity::CommandTarget())
		{
			Subsystem->SetDebugOpacityOverride(Opacity < 0.f ? -1.f : Opacity);
		}
	}));

static FAutoConsoleCommand GHoloOpacityScanlinesCmd(
	TEXT("HoloOpacity.Scanlines"),
	HologramOpacity::ScanlinesUsage,
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		int32 Value = 0;
		if (!HologramOpacity::ParseArgument(Args, 0, Value, HologramOpacity::ScanlinesUsage))
		{
			return;
		}
		if (UHologramOpacitySubsystem* Subsystem = HologramOpacity::CommandTarget())
		{
			Subsystem->SetDebugScanlinesOverride(Value);
		}
	}));

static FAutoConsoleCommand GHoloOpacityParamCmd(
	TEXT("HoloOpacity.Param"),
	HologramOpacity::ParamUsage,
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		float Value = 0.f;
		if (!HologramOpacity::ParseArgument(Args, 1, Value, HologramOpacity::ParamUsage))
		{
			return;
		}
		if (UHologramOpacitySubsystem* Subsystem = HologramOpacity::CommandTarget())
		{
			Subsystem->SetCompositeParameter(FName(*Args[0]), Value);
		}
	}));

static FAutoConsoleCommand GHoloOpacityDumpCmd(
	TEXT("HoloOpacity.Dump"),
	TEXT("HoloOpacity.Dump  Log the settings, the composite, every hologram mesh and every dismantle highlight."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (UHologramOpacitySubsystem* Subsystem = HologramOpacity::CommandTarget())
		{
			Subsystem->DumpState();
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

void UHologramOpacitySubsystem::FindSettings()
{
	UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings();
	const bool bReady = GameUserSettings && GameUserSettings->HasInitialized();
	OpacitySetting = bReady ? GameUserSettings->FindUserSetting(SettingId) : nullptr;
	ScanlinesSetting = bReady ? GameUserSettings->FindUserSetting(ScanlinesSettingId) : nullptr;
}

float UHologramOpacitySubsystem::ReadOpacitySetting() const
{
	if (DebugOpacityOverride >= 0.f)
	{
		return FMath::Clamp(DebugOpacityOverride, 0.f, 1.f);
	}

	const UFGUserSettingApplyType* Setting = OpacitySetting.Get();
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

	const UFGUserSettingApplyType* Setting = ScanlinesSetting.Get();
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
	Colors[3] = GameUserSettings ? HologramOpacity::ToLinearColor(GameUserSettings->GetHologramDismantleColor()) : FLinearColor(1.f, 0.183f, 0.f);
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
		if (Material && HologramOpacity::IsBuildGunMaterial(Material))
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

void UHologramOpacitySubsystem::UpdateComposite(UWorld* World, const TArray<UMeshComponent*>& Bodies, int32 HologramState, const FHologramOpacityDismantleScan& Dismantle)
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

	TArray<UPrimitiveComponent*> DismantleDrawn;
	UpdateDismantle(World, Dismantle, DismantleDrawn);

	// One colour for everything drawn: the most severe hologram state, else the dismantle colour
	const int32 NewState = Bodies.Num() > 0 ? HologramState : DismantleDrawn.Num() > 0 ? HologramOpacity::DismantleStateIndex : StateIndex;
	if (NewState != StateIndex)
	{
		StateIndex = NewState;
		bParametersDirty = true;
	}

	USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
	Component->ShowOnlyComponents.Reset(Bodies.Num() + DismantleDrawn.Num());
	for (UMeshComponent* Mesh : Bodies)
	{
		Component->ShowOnlyComponents.Add(Mesh);
	}
	for (UPrimitiveComponent* Primitive : DismantleDrawn)
	{
		Component->ShowOnlyComponents.Add(Primitive);
	}
	const bool bDraw = Component->ShowOnlyComponents.Num() > 0;
	SetCompositeEnabled(bDraw);
	if (bDraw)
	{
		Component->CaptureSceneDeferred();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Dismantle highlights
// ---------------------------------------------------------------------------------------------------------------------

void UHologramOpacitySubsystem::ScanDismantle(UWorld* World, FHologramOpacityDismantleScan& Out)
{
	const UFGOutlineComponent* Outline = UFGOutlineComponent::Get(World);
	if (!Outline)
	{
		return;
	}

	// A dismantle highlight's proxies, and the meshes of its actor that it marks (each actor's meshes scanned once)
	TSet<const AActor*> Scanned;
	auto AddHighlight = [&Out, &Scanned](const FActorOutlineState& State, AActor* Actor)
	{
		for (const TPair<FName, TObjectPtr<UStaticMeshComponent>>& Pair : State.OutlineProxies)
		{
			if (IsValid(Pair.Value))
			{
				Out.Proxies.Add(Pair.Value);
			}
		}
		for (const TPair<FName, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : State.InstancedOutlineProxies)
		{
			if (IsValid(Pair.Value))
			{
				Out.Proxies.Add(Pair.Value);
			}
		}
		bool bAlreadyScanned = false;
		Scanned.Add(Actor, &bAlreadyScanned);
		if (bAlreadyScanned)
		{
			return;
		}
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (Primitive->IsRegistered() && Primitive->IsVisible() && Primitive->bRenderInMainPass && Primitive->bRenderCustomDepth
				&& Primitive->CustomDepthStencilValue == HologramOpacity::StencilDismantle)
			{
				Out.Meshes.Add(Primitive);
			}
		}
	};
	// Highlights of the proxies alone stay as the game draws them (which real meshes they stand for is unknown)
	auto DismantleActor = [](const FActorOutlineState& State) -> AActor*
	{
		AActor* Actor = State.Actor;
		return State.OutlineColor == EOutlineColor::OC_DISMANTLE && IsValid(Actor) && !State.OnlyHighlightProxies ? Actor : nullptr;
	};

	// Whole actors: their meshes, and below their shared instances and fog planes
	if (const TArray<FActorOutlineState>* States = HologramOpacity::ActorOutlineStates(Outline))
	{
		for (const FActorOutlineState& State : *States)
		{
			if (AActor* Actor = DismantleActor(State))
			{
				Out.Actors.Add(Actor);
				AddHighlight(State, Actor);
			}
		}
	}
	// Single meshes of an actor: just the marked mesh; the actor's shared instances and fog planes aren't highlighted
	if (const TArray<FActorOutlineState>* States = HologramOpacity::ComponentOutlineStates(Outline))
	{
		for (const FActorOutlineState& State : *States)
		{
			if (AActor* Actor = DismantleActor(State))
			{
				AddHighlight(State, Actor);
			}
		}
	}

	// The black fog planes inside the buildings' openings are pooled meshes of the pool root, placed at the building's
	// fog plane transforms: they go with the building
	struct FFogPlane
	{
		FVector Location;
		const UStaticMesh* Mesh;
	};
	TArray<FFogPlane> FogPlanes;
	TSet<const UStaticMesh*> FogPlaneMeshes;
	for (const AActor* Actor : Out.Actors)
	{
		const AFGBuildable* Buildable = Cast<AFGBuildable>(Actor);
		const UFGBuildableSparseDataObject* SparseData = Buildable ? Buildable->GetBuildableSparseData() : nullptr;
		if (!SparseData || !SparseData->FogPlaneMesh)
		{
			continue;
		}
		FogPlaneMeshes.Add(SparseData->FogPlaneMesh);
		for (const FTransform& Relative : SparseData->mFogPlaneLocations)
		{
			FogPlanes.Add({(Relative * Actor->GetActorTransform()).GetLocation(), SparseData->FogPlaneMesh});
		}
	}
	if (FogPlanes.Num() == 0)
	{
		return;
	}
	// Only pooled meshes of a fog plane mesh: other pooled meshes (lights, signs) can sit just as close
	TArray<UStaticMeshComponent*> Pooled;
	for (TActorIterator<APoolRoot> It(World); It; ++It)
	{
		for (UActorComponent* Component : It->Components)
		{
			UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component);
			if (IsValid(Mesh) && FogPlaneMeshes.Contains(Mesh->GetStaticMesh()) && Mesh->IsRegistered() && Mesh->IsVisible())
			{
				Pooled.Add(Mesh);
			}
		}
	}
	// Each fog plane is the nearest pooled mesh of its mesh, if one is close enough
	for (const FFogPlane& FogPlane : FogPlanes)
	{
		UStaticMeshComponent* Nearest = nullptr;
		double NearestDistance = FMath::Square(HologramOpacity::FogPlaneTolerance);
		for (UStaticMeshComponent* Mesh : Pooled)
		{
			const double Distance = FVector::DistSquared(Mesh->GetComponentLocation(), FogPlane.Location);
			if (Distance < NearestDistance && Mesh->GetStaticMesh() == FogPlane.Mesh)
			{
				Nearest = Mesh;
				NearestDistance = Distance;
			}
		}
		if (Nearest)
		{
			Out.Meshes.AddUnique(Nearest);
		}
	}
}

void UHologramOpacitySubsystem::UpdateDismantle(UWorld* World, const FHologramOpacityDismantleScan& Dismantle, TArray<UPrimitiveComponent*>& OutDrawn)
{
	// Shared instances: hidden while their actor is highlighted (the capture draws copies)
	bool bRemoved = false;
	const TSet<AActor*> Highlighted(Dismantle.Actors);
	for (auto It = DismantleActors.CreateIterator(); It; ++It)
	{
		if (!Highlighted.Contains(It.Key().Get()))
		{
			ShowInstances(It.Value());
			It.RemoveCurrent();
			bRemoved = true;
		}
	}
	TArray<AActor*> Added;
	for (AActor* Actor : Dismantle.Actors)
	{
		if (!DismantleActors.Contains(Actor))
		{
			DismantleActors.Add(Actor, HideInstances(Actor));
			Added.Add(Actor);
		}
	}
	bool bCopiesValid = true;
	for (const TPair<TWeakObjectPtr<UInstancedStaticMeshComponent>, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceCopies)
	{
		const UInstancedStaticMeshComponent* Copy = Pair.Value.Get();
		bCopiesValid &= Copy && Copy->GetOwner() == Capture.Get() && Pair.Key.IsValid();
	}
	// Newly highlighted actors (a growing mass-dismantle selection) only add their instances; anything else recreates
	// them all
	if (bRemoved || !bCopiesValid || bRefreshInstanceCopies)
	{
		UpdateInstanceCopies(nullptr);
		bRefreshInstanceCopies = false;
	}
	else if (Added.Num() > 0)
	{
		UpdateInstanceCopies(&Added);
	}

	// The buildings' own meshes: drawn by the capture, and still casting their shadow in the main view
	const TSet<UPrimitiveComponent*> Meshes(Dismantle.Meshes);
	for (auto It = ShadowFlags.CreateIterator(); It; ++It)
	{
		UPrimitiveComponent* Mesh = It.Key().Get();
		if (!Mesh || !Meshes.Contains(Mesh))
		{
			if (Mesh)
			{
				Mesh->SetCastHiddenShadow(It.Value());
			}
			It.RemoveCurrent();
		}
	}
	for (UPrimitiveComponent* Mesh : Dismantle.Meshes)
	{
		if (!ShadowFlags.Contains(Mesh))
		{
			ShadowFlags.Add(Mesh, static_cast<bool>(Mesh->bCastHiddenShadow));
			Mesh->SetCastHiddenShadow(true);
		}
	}

	OutDrawn = Dismantle.Meshes;
	for (const TPair<TWeakObjectPtr<UInstancedStaticMeshComponent>, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceCopies)
	{
		if (UInstancedStaticMeshComponent* Copy = Pair.Value.Get(); Copy && Copy->GetInstanceCount() > 0)
		{
			OutDrawn.Add(Copy);
		}
	}

	// Out of the main view: what the capture draws, and the outline proxies, whose custom depth makes the game's own
	// highlight
	TArray<UPrimitiveComponent*> Hidden = OutDrawn;
	Hidden.Append(Dismantle.Proxies);
	SetMainViewHidden(World, Hidden);
}

TArray<FHologramOpacityHiddenInstance> UHologramOpacitySubsystem::HideInstances(AActor* Actor)
{
	TArray<FHologramOpacityHiddenInstance> Hidden;

	// Abstract instances (lightweight buildables and building parts), hidden with the game's own call through the
	// building's ownership handles (static and dynamic)
	if (const AFGBuildable* Buildable = Cast<AFGBuildable>(Actor))
	{
		for (const FInstanceOwnerHandlePtr& Handle : Buildable->mInstanceHandles)
		{
			if (!Handle.IsValid() || !Handle->IsInstanced())
			{
				continue;
			}
			UInstancedStaticMeshComponent* Source = Handle->GetInstanceComponent();
			FTransform Transform;
			// Leave instances the game has hidden itself
			if (!Source || !Source->GetInstanceTransform(static_cast<int32>(Handle->GetHandleID()), Transform, false) || HologramOpacity::IsHiddenScale(Transform))
			{
				continue;
			}
			Handle->HideInstance(true);
			FHologramOpacityHiddenInstance& Entry = Hidden.AddDefaulted_GetRef();
			Entry.Handle = Handle;
			Entry.Source = Source;
		}
	}

	// Factory meshes drawn by their coloured instance manager (production indicators; whole buildings without Lumen)
	TInlineComponentArray<UFGColoredInstanceMeshProxy*> Meshes(Actor);
	for (UFGColoredInstanceMeshProxy* Mesh : Meshes)
	{
		UInstancedStaticMeshComponent* Source = Mesh->IsRegistered() && Mesh->mInstanceHandle.IsInstanced() ? HologramOpacity::ColoredInstanceComponent(Mesh) : nullptr;
		const int32 Index = Mesh->mInstanceHandle.GetHandleID();
		FTransform Transform;
		if (!Source || !Source->GetInstanceTransform(Index, Transform, false) || HologramOpacity::IsHiddenScale(Transform))
		{
			continue;
		}
		FTransform Shrunk = Transform;
		Shrunk.SetScale3D(FVector(HologramOpacity::OwnHiddenScale));
		Source->UpdateInstanceTransform(Index, Shrunk, false, true, true);
		FHologramOpacityHiddenInstance& Entry = Hidden.AddDefaulted_GetRef();
		Entry.ColoredMesh = Mesh;
		Entry.Source = Source;
		Entry.OriginalTransform = Transform;
	}
	return Hidden;
}

void UHologramOpacitySubsystem::ShowInstances(const TArray<FHologramOpacityHiddenInstance>& Instances)
{
	for (const FHologramOpacityHiddenInstance& Entry : Instances)
	{
		if (Entry.Handle.IsValid())
		{
			// Does nothing once the instance is removed or shown again. (The handle has no record of who hid it, so an
			// instance the game hid as well while it was highlighted is shown too; leaving one hidden for good is worse.)
			Entry.Handle->UnHideInstance(true);
		}
		else if (const UFGColoredInstanceMeshProxy* Mesh = Entry.ColoredMesh.Get())
		{
			// Only while it is still the same instance, still shrunk by this mod (not reset or hidden by the game since)
			UInstancedStaticMeshComponent* Source = Entry.Source.Get();
			const int32 Index = Mesh->mInstanceHandle.GetHandleID();
			FTransform Transform;
			if (Source && Mesh->mInstanceHandle.IsInstanced() && HologramOpacity::ColoredInstanceComponent(Mesh) == Source
				&& Source->GetInstanceTransform(Index, Transform, false) && HologramOpacity::IsOwnHiddenScale(Transform))
			{
				Source->UpdateInstanceTransform(Index, Entry.OriginalTransform, false, true, true);
			}
		}
	}
}

void UHologramOpacitySubsystem::UpdateInstanceCopies(const TArray<AActor*>* Added)
{
	struct FCopiedInstance
	{
		FTransform Transform;
		TArray<float> CustomData;
	};

	// The hidden instances to copy, by the component that draws them, with their paint and other per-instance data
	TMap<UInstancedStaticMeshComponent*, TArray<FCopiedInstance>> BySource;
	auto Collect = [&BySource](const TArray<FHologramOpacityHiddenInstance>& Entries)
	{
		for (const FHologramOpacityHiddenInstance& Entry : Entries)
		{
			UInstancedStaticMeshComponent* Source = Entry.Source.Get();
			int32 Index = INDEX_NONE;
			FTransform Transform;
			if (!Source)
			{
				continue;
			}
			if (Entry.Handle.IsValid())
			{
				if (!Entry.Handle->IsInstanced() || Entry.Handle->GetInstanceComponent() != Source)
				{
					continue;
				}
				Index = static_cast<int32>(Entry.Handle->GetHandleID());
				// Where it really is (the hidden instance is shrunk and moved away)
				Transform = Entry.Handle->GetWorldTransform();
			}
			else if (const UFGColoredInstanceMeshProxy* Mesh = Entry.ColoredMesh.Get(); Mesh && Mesh->mInstanceHandle.IsInstanced())
			{
				Index = Mesh->mInstanceHandle.GetHandleID();
				// Not once the game has reset or hidden it itself: the world draws (or hides) it again
				FTransform Current;
				if (!Source->GetInstanceTransform(Index, Current, false) || !HologramOpacity::IsOwnHiddenScale(Current))
				{
					continue;
				}
				Transform = Entry.OriginalTransform * Source->GetComponentTransform();
			}
			else
			{
				continue;
			}
			FCopiedInstance& Copied = BySource.FindOrAdd(Source).AddDefaulted_GetRef();
			Copied.Transform = Transform;
			const int32 Floats = Source->NumCustomDataFloats;
			if (Floats > 0 && Index >= 0 && Source->PerInstanceSMCustomData.IsValidIndex((Index + 1) * Floats - 1))
			{
				Copied.CustomData = TArray<float>(Source->PerInstanceSMCustomData.GetData() + Index * Floats, Floats);
			}
		}
	};
	if (Added)
	{
		for (AActor* Actor : *Added)
		{
			if (const TArray<FHologramOpacityHiddenInstance>* Entries = DismantleActors.Find(Actor))
			{
				Collect(*Entries);
			}
		}
	}
	else
	{
		for (const TPair<TWeakObjectPtr<AActor>, TArray<FHologramOpacityHiddenInstance>>& Pair : DismantleActors)
		{
			Collect(Pair.Value);
		}
	}

	// On a full update, copies whose source has nothing hidden any more, or that belong to an old capture actor, go away
	AActor* Owner = Capture.Get();
	if (!Added)
	{
		for (auto It = InstanceCopies.CreateIterator(); It; ++It)
		{
			UInstancedStaticMeshComponent* Copy = It.Value().Get();
			if (!Copy || !Owner || Copy->GetOwner() != Owner || !BySource.Contains(It.Key().Get()))
			{
				if (Copy)
				{
					Copy->DestroyComponent();
				}
				It.RemoveCurrent();
			}
		}
	}
	if (!Owner)
	{
		return;
	}

	for (const TPair<UInstancedStaticMeshComponent*, TArray<FCopiedInstance>>& Pair : BySource)
	{
		UInstancedStaticMeshComponent* Copy = nullptr;
		if (const TWeakObjectPtr<UInstancedStaticMeshComponent>* Existing = InstanceCopies.Find(Pair.Key))
		{
			Copy = Existing->Get();
		}
		if (!Copy)
		{
			// Kept alive by its owner, the capture actor
			Copy = HologramOpacity::CreateInstanceCopy(Pair.Key, Owner);
			InstanceCopies.Add(Pair.Key, Copy);
		}
		else if (!Added)
		{
			Copy->ClearInstances();
		}
		for (const FCopiedInstance& Copied : Pair.Value)
		{
			const int32 NewIndex = Copy->AddInstance(Copied.Transform, true);
			if (Copied.CustomData.Num() > 0 && Copied.CustomData.Num() == Copy->NumCustomDataFloats)
			{
				Copy->SetCustomData(NewIndex, Copied.CustomData, false);
			}
		}
		Copy->MarkRenderStateDirty();
	}
}

void UHologramOpacitySubsystem::SetMainViewHidden(UWorld* World, const TArray<UPrimitiveComponent*>& Components)
{
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	if (Controller == MainViewController.Get() && Components.Num() == MainViewHidden.Num())
	{
		bool bSame = true;
		for (int32 Index = 0; Index < Components.Num() && bSame; ++Index)
		{
			bSame = MainViewHidden[Index].Get() == Components[Index];
		}
		if (bSame)
		{
			return;
		}
	}

	if (APlayerController* Previous = MainViewController.Get(); Previous && MainViewHidden.Num() > 0)
	{
		const TSet<TWeakObjectPtr<UPrimitiveComponent>> Ours(MainViewHidden);
		Previous->HiddenPrimitiveComponents.RemoveAll([&Ours](const TWeakObjectPtr<UPrimitiveComponent>& Component)
		{
			return Ours.Contains(Component);
		});
	}
	MainViewHidden.Reset();
	MainViewController = Controller;
	if (Controller)
	{
		for (UPrimitiveComponent* Component : Components)
		{
			Controller->HiddenPrimitiveComponents.Add(Component);
			MainViewHidden.Add(Component);
		}
	}
}

void UHologramOpacitySubsystem::StopDismantle()
{
	for (const TPair<TWeakObjectPtr<AActor>, TArray<FHologramOpacityHiddenInstance>>& Pair : DismantleActors)
	{
		ShowInstances(Pair.Value);
	}
	DismantleActors.Reset();
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, bool>& Pair : ShadowFlags)
	{
		if (UPrimitiveComponent* Mesh = Pair.Key.Get())
		{
			Mesh->SetCastHiddenShadow(Pair.Value);
		}
	}
	ShadowFlags.Reset();
	for (const TPair<TWeakObjectPtr<UInstancedStaticMeshComponent>, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceCopies)
	{
		if (UInstancedStaticMeshComponent* Copy = Pair.Value.Get())
		{
			Copy->DestroyComponent();
		}
	}
	InstanceCopies.Reset();
	bRefreshInstanceCopies = false;
	SetMainViewHidden(nullptr, TArray<UPrimitiveComponent*>());
}

bool UHologramOpacitySubsystem::HasDismantleState() const
{
	return DismantleActors.Num() > 0 || ShadowFlags.Num() > 0 || InstanceCopies.Num() > 0 || MainViewHidden.Num() > 0;
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
	StopDismantle();
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
	CompositeMaterial->SetVectorParameterValue(TEXT("HoloColor"), Colors[FMath::Clamp(StateIndex, 0, 3)]);
	// Dismantle highlights have a contour, build holograms don't
	CompositeMaterial->SetScalarParameterValue(TEXT("EdgeIntensity"), StateIndex == HologramOpacity::DismantleStateIndex ? HologramOpacity::DismantleEdgeIntensity : 0.f);
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

	// Once a second: find the two settings again (their values are read every frame), pick up colour changes from the
	// Holograms options, and refresh the copies of hidden instances (a building repainted while highlighted)
	const double Now = FPlatformTime::Seconds();
	if (Now - LastSlowCheck > 1.0)
	{
		LastSlowCheck = Now;
		FindSettings();
		bRefreshInstanceCopies = InstanceCopies.Num() > 0;
		const FLinearColor Previous[4] = {Colors[0], Colors[1], Colors[2], Colors[3]};
		ReadColors();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			bParametersDirty |= !Previous[Index].Equals(Colors[Index]);
		}
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
		if (bCompositeEnabled || HiddenMeshes.Num() > 0 || HasDismantleState())
		{
			StopComposite();
		}
		return;
	}
	// The composite's content or actors are missing (logged once): holograms stay vanilla, so there is nothing to scan
	if (bCompositeFailed)
	{
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
			// Meshes already hidden were classified before; the full check reads every material's package name
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

	// Buildings highlighted in dismantle mode
	FHologramOpacityDismantleScan Dismantle;
	ScanDismantle(World, Dismantle);

	if (Bodies.Num() == 0 && HiddenMeshes.Num() == 0 && Dismantle.IsEmpty() && !HasDismantleState())
	{
		SetCompositeEnabled(false);
		return;
	}
	if (!EnsureComposite(World))
	{
		StopComposite();
		return;
	}

	UpdateComposite(World, Bodies, HologramState, Dismantle);
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
	FindSettings();
	struct FNamedSetting
	{
		const TCHAR* Id;
		const UFGUserSettingApplyType* Setting;
	};
	const FNamedSetting Settings[] = {{SettingId, OpacitySetting.Get()}, {ScanlinesSettingId, ScanlinesSetting.Get()}};
	for (const FNamedSetting& Named : Settings)
	{
		if (Named.Setting)
		{
			const FVariant Value = Named.Setting->GetDisplayValue();
			UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s: type %d, value '%s'"), Named.Id, static_cast<int32>(Value.GetType()), *UFGUserSettingApplyType::VariantAsString(Value));
		}
		else
		{
			UE_LOG(LogHologramOpacity, Display, TEXT("Setting %s NOT registered"), Named.Id);
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

	// Outlined actors (dismantle mode and other highlights)
	if (const UFGGameUserSettings* GameUserSettings = UFGGameUserSettings::GetFGGameUserSettings())
	{
		UE_LOG(LogHologramOpacity, Display, TEXT("Dismantle colour %s"), *GameUserSettings->GetHologramDismantleColor().ToString());
	}
	const UFGOutlineComponent* Outline = UFGOutlineComponent::Get(World);
	if (!Outline)
	{
		UE_LOG(LogHologramOpacity, Display, TEXT("No outline component"));
		return;
	}
	struct FNamedStates
	{
		const TCHAR* PropertyName;
		const TArray<FActorOutlineState>* States;
	};
	const FNamedStates OutlineLists[] = {
		{TEXT("mActorOutlineStates"), HologramOpacity::ActorOutlineStates(Outline)},
		{TEXT("mComponentSpecifiedActorOutlineStates"), HologramOpacity::ComponentOutlineStates(Outline)}};
	for (const FNamedStates& List : OutlineLists)
	{
		const TCHAR* PropertyName = List.PropertyName;
		const TArray<FActorOutlineState>* States = List.States;
		UE_LOG(LogHologramOpacity, Display, TEXT("Outline %s: %d states"), PropertyName, States ? States->Num() : -1);
		if (States)
		{
			for (const FActorOutlineState& State : *States)
			{
				HologramOpacity::DumpOutlineState(State);
			}
		}
	}

	// What this mod has changed for dismantle highlights
	int32 NumHidden = 0;
	for (const TPair<TWeakObjectPtr<AActor>, TArray<FHologramOpacityHiddenInstance>>& Pair : DismantleActors)
	{
		NumHidden += Pair.Value.Num();
	}
	UE_LOG(LogHologramOpacity, Display, TEXT("Dismantle: %d actors, %d hidden instances, %d own meshes drawn, %d copies, %d hidden from the main view (controller %s)"),
		DismantleActors.Num(), NumHidden, ShadowFlags.Num(), InstanceCopies.Num(), MainViewHidden.Num(), *HologramOpacity::Describe(MainViewController.Get()));
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, bool>& Pair : ShadowFlags)
	{
		HologramOpacity::DumpPrimitive(TEXT("drawn"), NAME_None, Pair.Key.Get());
	}
	for (const TPair<TWeakObjectPtr<UInstancedStaticMeshComponent>, TWeakObjectPtr<UInstancedStaticMeshComponent>>& Pair : InstanceCopies)
	{
		const UInstancedStaticMeshComponent* Source = Pair.Key.Get();
		const UInstancedStaticMeshComponent* Copy = Pair.Value.Get();
		UE_LOG(LogHologramOpacity, Display, TEXT("    copy %s of %s (%s): %d instances, %d floats, registered %d"), Copy ? *Copy->GetName() : TEXT("None"),
			Source ? *Source->GetName() : TEXT("None"), Source && Source->GetStaticMesh() ? *Source->GetStaticMesh()->GetName() : TEXT("None"),
			Copy ? Copy->GetInstanceCount() : -1, Copy ? Copy->NumCustomDataFloats : -1, Copy && Copy->IsRegistered() ? 1 : 0);
	}
}
