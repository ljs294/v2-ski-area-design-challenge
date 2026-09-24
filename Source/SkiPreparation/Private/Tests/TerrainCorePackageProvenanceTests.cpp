#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainCorePackageStore.h"

namespace
{
FString ProvenanceTempRoot(const TCHAR* Name)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

SkiDomain::Heightfield ProvenanceField()
{
    SkiDomain::Heightfield Field;
    Field.Width = 4;
    Field.Height = 4;
    Field.WestM = -3.0;
    Field.NorthM = 7.0;
    Field.EastSpacingM = 1.0;
    Field.NorthSpacingM = 1.0;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples.resize(16U);
    for (uint32 Row = 0; Row < Field.Height; ++Row)
    {
        for (uint32 Column = 0; Column < Field.Width; ++Column)
        {
            Field.Samples[static_cast<uint64>(Row) * Field.Width + Column]
                = 100.0F + static_cast<float>(Row) + static_cast<float>(Column) * 0.25F;
        }
    }
    Field.Samples[10] = Field.NoDataValue;
    return Field;
}

SkiDomain::TerrainCoreManifest ProvenanceManifest(const SkiDomain::Heightfield& Field)
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "terraincore-package-provenance-test-v1";
    Manifest.ProcessingVersions = {"terraincore-derivation-v1"};
    Manifest.Source.SourceId = "fixture-ground-v1";
    Manifest.Source.Product = "synthetic bare earth";
    Manifest.Source.AcquisitionEpoch = "2026-09-24";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "WGS84";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "deterministic provenance fixture";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 1.0;
    Manifest.LocalOrigin = {46.93, -121.50, 1500.0};
    Manifest.Width = Field.Width;
    Manifest.Height = Field.Height;
    Manifest.DeliveredEastSpacingM = Field.EastSpacingM;
    Manifest.DeliveredNorthSpacingM = Field.NorthSpacingM;
    Manifest.SampleCenterBounds = {Field.WestM,
        Field.SampleNorthM(Field.Height - 1U), Field.EastM(Field.Width - 1U), Field.NorthM};
    SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
        Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    return Manifest;
}

SkiPreparation::TerrainCoreTileSource MakeTileSource(const SkiDomain::Heightfield& Field)
{
    return [&Field](const SkiDomain::TerrainCoreTileDescriptor& Planned,
        SkiPreparation::TerrainCoreEncodedTile& OutTile, FString& OutError)
    {
        return SkiPreparation::DeriveTerrainCoreTile(Field, Planned, OutTile, OutError);
    };
}

SkiPreparation::TerrainCoreProvenanceTileSource MakeProvenanceSource()
{
    return [](const SkiDomain::TerrainCoreTileDescriptor&,
        TArrayView<const uint8> Validity, TArray<uint8>& OutProvenance,
        TArray<uint8>& OutSourceIndices, FString& OutError)
    {
        OutError.Reset();
        OutProvenance.SetNumUninitialized(Validity.Num());
        OutSourceIndices.SetNumUninitialized(Validity.Num());
        for (int32 Index = 0; Index < Validity.Num(); ++Index)
        {
            if (Validity[Index] == 0)
            {
                OutProvenance[Index] = static_cast<uint8>(
                    SkiApplication::TerrainSampleProvenance::NoData);
                OutSourceIndices[Index] = SkiApplication::TerrainCoreNoSourceIndex;
            }
            else
            {
                OutProvenance[Index] = static_cast<uint8>(
                    SkiApplication::TerrainSampleProvenance::S1MNative);
                OutSourceIndices[Index] = 0;
            }
        }
        return true;
    };
}

