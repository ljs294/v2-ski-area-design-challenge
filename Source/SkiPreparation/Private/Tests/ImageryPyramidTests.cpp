#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiPreparation/ImageryPyramid.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <string>

namespace
{
SkiDomain::TerrainCoreManifest MakeTerrainCore()
{
    SkiDomain::TerrainCoreManifest TerrainCore;
    TerrainCore.ContentId = std::string(64, 'a');
    TerrainCore.Width = 512;
    TerrainCore.Height = 300;
    TerrainCore.DeliveredEastSpacingM = 2.0;
    TerrainCore.DeliveredNorthSpacingM = 3.0;
    TerrainCore.LocalOrigin = {47.2, -121.4, 1500.0};
    TerrainCore.SampleCenterBounds = {100.0, 200.0, 1122.0, 1097.0};
    SkiDomain::ComputeTerrainCoreBounds(TerrainCore.Width, TerrainCore.Height,
        TerrainCore.DeliveredEastSpacingM, TerrainCore.DeliveredNorthSpacingM,
        TerrainCore.SampleCenterBounds, TerrainCore.OuterBounds);
    return TerrainCore;
}

SkiPreparation::ImagerySourceMetadata MakeSource()
{
    SkiPreparation::ImagerySourceMetadata Source;
    Source.SourceId = "usgs-imagery-only";
    Source.Product = "USGSImageryOnly";
    Source.ServiceUrlTemplate = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}";
    Source.License = "Public domain, U.S. Government work";
    Source.Attribution = "USGS The National Map";
    Source.TermsUrl = "https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits";
    return Source;
}

bool MakeRequiredManifest(const SkiDomain::TerrainCoreManifest& TerrainCore,
    SkiPreparation::ImageryPyramidManifest& OutManifest)
{
    const SkiPreparation::ImageryPyramidValidation Built =
        SkiPreparation::BuildRequiredImageryPyramid(TerrainCore, MakeSource(), OutManifest);
    if (!Built.Ok()) return false;
    for (int32 Index = 0; Index < OutManifest.Tiles.Num(); ++Index)
    {
        SkiPreparation::ImageryPyramidTile& Tile = OutManifest.Tiles[Index];
        Tile.Sha256 = std::string(64, 'a');
        Tile.Bytes = static_cast<std::uint64_t>(128 + Index);
    }
    return true;
}

class FManifestAssetReader final : public SkiPreparation::IImageryPyramidAssetReader
{
public:
    explicit FManifestAssetReader(const SkiPreparation::ImageryPyramidManifest& InManifest)
        : Manifest(InManifest) {}

    bool Inspect(const std::string& RelativePath,
        const std::uint64_t MaximumBytes, const SkiPreparation::Cancellation& CancellationValue,
        SkiPreparation::ImageryPyramidAssetFacts& OutFacts, FString& OutError) override
    {
        ++Calls;
        if (MaximumBytes != SkiPreparation::ImageryPyramidMaxTileBytes)
        {
            OutError = TEXT("unexpected tile size limit");
            return false;
        }
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("cancelled");
            return false;
        }
        if (CancelAfterThisCall != nullptr && Calls == CancelOnCall)
            CancelAfterThisCall->Cancel();
        for (const SkiPreparation::ImageryPyramidTile& Tile : Manifest.Tiles)
        {
            if (Tile.Path != RelativePath) continue;
            OutFacts.Encoding = Manifest.TileEncoding;
            OutFacts.Width = Tile.RasterWidth;
            OutFacts.Height = Tile.RasterHeight;
            OutFacts.Bytes = Tile.Bytes;
            OutFacts.Sha256 = Tile.Sha256;
            if (MismatchSizeOnCall == Calls) ++OutFacts.Bytes;
            if (MismatchHashOnCall == Calls) OutFacts.Sha256[0] = 'b';
            if (MismatchDimensionsOnCall == Calls) --OutFacts.Width;
            if (MismatchEncodingOnCall == Calls) OutFacts.Encoding = "png-rgba8";
            return true;
        }
        OutError = TEXT("missing fixture");
        return false;
    }

    int32 Calls = 0;
    int32 CancelOnCall = INDEX_NONE;
    int32 MismatchSizeOnCall = INDEX_NONE;
    int32 MismatchHashOnCall = INDEX_NONE;
    int32 MismatchDimensionsOnCall = INDEX_NONE;
    int32 MismatchEncodingOnCall = INDEX_NONE;
    SkiPreparation::Cancellation* CancelAfterThisCall = nullptr;

