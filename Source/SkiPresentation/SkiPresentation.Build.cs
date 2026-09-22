using UnrealBuildTool;

public class SkiPresentation : ModuleRules
{
    public SkiPresentation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG", "InputCore" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "SkiApplication", "SkiDomain", "SkiPreparation", "SkiTerrainRuntime", "ApplicationCore", "Slate", "SlateCore", "WebBrowser", "WebBrowserWidget"
        });
    }
}