bool WriteProvenancePackage(const FString& Root, const bool IncludeProvenance,
    FString& OutDirectory, SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError)
{
    const SkiDomain::Heightfield Field = ProvenanceField();
    SkiPreparation::TerrainCorePackageStore Store(Root);
    const auto TileSource = MakeTileSource(Field);
    if (IncludeProvenance)
    {
        return Store.WriteAndActivateFromTiles(ProvenanceManifest(Field), TileSource,
            OutDirectory, OutManifest, OutError, nullptr, 0, 0,
            MakeProvenanceSource());
    }
    return Store.WriteAndActivateFromTiles(ProvenanceManifest(Field), TileSource,
        OutDirectory, OutManifest, OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCorePackageVerifiedProvenanceTest,
    "MountainPlanner.M5.TerrainCorePackageStore.VerifiedProvenanceSidecars",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCorePackageVerifiedProvenanceTest::RunTest(const FString&)
{
    const FString Root = ProvenanceTempRoot(TEXT("TerrainCoreProvenance"));
    const FString RepeatRoot = ProvenanceTempRoot(TEXT("TerrainCoreProvenanceRepeat"));
    FString Directory, RepeatDirectory, Error;
    SkiDomain::TerrainCoreManifest Written, Repeated;
    TestTrue(*FString::Printf(TEXT("Package with TCP1 sidecars activates: %s"), *Error),
        WriteProvenancePackage(Root, true, Directory, Written, Error));
    if (Directory.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        IFileManager::Get().DeleteDirectory(*RepeatRoot, false, true);
        return false;
    }

    TestTrue(*FString::Printf(TEXT("Identical sidecar package writes: %s"), *Error),
        WriteProvenancePackage(RepeatRoot, true, RepeatDirectory, Repeated, Error));
    TestEqual(TEXT("Package content identity is deterministic across roots"),
        Written.ContentId, Repeated.ContentId);

    SkiPreparation::TerrainCorePackageStore Store(Root);
    SkiPreparation::TerrainCorePackageIndex Index;
    const FString Id = UTF8_TO_TCHAR(Written.ContentId.c_str());
    TestTrue(*FString::Printf(TEXT("Provenance package reopens: %s"), *Error),
        Store.Open(Id, Index, Error));
    if (!Index.Manifest.ContentId.empty())
    {
        TestEqual(TEXT("One LOD0 tile is declared"), Index.ProvenanceSidecars.size(), size_t(1));
        TestEqual(TEXT("Sidecar path is canonical"), Index.ProvenanceSidecars[0].Path,
            std::string("provenance/lod0/0/0.tcp"));
        TestTrue(TEXT("Sidecar declares exact file size and hash"),
            Index.ProvenanceSidecars[0].Bytes > 265U
            && Index.ProvenanceSidecars[0].Sha256.size() == 64U
            && Index.ProvenanceSidecars[0].SemanticSha256.size() == 64U);
        TestEqual(TEXT("Canonical package manifest digest is exposed"),
            Index.PackageManifestSha256.Len(), 64);
        SkiPreparation::TerrainCorePackageIndex RepeatIndex;
        SkiPreparation::TerrainCorePackageStore RepeatStore(RepeatRoot);
        TestTrue(*FString::Printf(TEXT("Repeated provenance package reopens: %s"), *Error),
            RepeatStore.Open(UTF8_TO_TCHAR(Repeated.ContentId.c_str()), RepeatIndex, Error));
        TestEqual(TEXT("Canonical package manifest digest is deterministic"),
            Index.PackageManifestSha256, RepeatIndex.PackageManifestSha256);
        TestTrue(*FString::Printf(TEXT("Complete package verification passes: %s"), *Error),
            Store.Verify(Index, Error));

        SkiApplication::TerrainCoreProvenanceSummary Summary;
        TestTrue(*FString::Printf(TEXT("Verified provenance summary reopens: %s"), *Error),
            Store.ReadVerifiedProvenanceSummary(Index, Summary, Error));
        TestEqual(TEXT("Summary binds to package core content ID"),
            Summary.TerrainCoreId, Written.ContentId);
        TestEqual(TEXT("Canonical grid sample count is exact"), Summary.TotalSamples,
            uint64(16));
        TestEqual(TEXT("Valid sample count is derived from stored validity"),
            Summary.ValidSamples, uint64(15));
        TestEqual(TEXT("NoData sample count is derived from stored validity"),
            Summary.NoDataSamples, uint64(1));
        TestEqual(TEXT("Primary source record is exact"), Summary.Sources.size(), size_t(1));
        if (Summary.Sources.size() == 1U)
        {
            TestEqual(TEXT("Primary source has fifteen valid samples"),
                Summary.Sources[0].SampleCount, uint64(15));
            TestEqual(TEXT("Primary source class cross-tab is preserved"),
                Summary.Sources[0].SamplesByProvenance[0], uint64(15));
        }

        TArray<uint8> SidecarBytes;
        TestTrue(*FString::Printf(TEXT("Persisted sidecar can be read: %s"), *Error),
            Store.ReadProvenanceSidecar(Index, 0, 0, 0, SidecarBytes, Error));
        TestTrue(TEXT("Persisted record has the TCP1 wire marker"),
            SidecarBytes.Num() > 4 && SidecarBytes[0] == 'T' && SidecarBytes[1] == 'C'
            && SidecarBytes[2] == 'P' && SidecarBytes[3] == '1');

        const FString SidecarPath = FPaths::Combine(Directory,
            TEXT("provenance/lod0/0/0.tcp"));
        TArray<uint8> OriginalBytes = SidecarBytes;
        TArray<uint8> TamperedHeader = OriginalBytes;
        if (TamperedHeader.IsValidIndex(8)) TamperedHeader[8] ^= 1U;
        TestTrue(TEXT("Header tamper fixture is written"),
            FFileHelper::SaveArrayToFile(TamperedHeader, *SidecarPath));
        SkiPreparation::TerrainCorePackageIndex Rejected;
        TestFalse(TEXT("Header/byte tamper is rejected while semantic digest stays declared"),
            Store.Open(Id, Rejected, Error));
        TestTrue(TEXT("Rejected tampered package clears index output"),
            Rejected.PackageDirectory.IsEmpty() && Rejected.Manifest.ContentId.empty());
        TestTrue(TEXT("Original sidecar can be restored after tamper check"),
            FFileHelper::SaveArrayToFile(OriginalBytes, *SidecarPath));

        TestTrue(TEXT("Missing sidecar fixture is applied"),
            IFileManager::Get().Delete(*SidecarPath, false, true, true));
        TestFalse(TEXT("Missing declared sidecar is rejected on reopen"),
            Store.Open(Id, Rejected, Error));
        TestTrue(TEXT("Original sidecar can be restored after missing-file check"),
            FFileHelper::SaveArrayToFile(OriginalBytes, *SidecarPath));

        const FString ExtraPath = FPaths::Combine(Directory, TEXT("provenance/unlisted.bin"));
        TestTrue(TEXT("Extra-file fixture is written"),
            FFileHelper::SaveStringToFile(TEXT("unlisted"), *ExtraPath));
        TestFalse(TEXT("Package exact-tree check rejects an extra file"),
            Store.Open(Id, Rejected, Error));
        IFileManager::Get().Delete(*ExtraPath, false, true, true);
        TestTrue(TEXT("Package reopens after removing the extra file"),
            Store.Open(Id, Index, Error));
    }

    const FString LegacyRoot = ProvenanceTempRoot(TEXT("TerrainCoreLegacyProvenance"));
    FString LegacyDirectory;
    SkiDomain::TerrainCoreManifest LegacyManifest;
    TestTrue(*FString::Printf(TEXT("Core without sidecars remains writable: %s"), *Error),
        WriteProvenancePackage(LegacyRoot, false, LegacyDirectory, LegacyManifest, Error));
    if (!LegacyDirectory.IsEmpty())
    {
        SkiPreparation::TerrainCorePackageStore LegacyStore(LegacyRoot);
        SkiPreparation::TerrainCorePackageIndex LegacyIndex;
        TestTrue(TEXT("Legacy core package still opens and verifies"),
            LegacyStore.Open(UTF8_TO_TCHAR(LegacyManifest.ContentId.c_str()), LegacyIndex, Error)
            && LegacyStore.Verify(LegacyIndex, Error));
        SkiApplication::TerrainCoreProvenanceSummary LegacySummary;
        TestFalse(TEXT("Legacy package cannot pretend to provide grade provenance"),
            LegacyStore.ReadVerifiedProvenanceSummary(LegacyIndex, LegacySummary, Error));
        TestTrue(TEXT("Unavailable legacy summary remains empty"),
            LegacySummary.TerrainCoreId.empty() && LegacySummary.TotalSamples == 0U);
    }

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    IFileManager::Get().DeleteDirectory(*RepeatRoot, false, true);
    IFileManager::Get().DeleteDirectory(*LegacyRoot, false, true);
    return true;
}

#endif