private:
    const SkiPreparation::ImageryPyramidManifest& Manifest;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryPyramidLayoutAndStorageTest,
    "MountainPlanner.M5.ImageryPyramid.LayoutAndStorage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryPyramidLayoutAndStorageTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest TerrainCore = MakeTerrainCore();
    SkiPreparation::ImageryPyramidManifest Manifest;
    const SkiPreparation::ImageryPyramidValidation Built =
        SkiPreparation::BuildRequiredImageryPyramid(TerrainCore, MakeSource(), Manifest);
    TestTrue(TEXT("required tile manifest builds from TerrainCore geometry"), Built.Ok());
    if (!Built.Ok()) return false;

    TestEqual(TEXT("all five TerrainCore levels are included"), Manifest.Tiles.Num(), 11);
    TSet<int32> Lods;
    for (const SkiPreparation::ImageryPyramidTile& Tile : Manifest.Tiles)
    {
        Lods.Add(Tile.LodIndex);
        TestEqual(TEXT("image raster width is fixed at 256 pixels"),
            static_cast<int32>(Tile.RasterWidth), 256);
        TestEqual(TEXT("image raster height is fixed at 256 pixels"),
            static_cast<int32>(Tile.RasterHeight), 256);
        TestEqual(TEXT("meters per pixel follows the TerrainCore LOD factor"),
            Tile.MetersPerPixelEast,
            TerrainCore.DeliveredEastSpacingM * Tile.LodFactor);
    }
    TestEqual(TEXT("five distinct LODs"), Lods.Num(), 5);
    TestTrue(TEXT("required layout validates before acquisition fills asset fields"),
        SkiPreparation::ValidateImageryPyramidLayout(TerrainCore, Manifest).Ok());
    TestTrue(TEXT("source attribution and license are retained"),
        Manifest.Source.Attribution == "USGS The National Map"
            && Manifest.Source.License == "Public domain, U.S. Government work");
    TestTrue(TEXT("required payload path is canonical and deterministic"),
        Manifest.Tiles[0].Path == "imagery/lod0/0/0.jpg");
    TestEqual(TEXT("last pyramid tile coverage clamps to the TerrainCore sample edge"),
        Manifest.Tiles[Manifest.Tiles.Num() - 1].SampleCenterBounds.EastM,
        TerrainCore.SampleCenterBounds.EastM);

    SkiPreparation::ImageryStorageEstimate Estimate;
    TestTrue(TEXT("preview estimate uses explicit per-tile bounds"),
        SkiPreparation::EstimateImageryPyramidStorage(TerrainCore, Manifest, 1000, 2000, Estimate));
    TestEqual(TEXT("estimated lower bound covers every LOD tile"),
        Estimate.MinimumBytes, static_cast<std::uint64_t>(11000));
    TestEqual(TEXT("estimated upper bound covers every LOD tile"),
        Estimate.MaximumBytes, static_cast<std::uint64_t>(22000));
    TestEqual(TEXT("preview storage remains labelled estimated"),
        static_cast<int32>(Estimate.Certainty),
        static_cast<int32>(SkiPreparation::ImageryStorageSizeCertainty::Estimated));

    TestTrue(TEXT("fixture payload metadata can be added to every required tile"),
        MakeRequiredManifest(TerrainCore, Manifest));
    TestTrue(TEXT("completed declarations validate"),
        SkiPreparation::ValidateImageryPyramidManifest(TerrainCore, Manifest).Ok());

    {
        SkiPreparation::ImageryPyramidManifest Missing = Manifest;
        Missing.Tiles.RemoveAt(Missing.Tiles.Num() - 1);
        TestEqual(TEXT("missing component is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, Missing).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::MissingTile));
    }
    {
        SkiPreparation::ImageryPyramidManifest Duplicate = Manifest;
        Duplicate.Tiles[1] = Duplicate.Tiles[0];
        TestEqual(TEXT("duplicate TerrainCore key is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, Duplicate).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::DuplicateTile));
    }
    {
        SkiPreparation::ImageryPyramidManifest Overlap = Manifest;
        Overlap.Tiles[1].SampleCenterBounds.WestM -= 0.5;
        TestEqual(TEXT("positive-area overlap is rejected with local neighbor checks"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidLayout(
                TerrainCore, Overlap).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::TileOverlap));
    }
    {
        SkiPreparation::ImageryPyramidManifest BadPath = Manifest;
        BadPath.Tiles[0].Path = "../outside.jpg";
        TestEqual(TEXT("unsafe asset path is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, BadPath).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::InvalidAssetPath));
    }
    {
        SkiPreparation::ImageryPyramidManifest DuplicatePath = Manifest;
        DuplicatePath.Tiles[1].Path = DuplicatePath.Tiles[0].Path;
        TestEqual(TEXT("duplicate asset path is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, DuplicatePath).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::DuplicateAssetPath));
    }
    {
        SkiPreparation::ImageryPyramidManifest DuplicatePathCase = Manifest;
        DuplicatePathCase.Tiles[1].Path = "IMAGERY/LOD0/0/0.jpg";
        TestEqual(TEXT("asset paths are unique under Windows case folding"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, DuplicatePathCase).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::DuplicateAssetPath));
    }
    {
        SkiPreparation::ImageryPyramidManifest BadHash = Manifest;
        BadHash.Tiles[0].Sha256 = "not-a-sha256";
        TestEqual(TEXT("malformed content hash is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidManifest(
                TerrainCore, BadHash).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::InvalidAssetHash));
    }
    {
        SkiPreparation::ImageryPyramidManifest WrongEncoding = Manifest;
        WrongEncoding.TileEncoding = "png-rgba8";
        TestEqual(TEXT("unsupported tile encoding is rejected"),
            static_cast<int32>(SkiPreparation::ValidateImageryPyramidLayout(
                TerrainCore, WrongEncoding).Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::UnsupportedEncoding));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryPyramidExactVerificationTest,
    "MountainPlanner.M5.ImageryPyramid.ExactContentVerification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryPyramidExactVerificationTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest TerrainCore = MakeTerrainCore();
    SkiPreparation::ImageryPyramidManifest Manifest;
    TestTrue(TEXT("fixture manifest builds"), MakeRequiredManifest(TerrainCore, Manifest));
    if (Manifest.Tiles.IsEmpty()) return false;

    SkiPreparation::Cancellation Cancellation;
    FManifestAssetReader Reader(Manifest);
    SkiPreparation::ImageryPyramidVerificationReport Report;
    TestTrue(TEXT("stored asset adapter verifies every declared byte sequence"),
        SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, Reader,
            Cancellation, Report));
    TestTrue(TEXT("verified storage is exact"), Report.Storage.IsExact());
    TestEqual(TEXT("all required tiles were inspected"), Report.VerifiedTiles, Manifest.Tiles.Num());

    std::uint64_t ExpectedBytes = 0;
    for (const SkiPreparation::ImageryPyramidTile& Tile : Manifest.Tiles)
        ExpectedBytes += Tile.Bytes;
    TestEqual(TEXT("exact byte total is the sum of all tile files"),
        Report.Storage.MinimumBytes, ExpectedBytes);
    TestEqual(TEXT("verified byte range has no uncertainty"),
        Report.Storage.MaximumBytes, ExpectedBytes);

    {
        FManifestAssetReader WrongSize(Manifest);
        WrongSize.MismatchSizeOnCall = 2;
        SkiPreparation::ImageryPyramidVerificationReport WrongSizeReport;
        TestFalse(TEXT("declared size must match the exact file size"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, WrongSize,
                Cancellation, WrongSizeReport));
        TestEqual(TEXT("size mismatch has a stable error"),
            static_cast<int32>(WrongSizeReport.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::AssetSizeMismatch));
        TestFalse(TEXT("failed verification cannot publish exact storage"),
            WrongSizeReport.Storage.IsExact());
    }
    {
        FManifestAssetReader WrongHash(Manifest);
        WrongHash.MismatchHashOnCall = 1;
        SkiPreparation::ImageryPyramidVerificationReport WrongHashReport;
        TestFalse(TEXT("declared hash must match the exact file hash"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, WrongHash,
                Cancellation, WrongHashReport));
        TestEqual(TEXT("hash mismatch has a stable error"),
            static_cast<int32>(WrongHashReport.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::AssetHashMismatch));
    }
    {
        FManifestAssetReader WrongDimensions(Manifest);
        WrongDimensions.MismatchDimensionsOnCall = 1;
        SkiPreparation::ImageryPyramidVerificationReport WrongDimensionsReport;
        TestFalse(TEXT("stored image dimensions must match the required raster size"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, WrongDimensions,
                Cancellation, WrongDimensionsReport));
        TestEqual(TEXT("dimension mismatch has a stable error"),
            static_cast<int32>(WrongDimensionsReport.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::AssetDimensionsMismatch));
    }
    {
        FManifestAssetReader WrongEncoding(Manifest);
        WrongEncoding.MismatchEncodingOnCall = 1;
        SkiPreparation::ImageryPyramidVerificationReport WrongEncodingReport;
        TestFalse(TEXT("stored image encoding must match the manifest"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, WrongEncoding,
                Cancellation, WrongEncodingReport));
        TestEqual(TEXT("encoding mismatch has a stable error"),
            static_cast<int32>(WrongEncodingReport.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::AssetEncodingMismatch));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryPyramidCancellationTest,
    "MountainPlanner.M5.ImageryPyramid.Cancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryPyramidCancellationTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest TerrainCore = MakeTerrainCore();
    SkiPreparation::ImageryPyramidManifest Manifest;
    TestTrue(TEXT("fixture manifest builds"), MakeRequiredManifest(TerrainCore, Manifest));
    if (Manifest.Tiles.IsEmpty()) return false;

    {
        SkiPreparation::Cancellation Cancellation;
        Cancellation.Cancel();
        FManifestAssetReader Reader(Manifest);
        SkiPreparation::ImageryPyramidVerificationReport Report;
        TestFalse(TEXT("cancelled verification is rejected before asset inspection"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, Reader,
                Cancellation, Report));
        TestEqual(TEXT("pre-cancel makes no adapter calls"), Reader.Calls, 0);
        TestEqual(TEXT("pre-cancel reports cancellation"),
            static_cast<int32>(Report.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::Cancelled));
    }
    {
        SkiPreparation::Cancellation Cancellation;
        FManifestAssetReader Reader(Manifest);
        Reader.CancelAfterThisCall = &Cancellation;
        Reader.CancelOnCall = 1;
        SkiPreparation::ImageryPyramidVerificationReport Report;
        TestFalse(TEXT("mid-verification cancellation prevents publishing the remainder"),
            SkiPreparation::VerifyImageryPyramid(TerrainCore, Manifest, Reader,
                Cancellation, Report));
        TestEqual(TEXT("only the active tile reaches the adapter"), Reader.Calls, 1);
        TestEqual(TEXT("mid-run cancellation reports cancellation"),
            static_cast<int32>(Report.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::Cancelled));
        TestFalse(TEXT("cancelled verification does not claim an exact total"),
            Report.Storage.IsExact());
    }
    {
        SkiPreparation::Cancellation Cancellation;
        Cancellation.Cancel();
        SkiPreparation::ImageryPyramidManifest OutManifest;
        const SkiPreparation::ImageryPyramidValidation Built =
            SkiPreparation::BuildRequiredImageryPyramid(TerrainCore, MakeSource(),
                OutManifest, &Cancellation);
        TestEqual(TEXT("cancelled planning is rejected"),
            static_cast<int32>(Built.Error),
            static_cast<int32>(SkiPreparation::ImageryPyramidError::Cancelled));
        TestEqual(TEXT("cancelled planning publishes no partial required set"),
            OutManifest.Tiles.Num(), 0);
    }
    return true;
}

#endif
