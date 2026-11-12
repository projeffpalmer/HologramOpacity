#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "HologramOpacitySubsystem.generated.h"

class AFGHologram;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UMeshComponent;
class UWorld;

/** One scalar parameter that follows the opacity slider: lerp(ValueAtZero, ValueAtOne, Opacity). */
USTRUCT()
struct FHologramOpacityScalarRule
{
	GENERATED_BODY()

	UPROPERTY()
	FName Parameter;

	/** Value used at 0% opacity. */
	UPROPERTY()
	float ValueAtZero = 0.f;

	/** Value used at 100% opacity. Read from the source material when the rule is created. */
	UPROPERTY()
	float ValueAtOne = 1.f;
};

/** A dynamic material instance owned by the mod. */
USTRUCT()
struct FHologramOpacityMaterial
{
	GENERATED_BODY()

	/** Key used by the console commands: pp, custom_ok, custom_warn, custom_error */
	UPROPERTY()
	FString Key;

	/** The vanilla material this replaces (the hologram post process for "pp"), or null */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Original = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> Dynamic = nullptr;

	UPROPERTY()
	TArray<FHologramOpacityScalarRule> Rules;
};

/** What the mod changed on one hologram mesh component, so it can be put back exactly. */
struct FHologramOpacitySavedMesh
{
	TArray<TWeakObjectPtr<UMaterialInterface>> Materials;
	bool bRenderCustomDepth = false;
	bool bRenderInMainPass = true;
	int32 Stencil = 0;
};

/**
 * Applies the "Hologram Opacity" user setting (Options > User Interface > Holograms/Build Mode) to build mode holograms.
 *
 * In Satisfactory 1.2 a hologram renders the building's own (opaque) materials and writes a custom depth stencil
 * value (5 valid, 3 soft clearance, 6 invalid); the global post process PPI_OutlineHologram then replaces those
 * pixels with the hologram colour and scan lines. That replacement is opaque, so below 100% the mod draws the
 * hologram body itself: the building materials are swapped for a translucent hologram material from this mod (in
 * the colours from the Holograms options) and custom depth is turned off on those meshes so the post process
 * leaves them alone. At 100% everything is restored and the hologram is vanilla.
 *
 * "Show Scanlines" (checkbox under the slider): when unchecked, the body material's scan lines are switched off,
 * and because the vanilla scan lines at 100% come from the post process, the mod then draws the hologram body at
 * 100% too (fully opaque, same colour, no lines).
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
	void SetMode(int32 InMode);
	void SetStencilOverrides(int32 Ok, int32 Warn, int32 Error);
	void SetScalarRule(const FString& Key, FName Parameter, float ValueAtZero);
	void SetScalarDirect(const FString& Key, FName Parameter, float Value);
	void DumpState();

private:
	enum EMode : int32
	{
		/** Leave the hologram meshes alone (only post process rules apply) */
		Mode_Vanilla = 0,
		/** Translucent body, post process still drawn over it */
		Mode_TranslucentWithPostProcess = 1,
		/** Translucent body, post process skipped (true see-through), default */
		Mode_Translucent = 2,
		/** Body not drawn in the main pass, post process only (ghost) */
		Mode_Ghost = 3,
	};

	float ReadOpacitySetting() const;
	bool ReadScanlinesSetting() const;
	bool EnsureMaterials();
	FHologramOpacityMaterial* FindEntry(const FString& Key);
	const FHologramOpacityMaterial* FindEntry(const FString& Key) const;
	FHologramOpacityMaterial& AddEntry(const FString& Key, UMaterialInterface* Original, UMaterialInterface* Parent);
	void AddRule(FHologramOpacityMaterial& Entry, FName Parameter, float ValueAtZero);
	void RefreshDynamicParameters();
	bool IsVanilla() const { return CurrentOpacity >= 0.999f; }
	/** The mod draws hologram bodies below 100%, and at 100% too when scan lines are turned off */
	bool WantsMeshChanges() const { return Mode != Mode_Vanilla && (!IsVanilla() || !bShowScanlines); }

	/** True for the meshes that make up the building preview (not guidelines, clearance boxes or arrows). */
	static bool IsHologramBodyMesh(const UMeshComponent* Mesh);
	UMaterialInterface* GetBodyMaterial(uint8 HologramMaterialState) const;
	void ApplyToHologram(AFGHologram* Hologram);
	void ApplyToHolograms(UWorld* World);
	void RestoreAllMeshes();
	void ApplyToPostProcess(UWorld* World);

	UPROPERTY(Transient)
	TArray<FHologramOpacityMaterial> Materials;

	/** Hologram meshes the mod has changed, with what they looked like before */
	TMap<TWeakObjectPtr<UMeshComponent>, FHologramOpacitySavedMesh> ModifiedMeshes;

	float CurrentOpacity = 1.f;
	float DebugOpacityOverride = -1.f;
	bool bShowScanlines = true;
	/** -1 = use the Options value, 0 = off, 1 = on */
	int32 DebugScanlinesOverride = -1;
	/** Scan line defaults read from M_HologramOpacity (restored when lines are turned back on) */
	float DefaultBaseIntensity = 0.85f;
	float DefaultLineIntensity = 0.18f;
	float DefaultLineOpacityBoost = 0.1f;
	float DefaultLineDuty = 0.5f;
	int32 Mode = Mode_Translucent;
	/** Debug: custom depth stencil values to force on body meshes per state (ok, warning, error); -1 = leave */
	int32 StencilOverride[3] = {-1, -1, -1};
	FLinearColor LastColors[3];
	bool bMaterialsReady = false;
	bool bMaterialsFailed = false;
	bool bInitialized = false;
	bool bForceRefresh = true;
	bool bLastApplied = false;
	double LastSlowCheck = -1000.0;
};
