using UnrealBuildTool;

public class SkiPreparation : ModuleRules
{
    public SkiPreparation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "SkiApplication", "SkiDomain" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "HTTP", "Json", "JsonUtilities", "ImageCore", "ImageWrapper", "LibTiff", "Projects",
            "PROJ", "SQLiteCore", "SSL"
        });
        AddEngineThirdPartyPrivateStaticDependencies(Target, "libcurl", "nghttp2", "OpenSSL", "LibJpegTurbo", "zlib");
    }
}
