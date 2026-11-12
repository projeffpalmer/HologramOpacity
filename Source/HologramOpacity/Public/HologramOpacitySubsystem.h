#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "HologramOpacitySubsystem.generated.h"

class APostProcessVolume;
class ASceneCapture2D;
class UMaterialInstanceDynamic;
class UMeshComponent;
class UTextureRenderTarget2D;
class UWorld;

/**
 * Applies the "Hologram Opacity" and "Show Scanlines" user settings (Options > User Interface > Holograms/Build Mode)
 * to build mode holograms.
 *
 * In Satisfactory 1.2 a hologram renders the building's own materials (textures, paint, decals, animated parts) and
 * writes a custom depth stencil value; the global post process PPI_OutlineHologram then recolours those pixels with
 * the hologram colour and scan lines. That result is opaque.
 *
 * Below 100%, or with scan lines off, the mod keeps the hologram's own rendering but takes it out of the main view: the
 * hologram body meshes are made visible to scene captures only, and a scene capture with the main view's camera,
 * resolution and jitter draws just those meshes, lit, into a render target (colour in RGB, depth in A). The post
 * process material M_HologramOpacityComposite draws the hologram from that target in the colour from the Holograms
 * options, with or without scan lines, and blends it over the scene at the slider's opacity, behind the world and the
 * build gun's helper markers where they are in front. Materials are never changed. At 100% with scan lines on the
 * meshes get their original flag back and the game draws the hologram itself.
 *
 * (A capture rendered inside the main renderer would be cheaper, but in this game it trips an engine assertion in the
 * Nanite base pass, so the capture runs as its own scene renderer.)
 */
UCLASS()
class HOLOGRAMOPACITY_API UHologramOpacitySubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** StrId of the FGUserSetting asset /HologramOpacity/Settings/US_HologramOpacity */
	static const TCHAR* SettingId;
	/** StrId of the FGUserSetting asset /HologramOpacity/Settings/US_HologramScanlines */
	static const TCHAR* ScanlinesSettingId;

	// Begin USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem

	// Begin FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual UWorld* GetTickableGameObjectWorld() const override;
	// End FTickableGameObject

	/** The subsystem of the running game instance, for console commands */
	static UHologramOpacitySubsystem* GetActive();

	/** Current effective opacity in [0, 1] */
	float GetOpacity() const { return CurrentOpacity; }

	/** Current effective "Show Scanlines" value */
	bool GetShowScanlines() const { return bShowScanlines; }

	// Debug / tuning, driven by the HoloOpacity.* console commands
	void SetDebugOpacityOverride(float InOpacity);
	void SetDebugScanlinesOverride(int32 InValue);
	void SetCompositeParameter(FName Parameter, float Value);
	void DumpState();

private:
	float ReadOpacitySetting() const;
	bool ReadScanlinesSetting() const;
	bool IsVanilla() const { return CurrentOpacity >= 0.999f; }
	/** The mod draws holograms itself below 100%, and at 100% too when scan lines are turned off */
	bool WantsComposite() const { return !IsVanilla() || !bShowScanlines; }

	/** True for the meshes that make up the building preview (not guidelines, clearance boxes or arrows) */
	static bool IsHologramBodyMesh(const UMeshComponent* Mesh);

	/** Loads the material and render target, and spawns the capture and post process volume in this world */
	bool EnsureComposite(UWorld* World);
	/** Hides the current hologram bodies from the main view, feeds them to the capture, and picks the state colour */
	void UpdateComposite(UWorld* World, const TArray<UMeshComponent*>& Bodies, int32 HologramState);
	/** Gives every hidden mesh its own flag back and switches the composite off */
	void StopComposite();
	void SetCompositeEnabled(bool bEnabled);
	void RefreshCompositeParameters();
	void ReadColors();

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CompositeMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> CaptureTarget = nullptr;

	/** Spawned (transient, never saved) in the current game world */
	TWeakObjectPtr<ASceneCapture2D> Capture;
	TWeakObjectPtr<APostProcessVolume> Volume;

	/** Hologram meshes hidden from the main view, with their original "visible in scene capture only" flag */
	TMap<TWeakObjectPtr<UMeshComponent>, bool> HiddenMeshes;

	/** Scalar values set with HoloOpacity.Param, kept when the material instance is recreated */
	TMap<FName, float> ParameterOverrides;

	float CurrentOpacity = 1.f;
	float DebugOpacityOverride = -1.f;
	bool bShowScanlines = true;
	/** -1 = use the Options value, 0 = off, 1 = on */
	int32 DebugScanlinesOverride = -1;
	/** Hologram state the composite colours with: 0 valid, 1 soft clearance, 2 invalid */
	int32 StateIndex = 0;
	FLinearColor Colors[3];
	bool bCompositeEnabled = false;
	bool bCompositeFailed = false;
	bool bInitialized = false;
	bool bParametersDirty = true;
	double LastColorCheck = -1000.0;
};
