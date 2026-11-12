using UnrealBuildTool;

public class HologramOpacity : ModuleRules
{
	public HologramOpacity(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies (same set as the Alpakit C++ template)
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			"GeometryCollectionEngine",
			"AnimGraphRuntime",
			"AssetRegistry",
			"NavigationSystem",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			"NetCore",
			"GameplayTags",
			"Json", "JsonUtilities"
		});

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {"AnimGraph"});
		}

		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});

		// Abstract (shared, instanced) building meshes, for dismantle highlights
		PrivateDependencyModuleNames.AddRange(new string[] {"AbstractInstance"});

		// Editor-only asset authoring helpers (HologramOpacityEditorLibrary)
		if (Target.bBuildEditor) {
			PrivateDependencyModuleNames.AddRange(new string[] {"GameFeatures"});
		}
	}
}
