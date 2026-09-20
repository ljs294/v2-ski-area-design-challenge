using UnrealBuildTool;

public class SkiApplication : ModuleRules
{
    public SkiApplication(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "SkiDomain" });
    }
}
