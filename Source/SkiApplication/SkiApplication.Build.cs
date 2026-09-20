using UnrealBuildTool;

public class SkiApplication : ModuleRules
{
    public SkiApplication(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "SkiDomain" });
    }
}
