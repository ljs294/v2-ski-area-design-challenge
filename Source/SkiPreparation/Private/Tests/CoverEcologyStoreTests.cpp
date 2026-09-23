#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainCorePackageStore.h"

namespace
{
FString CoverTempRoot(const TCHAR* Name)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

SkiDomain::CoverEcologyManifest CoverManifest()
{
    SkiDomain::CoverEcologyManifest Manifest;
    Manifest.GeneratorVersion = "cover-store-test-v1";
    Manifest.CoverRevision = 3;
    Manifest.Source = {"worldcover-2021", "ESA WorldCover analytical COG", "2021",
        "worldcover-v200-cog", "CC BY 4.0", "ESA WorldCover project"};
    Manifest.Transform.Width = 4;
    Manifest.Transform.Height = 3;
    Manifest.Transform.LongitudeStepDeg = 0.01;
    Manifest.Transform.LatitudeStepDeg = 0.02;
    Manifest.Transform.SampleCenterBounds = {-71.3, 44.2, -71.27, 44.24};
    SkiDomain::ComputeCoverEcologyOuterBounds(Manifest.Transform.Width,
        Manifest.Transform.Height, Manifest.Transform.LongitudeStepDeg,
        Manifest.Transform.LatitudeStepDeg, Manifest.Transform.SampleCenterBounds,
        Manifest.Transform.OuterBounds);
    return Manifest;
}

bool ActivateCore(const FString& Root, SkiDomain::TerrainCoreManifest& Out, FString& Error)
{
    SkiDomain::Heightfield Field;
    Field.Width = 4;
    Field.Height = 3;
    Field.WestM = 0.0;
    Field.NorthM = 4.0;
    Field.EastSpacingM = 1.0;
    Field.NorthSpacingM = 2.0;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "cover-composite-test-v1";
    Manifest.ProcessingVersions = {"terraincore-derivation-v1"};
    Manifest.Source.SourceId = "fixture-ground";
    Manifest.Source.Product = "synthetic bare earth";
    Manifest.Source.AcquisitionEpoch = "2026-09-22";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "WGS84";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "Mountain Planner fixture";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 2.0;
    Manifest.LocalOrigin = {44.22, -71.285, 1000.0};
    Manifest.Width = Field.Width;
    Manifest.Height = Field.Height;
    Manifest.DeliveredEastSpacingM = Field.EastSpacingM;
    Manifest.DeliveredNorthSpacingM = Field.NorthSpacingM;
    Manifest.SampleCenterBounds = {0.0, 0.0, 3.0, 4.0};
    SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
        Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    FString Directory;
    return SkiPreparation::TerrainCorePackageStore(Root).WriteAndActivate(
        Manifest, Field, Directory, Out, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoverEcologyContractAndStoreTest,
    "MountainPlanner.P1.Product.CoverEcology.ContractAndStore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoverEcologyContractAndStoreTest::RunTest(const FString& Parameters)
{
    const FString Root = CoverTempRoot(TEXT("CoverEcologyStore"));
    const TArray<uint8> Classes{10, 10, 20, 20, 30, 30, 40, 40, 50, 50, 60, 60};
    const TArray<uint8> Validity{0xff, 0x0f};
    SkiPreparation::CoverEcologyStore Store(Root);
    FString Directory, Error;
    SkiDomain::CoverEcologyManifest Written;
    TestTrue(*FString::Printf(TEXT("Independent cover activates: %s"), *Error),
        Store.WriteAndActivate(CoverManifest(), Classes, Validity,
            Directory, Written, Error));
    TestTrue(TEXT("Cover identity is content-addressed"),
        SkiDomain::IsTerrainCoreSha256(Written.ContentId));
    TArray<uint8> InvalidClasses = Classes;
    InvalidClasses[0] = 11;
    FString InvalidDirectory = TEXT("sentinel");
    SkiDomain::CoverEcologyManifest InvalidManifest = Written;
    TestFalse(TEXT("Unknown analytical class is rejected"), Store.WriteAndActivate(
        CoverManifest(), InvalidClasses, Validity, InvalidDirectory, InvalidManifest, Error));
    TArray<uint8> InvalidPadding = Validity;
    InvalidPadding.Last() |= 0x80;
    TestFalse(TEXT("Noncanonical validity padding is rejected"), Store.WriteAndActivate(
        CoverManifest(), Classes, InvalidPadding, InvalidDirectory, InvalidManifest, Error));
    SkiPreparation::CoverEcologyPackageIndex Index;
    const FString Id = UTF8_TO_TCHAR(Written.ContentId.c_str());
    TestTrue(TEXT("Cover reopens independently"), Store.Open(Id, Index, Error));
    TestTrue(TEXT("Cover verifies independently"), Store.Verify(Index, Error));
    TArray<uint8> LoadedClasses, LoadedValidity;
    TestTrue(TEXT("Cover channels load"),
        Store.ReadChannels(Index, LoadedClasses, LoadedValidity, Error));
    TestEqual(TEXT("Categorical classes preserved"), LoadedClasses, Classes);
    TestEqual(TEXT("Validity mask preserved"), LoadedValidity, Validity);
    TestEqual(TEXT("Outer transform preserved"), Index.Manifest.Transform.OuterBounds.WestDeg,
        Written.Transform.OuterBounds.WestDeg);

    auto StaleLease = MakeShared<SkiPreparation::PreparationOperationLease,
        ESPMode::ThreadSafe>(7, 9);
    StaleLease->Invalidate();
    FString RejectedDirectory = TEXT("sentinel");
    SkiDomain::CoverEcologyManifest Rejected = Written;
    TestFalse(TEXT("Stale lease cannot activate cover"), Store.WriteAndActivate(
        CoverManifest(), Classes, Validity, RejectedDirectory, Rejected, Error,
        StaleLease, 7, 9));
    TestTrue(TEXT("Rejected outputs are cleared"),
        RejectedDirectory.IsEmpty() && Rejected.ContentId.empty());

    const FString UndeclaredPath = FPaths::Combine(Directory, TEXT("undeclared.bin"));
    TestTrue(TEXT("Undeclared fixture writes"),
        FFileHelper::SaveArrayToFile(TArray<uint8>{1}, *UndeclaredPath));
    SkiPreparation::CoverEcologyPackageIndex HostileIndex;
    TestFalse(TEXT("Undeclared package files are rejected"),
        Store.Open(Id, HostileIndex, Error));
    TestTrue(TEXT("Undeclared fixture removes"), IFileManager::Get().Delete(*UndeclaredPath));

    const FString ManifestPath = FPaths::Combine(Directory, TEXT("manifest.json"));
    FString OriginalJson;
    TestTrue(TEXT("Manifest fixture loads"),
        FFileHelper::LoadFileToString(OriginalJson, *ManifestPath));
    const FString RewrittenJson = OriginalJson.Replace(TEXT("2021"), TEXT("2022"));
    TestTrue(TEXT("Manifest identity fixture changes"), RewrittenJson != OriginalJson);
    TestTrue(TEXT("Manifest identity fixture writes"), FFileHelper::SaveStringToFile(
        RewrittenJson, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestFalse(TEXT("Manifest content-ID mismatch is rejected"),
        Store.Open(Id, HostileIndex, Error));
    TestTrue(TEXT("Manifest fixture restores"), FFileHelper::SaveStringToFile(
        OriginalJson, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

    const FString ClassPath = FPaths::Combine(Directory, TEXT("channels/classes.u8"));
    TArray<uint8> Tampered = Classes;
    Tampered[0] ^= 0xff;
    TestTrue(TEXT("Tamper fixture writes"), FFileHelper::SaveArrayToFile(Tampered, *ClassPath));
    TestFalse(TEXT("Tampered cover hash is rejected"), Store.Open(Id, HostileIndex, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoverEcologyCompositeActivationTest,
    "MountainPlanner.P1.Product.CoverEcology.CompositeActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoverEcologyCompositeActivationTest::RunTest(const FString& Parameters)
{
    const FString Root = CoverTempRoot(TEXT("CoverEcologyComposite"));
    FString Error;
    SkiDomain::TerrainCoreManifest Core;
    const bool CoreReady = ActivateCore(Root, Core, Error);
    TestTrue(*FString::Printf(TEXT("TerrainCore prerequisite activates: %s"), *Error), CoreReady);
    if (!CoreReady)
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    const TArray<uint8> Classes{10, 10, 20, 20, 30, 30, 40, 40, 50, 50, 60, 60};
    const TArray<uint8> Validity{0xff, 0x0f};
    SkiDomain::CoverEcologyManifest Cover;
    FString CoverDirectory;
    TestTrue(TEXT("Cover prerequisite activates"),
        SkiPreparation::CoverEcologyStore(Root).WriteAndActivate(CoverManifest(),
            Classes, Validity, CoverDirectory, Cover, Error));

    SkiDomain::InstalledTerrainReceipt Input;
    Input.GeneratorVersion = "composite-test-v1";
    Input.TerrainCoreId = Core.ContentId;
    Input.CoverEcologyId = Cover.ContentId;
    Input.OptionalSources = {
        {"naip", "USDA NAIP RGBN", SkiDomain::OptionalSourceStatus::Unavailable,
            {}, "NO_COMPLETE_COVERAGE", "Public domain", "USDA"},
        {"overpass", "OpenStreetMap vector context",
            SkiDomain::OptionalSourceStatus::NotRequested, {}, "NOT_REQUESTED",
            "ODbL 1.0", "OpenStreetMap contributors"},
    };
    SkiPreparation::InstalledTerrainStore Store(Root);
    FString ReceiptDirectory;
    SkiDomain::InstalledTerrainReceipt Written;
    TestTrue(*FString::Printf(TEXT("Composite receipt activates after both reopen: %s"), *Error),
        Store.WriteAndActivate(Input, ReceiptDirectory, Written, Error));
    SkiPreparation::InstalledTerrainIndex Reopened;
    TestTrue(TEXT("Composite receipt reopens"), Store.Open(
        UTF8_TO_TCHAR(Written.ContentId.c_str()), Reopened, Error));
    TestEqual(TEXT("Composite references immutable TerrainCore"),
        Reopened.Receipt.TerrainCoreId, Core.ContentId);
    TestEqual(TEXT("Composite references independent cover"),
        Reopened.Receipt.CoverEcologyId, Cover.ContentId);
    TestTrue(TEXT("Unavailable optional source has no placeholder artifact"),
        Reopened.Receipt.OptionalSources[0].ArtifactId.empty());

    SkiDomain::InstalledTerrainReceipt Missing = Input;
    Missing.CoverEcologyId = std::string(64, 'a');
    FString RejectedDirectory = TEXT("sentinel");
    SkiDomain::InstalledTerrainReceipt Rejected = Written;
    TestFalse(TEXT("Composite cannot activate with a missing cover component"),
        Store.WriteAndActivate(Missing, RejectedDirectory, Rejected, Error));
    TestTrue(TEXT("Failed composite clears publication outputs"),
        RejectedDirectory.IsEmpty() && Rejected.ContentId.empty());

    auto Lease = MakeShared<SkiPreparation::PreparationOperationLease,
        ESPMode::ThreadSafe>(11, 12);
    Lease->Invalidate();
    TestFalse(TEXT("Stale lease cannot publish a composite"), Store.WriteAndActivate(
        Input, RejectedDirectory, Rejected, Error, Lease, 11, 12));

    const FString CoverClassPath = FPaths::Combine(CoverDirectory,
        TEXT("channels/classes.u8"));
    TArray<uint8> TamperedClasses = Classes;
    TamperedClasses[0] = 20;
    TestTrue(TEXT("Composite tamper fixture writes"),
        FFileHelper::SaveArrayToFile(TamperedClasses, *CoverClassPath));
    SkiPreparation::InstalledTerrainIndex InvalidComposite;
    TestFalse(TEXT("Composite reopen re-verifies referenced components"), Store.Open(
        UTF8_TO_TCHAR(Written.ContentId.c_str()), InvalidComposite, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
