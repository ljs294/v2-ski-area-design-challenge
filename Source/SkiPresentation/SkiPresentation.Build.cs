using UnrealBuildTool;

public class SkiPresentation : ModuleRules
{
    public SkiPresentation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "SkiApplication", "SkiPreparation", "SkiTerrainRuntime", "Slate", "SlateCore", "WebBrowser", "WebBrowserWidget"
        });
    }
}
