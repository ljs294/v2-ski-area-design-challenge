using UnrealBuildTool;

public class SkiAreaDesignChallengeTarget : TargetRules
{
    public SkiAreaDesignChallengeTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        CppStandard = CppStandardVersion.Cpp20;
        WindowsPlatform.CompilerVersion = "14.44.35229";
        WindowsPlatform.WindowsSdkVersion = "10.0.26100.0";
        ExtraModuleNames.Add("SkiPresentation");
    }
}
