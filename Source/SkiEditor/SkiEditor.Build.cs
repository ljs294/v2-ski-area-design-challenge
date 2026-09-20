using UnrealBuildTool;

public class SkiEditor : ModuleRules
{
    public SkiEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        if (!Target.bBuildEditor)
        {
            throw new BuildException("SkiEditor must never be linked into a player target.");
        }
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "UnrealEd" });
    }
}
