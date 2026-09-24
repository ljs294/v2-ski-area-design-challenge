#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/TerrainCorePackageStore.h"

namespace
{
bool MakeCore(const FString& Root, const char* SourceId,
    SkiDomain::TerrainCoreManifest& Out, FString& Error)
{
    SkiDomain::Heightfield Field;
    Field.Width = 2; Field.Height = 2;
    Field.EastSpacingM = 1.0; Field.NorthSpacingM = 1.0;
    Field.NoDataValue = -9999.0; Field.CurrentRevision = 1;
    Field.Samples = {100,101,102,103};
    SkiDomain::TerrainCoreManifest M;
    M.GeneratorVersion = "library-test-v1";
    M.ProcessingVersions = {"terraincore-derivation-v1"};
    M.Source.SourceId = SourceId;
    M.Source.Product = "synthetic";
    M.Source.AcquisitionEpoch = "2026-09-23";
    M.Source.HorizontalCrs = "LOCAL_ENU";
    M.Source.HorizontalDatum = "WGS84";
    M.Source.VerticalDatum = "synthetic";
    M.Source.License = "CC0";
    M.Source.Attribution = "fixture";
    M.Source.NativeEastSpacingM = 1.0;
    M.Source.NativeNorthSpacingM = 1.0;
    M.LocalOrigin = {44.0,-71.0,100.0};
    FString Directory;
    return SkiPreparation::TerrainCorePackageStore(Root).WriteAndActivate(
        M, Field, Directory, Out, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstalledTerrainLibraryTest,
    "MountainPlanner.M1.Library.ListVerified",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInstalledTerrainLibraryTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("InstalledTerrainLibrary"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    SkiPreparation::InstalledTerrainStore Store(Root);
    FString Error;
    TArray<SkiPreparation::InstalledTerrainLibraryEntry> Entries;
    TestTrue(TEXT("Missing library is an empty listing"), Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Missing library has no entries"), Entries.Num(), 0);

    SkiDomain::TerrainCoreManifest Core, Surround;
    const bool CoreReady = MakeCore(Root, "library-primary", Core, Error)
        && MakeCore(Root, "library-surround", Surround, Error);
    TestTrue(*FString::Printf(TEXT("Core fixtures activate: %s"), *Error), CoreReady);
    if (!CoreReady) return false;
    SkiDomain::CoverEcologyManifest Cover;
    Cover.GeneratorVersion = "library-cover-v1";
    Cover.Source = {"worldcover", "synthetic", "2021", "fixture", "CC0", "fixture"};
    Cover.Transform.Width = 2; Cover.Transform.Height = 2;
    Cover.Transform.LongitudeStepDeg = 0.01; Cover.Transform.LatitudeStepDeg = 0.01;
    Cover.Transform.SampleCenterBounds = {-71.01,43.99,-71.0,44.0};
    SkiDomain::ComputeCoverEcologyOuterBounds(2,2,0.01,0.01,
        Cover.Transform.SampleCenterBounds, Cover.Transform.OuterBounds);
    SkiDomain::CoverEcologyManifest WrittenCover;
    FString CoverDirectory;
    const TArray<uint8> Classes{10,10,10,10};
    const TArray<uint8> Validity{0x0f};
    const bool CoverReady = SkiPreparation::CoverEcologyStore(Root).WriteAndActivate(
        Cover, Classes, Validity, CoverDirectory, WrittenCover, Error);
    TestTrue(*FString::Printf(TEXT("Cover fixture activates: %s"), *Error), CoverReady);
    if (!CoverReady) return false;
    SkiDomain::InstalledTerrainReceipt Receipt;
    Receipt.GeneratorVersion = "legacy-install-v1";
    Receipt.TerrainCoreId = Core.ContentId;
    Receipt.SurroundTerrainCoreId = Surround.ContentId;
    Receipt.CoverEcologyId = WrittenCover.ContentId;
    FString ReceiptDirectory;
    SkiDomain::InstalledTerrainReceipt Written;
    const bool Installed = Store.WriteAndActivate(Receipt, ReceiptDirectory, Written, Error);
    TestTrue(*FString::Printf(TEXT("Composite activates: %s"), *Error), Installed);
    if (!Installed) return false;

    const FString Parent = FPaths::Combine(Root, TEXT("InstalledTerrain"));
    const FString Staging = FPaths::Combine(Root, TEXT(".installedterrain-staging"), TEXT("incomplete"));
    IFileManager::Get().MakeDirectory(*Staging, true);
    FFileHelper::SaveStringToFile(TEXT("partial"), *FPaths::Combine(Staging, TEXT("receipt.json")));
    const FString Corrupt = FPaths::Combine(Parent, FString::ChrN(64,TEXT('a')));
    IFileManager::Get().MakeDirectory(*Corrupt, true);
    FFileHelper::SaveStringToFile(TEXT("{}"), *FPaths::Combine(Corrupt, TEXT("receipt.json")));
    IFileManager::Get().MakeDirectory(*FPaths::Combine(Parent, TEXT("not-a-content-id")), true);

    TestTrue(TEXT("Listing succeeds with corrupt and incomplete neighbors"),
        Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Only verified composite is listed"), Entries.Num(), 1);
    if (Entries.Num() == 1)
    {
        TestEqual(TEXT("Verified content ID"), Entries[0].ContentId,
            UTF8_TO_TCHAR(Written.ContentId.c_str()));
        TestEqual(TEXT("Verified source ID"), Entries[0].SourceId,
            FString(TEXT("library-primary")));
        TestEqual(TEXT("Verified source epoch"), Entries[0].AcquisitionEpoch,
            FString(TEXT("2026-09-23")));
        TestEqual(TEXT("Legacy generator retained without name revalidation"),
            Entries[0].GeneratorVersion, FString(TEXT("legacy-install-v1")));
    }
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
