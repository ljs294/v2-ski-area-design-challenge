using UnrealBuildTool;

public class SkiAreaDesignChallengeEditorTarget : TargetRules
{
    public SkiAreaDesignChallengeEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        CppStandard = CppStandardVersion.Cpp20;
        WindowsPlatform.CompilerVersion = "14.44.35229";
        WindowsPlatform.WindowsSdkVersion = "10.0.26100.0";
        ExtraModuleNames.AddRange(new[] { "SkiPresentation", "SkiEditor" });
    }
}
