using UnrealBuildTool;

public class SkiTerrainRuntime : ModuleRules
{
    public SkiTerrainRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "GeometryCore", "GeometryFramework",
            "SkiApplication", "SkiDomain"
        });
    }
}
