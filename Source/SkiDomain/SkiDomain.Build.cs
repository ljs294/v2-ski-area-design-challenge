using System;
using System.IO;
using System.Linq;
using EpicGames.Core;
using UnrealBuildTool;

public class SkiDomain : ModuleRules
{
    public SkiDomain(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.NoPCHs;
        bUseUnity = false;
        PrivateDependencyModuleNames.Add("Core"); // Registration shim only.
        if (Target.LinkType == TargetLinkType.Modular)
        {
            PublicDefinitions.Add("SKI_DOMAIN_SHARED=1");
            PrivateDefinitions.Add("SKI_DOMAIN_EXPORTS=1");
        }

        string Root = Path.GetFullPath(Path.Combine(ModuleDirectory, "../.."));
        string Manifest = Path.Combine(Root, "Tools/Build/domain-sources.json");
        ExternalDependencies.Add(Manifest);
        JsonObject Document = JsonObject.Read(new FileReference(Manifest));
        string[] Listed = Document.GetStringArrayField("sources")
            .Select(Item => Path.GetFullPath(Path.Combine(Root, Item)))
            .OrderBy(Item => Item, StringComparer.OrdinalIgnoreCase).ToArray();
        string[] Discovered = Directory.GetFiles(Path.Combine(ModuleDirectory, "Private"), "*.cpp", SearchOption.AllDirectories)
            .Where(Item => Path.GetFileName(Item) != "SkiDomainModule.cpp")
            .Select(Path.GetFullPath).OrderBy(Item => Item, StringComparer.OrdinalIgnoreCase).ToArray();
        if (!Listed.SequenceEqual(Discovered, StringComparer.OrdinalIgnoreCase))
        {
            throw new BuildException("SkiDomain sources differ from the standalone source manifest.");
        }
    }
}
