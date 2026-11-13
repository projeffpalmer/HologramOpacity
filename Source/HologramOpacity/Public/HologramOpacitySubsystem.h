#pragma once

#include "CoreMinimal.h"
#include "InstanceData.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "HologramOpacitySubsystem.generated.h"

class AActor;
class APlayerController;
class APostProcessVolume;
class ASceneCapture2D;
class UFGColoredInstanceMeshProxy;
class UFGUserSettingApplyType;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMeshComponent;
class UPrimitiveComponent;
class UTextureRenderTarget2D;
class UWorld;

/** What the game's dismantle highlight (outline colour OC_DISMANTLE) covers this frame */
struct FHologramOpacityDismantleScan
{
	/** Highlighted actors (whole-actor highlights only: their shared instances and fog planes go with them) */
	TArray<AActor*> Actors;
	/** Their own visible meshes, which the highlight marks with custom depth stencil 8 (for a highlight of one mesh of
	 *  an actor, just that mesh) */
	TArray<UPrimitiveComponent*> Meshes;
	/** The highlight's custom-depth-only copies of their instanced meshes (owned by the player) */
	TArray<UPrimitiveComponent*> Proxies;

	bool IsEmpty() const { return Actors.Num() == 0 && Meshes.Num() == 0 && Proxies.Num() == 0; }
};

/** One shared (instanced) mesh of a highlighted building, hidden in the world while the capture draws a copy */
struct FHologramOpacityHiddenInstance
{
	/** An abstract instance (lightweight buildables, building parts) of an AFGBuildable, hidden with the game's own call */
	FInstanceOwnerHandlePtr Handle;
	/** Or a factory mesh instanced by its coloured instance manager, hidden by shrinking it in place to a scale only this
	 *  mod uses, so it is only restored while it is still shrunk by this mod */
	TWeakObjectPtr<UFGColoredInstanceMeshProxy> ColoredMesh;
	/** The instanced mesh component that draws it */
	TWeakObjectPtr<UInstancedStaticMeshComponent> Source;
	/** The coloured instance's own transform, in Source's space */
	FTransform OriginalTransform = FTransform::Identity;
};

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
 * Dismantle mode highlights a real building the same way (custom depth stencil 8 on its own meshes, plus custom-depth-
 * only proxies of its instanced meshes, in the dismantle colour, with a contour). Below 100%, or without scan lines,
 * the building's own meshes and the proxies leave the player's view (its hidden component list, so their shadows
 * stay), its shared instances are hidden in place, and the capture draws the meshes and transient copies of those
 * instances (same mesh, materials, transform and per-instance paint data). Everything is restored when the
 * highlight ends or at 100% with scan lines on.
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
	/** Looks the two settings up by StrId (in Tick, once a second) */
	void FindSettings();
	float ReadOpacitySetting() const;
	bool ReadScanlinesSetting() const;
	bool IsVanilla() const { return CurrentOpacity >= 0.999f; }
	/** The mod draws holograms itself below 100%, and at 100% too when scan lines are turned off */
	bool WantsComposite() const { return !IsVanilla() || !bShowScanlines; }

	/** True for the meshes that make up the building preview (not guidelines, clearance boxes or arrows) */
	static bool IsHologramBodyMesh(const UMeshComponent* Mesh);

	/** Loads the material and render target, and spawns the capture and post process volume in this world */
	bool EnsureComposite(UWorld* World);
	/** Hides the current hologram bodies and dismantle highlights from the main view, feeds them to the capture, and
	 *  picks the state colour */
	void UpdateComposite(UWorld* World, const TArray<UMeshComponent*>& Bodies, int32 HologramState, const FHologramOpacityDismantleScan& Dismantle);
	/** Gives every hidden mesh its own flag back and switches the composite off */
	void StopComposite();
	void SetCompositeEnabled(bool bEnabled);
	void RefreshCompositeParameters();
	void ReadColors();

	/** Finds the dismantle highlight's actors, meshes and proxies */
	static void ScanDismantle(UWorld* World, FHologramOpacityDismantleScan& Out);
	/** Takes the highlighted buildings out of the main view; returns what the capture draws instead */
	void UpdateDismantle(UWorld* World, const FHologramOpacityDismantleScan& Dismantle, TArray<UPrimitiveComponent*>& OutDrawn);
	static TArray<FHologramOpacityHiddenInstance> HideInstances(AActor* Actor);
	static void ShowInstances(const TArray<FHologramOpacityHiddenInstance>& Instances);
	/** Updates the capture's copies of the hidden instances, one instanced component per source component. With Added,
	 *  only appends the instances of those newly highlighted actors; otherwise recreates every copy's instances. */
	void UpdateInstanceCopies(const TArray<AActor*>* Added);
	/** Sets the components this mod hides through the player controller's hidden component list */
	void SetMainViewHidden(UWorld* World, const TArray<UPrimitiveComponent*>& Components);
	/** Restores everything changed for dismantle highlights */
	void StopDismantle();
	bool HasDismantleState() const;

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

	/** Dismantle-highlighted actors and the shared instances hidden for each */
	TMap<TWeakObjectPtr<AActor>, TArray<FHologramOpacityHiddenInstance>> DismantleActors;
	/** Highlighted buildings' own meshes drawn by the capture, with their original "cast hidden shadow" flag */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, bool> ShadowFlags;
	/** The capture's copy of the hidden instances of each source instanced component (owned by the capture actor) */
	TMap<TWeakObjectPtr<UInstancedStaticMeshComponent>, TWeakObjectPtr<UInstancedStaticMeshComponent>> InstanceCopies;
	/** Set once a second while copies exist, so paint or other per-instance changes reach the copies */
	bool bRefreshInstanceCopies = false;
	/** Components this mod added to the player controller's hidden component list */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> MainViewHidden;
	TWeakObjectPtr<APlayerController> MainViewController;

	/** The two settings, found by StrId once a second; their values are read every frame */
	TWeakObjectPtr<UFGUserSettingApplyType> OpacitySetting;
	TWeakObjectPtr<UFGUserSettingApplyType> ScanlinesSetting;

	float CurrentOpacity = 1.f;
	float DebugOpacityOverride = -1.f;
	bool bShowScanlines = true;
	/** -1 = use the Options value, 0 = off, 1 = on */
	int32 DebugScanlinesOverride = -1;
	/** State the composite colours with: 0 valid, 1 soft clearance, 2 invalid hologram, 3 dismantle */
	int32 StateIndex = 0;
	FLinearColor Colors[4];
	bool bCompositeEnabled = false;
	bool bCompositeFailed = false;
	bool bInitialized = false;
	bool bParametersDirty = true;
	/** When Tick last looked the settings up and re-read the colours (FPlatformTime seconds) */
	double LastSlowCheck = -1000.0;
};
