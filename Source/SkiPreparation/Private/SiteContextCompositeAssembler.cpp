#include "SkiPreparation/SiteContextCompositeAssembler.h"

#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace
{
using namespace SkiPreparation;

std::string Utf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), Converted.Length());
}

bool SameBounds(const SkiDomain::MetricBounds& A, const SkiDomain::MetricBounds& B)
{
    const double Scale = std::max({1.0, std::abs(A.WestM), std::abs(A.SouthM),
        std::abs(A.EastM), std::abs(A.NorthM), std::abs(B.WestM), std::abs(B.SouthM),
        std::abs(B.EastM), std::abs(B.NorthM)});
    const double Tolerance = Scale * 1.0e-9;
    return std::isfinite(A.WestM) && std::isfinite(A.SouthM)
        && std::isfinite(A.EastM) && std::isfinite(A.NorthM)
        && std::isfinite(B.WestM) && std::isfinite(B.SouthM)
        && std::isfinite(B.EastM) && std::isfinite(B.NorthM)
        && std::abs(A.WestM - B.WestM) <= Tolerance
        && std::abs(A.SouthM - B.SouthM) <= Tolerance
        && std::abs(A.EastM - B.EastM) <= Tolerance
        && std::abs(A.NorthM - B.NorthM) <= Tolerance;
}

std::string ImageryPyramidPath(const ImageryPyramidTile& Tile)
{
    return "imagery/lod" + std::to_string(Tile.LodIndex) + "/"
        + std::to_string(Tile.TileY) + "/" + std::to_string(Tile.TileX) + ".jpg";
}

std::string SiteContextImageryPath(const ImageryPyramidTile& Tile)
{
    return "imagery/lod" + std::to_string(Tile.LodIndex) + "/"
        + std::to_string(Tile.TileX) + "/" + std::to_string(Tile.TileY) + ".jpg";
}

FString CanonicalJsonSha256(const FString& Json)
{
    const FTCHARToUTF8 Encoded(*Json);
    return SkiPreparation::Sha256(TArrayView<const uint8>(
        reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
}

bool ValidateQualityBeforeWriting(const std::string& GeneratorVersion,
    const SkiDomain::TerrainProvenanceCounts& Counts,
    const SkiDomain::TerrainQualityReport& Quality,
    const SkiDomain::TerrainCoreManifest& Core,
    const SkiDomain::CoverEcologyManifest& Cover, FString& OutError)
{
    CompositeInstallReceipt Probe;
    Probe.GeneratorVersion = GeneratorVersion;
    Probe.ProvenanceCounts = Counts;
    Probe.Quality = Quality;
    const FString QualityId = ComputeTerrainQualityReportId(Counts, Quality);
    const std::string QualityIdUtf8 = Utf8(QualityId);
    Probe.Components = {
        {CompositeInstallComponentKind::TerrainCore, CompositeInstallComponentStatus::Verified,
            Core.ContentId, Utf8(CanonicalJsonSha256(SerializeTerrainCoreManifest(Core, true)))},
        {CompositeInstallComponentKind::CoverEcology, CompositeInstallComponentStatus::Verified,
            Cover.ContentId, Utf8(CanonicalJsonSha256(SerializeCoverEcologyManifest(Cover, true)))},
        {CompositeInstallComponentKind::SiteContext, CompositeInstallComponentStatus::Verified,
            std::string(64, 'f'), std::string(64, 'f')},
        {CompositeInstallComponentKind::QualityReport, CompositeInstallComponentStatus::Verified,
            QualityIdUtf8, QualityIdUtf8},
    };
    Probe.ContentId = Utf8(ComputeCompositeInstallReceiptId(Probe));
    if (ValidateCompositeInstallReceipt(Probe).Ok()) return true;
    OutError = TEXT("Terrain provenance counts or quality report fail schema-3 composite validation.");
    return false;
}
}

bool SkiPreparation::AssembleAndActivateSiteContextComposite(
    const FString& DataRoot, const FString& TerrainCoreId, const FString& CoverEcologyId,
    const FString& GeneratorVersion, const ImageryPyramidManifest& Imagery,
    ISiteContextCompositeImagerySource& ImagerySource,
    const OsmVectorSiteContextAsset& Vectors,
    const SkiDomain::TerrainProvenanceCounts& ProvenanceCounts,
    const SkiDomain::TerrainQualityReport& Quality,
    const Cancellation& CancellationValue,
    SiteContextCompositeAssemblyResult& OutResult, FString& OutError)
{
    OutResult = {};
    OutError.Reset();
    const auto Fail = [&](const TCHAR* Detail)
    {
        OutError = Detail;
        return false;
    };
    if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));

    TerrainCorePackageStore CoreStore(DataRoot);
    TerrainCorePackageIndex Core;
    if (!CoreStore.Open(TerrainCoreId, Core, OutError)
        || !CoreStore.Verify(Core, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Prepared TerrainCore could not be reopened and verified.");
        return false;
    }
    if (UTF8_TO_TCHAR(Core.Manifest.ContentId.c_str()) != TerrainCoreId)
        return Fail(TEXT("Reopened TerrainCore does not match the requested component identity."));

    CoverEcologyStore CoverStore(DataRoot);
    CoverEcologyPackageIndex Cover;
    if (!CoverStore.Open(CoverEcologyId, Cover, OutError)
        || !CoverStore.Verify(Cover, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Prepared CoverEcology could not be reopened and verified.");
        return false;
    }
    if (UTF8_TO_TCHAR(Cover.Manifest.ContentId.c_str()) != CoverEcologyId)
        return Fail(TEXT("Reopened CoverEcology does not match the requested component identity."));

    const SkiDomain::TerrainCoreManifest& TerrainCore = Core.Manifest;
    if (!SkiDomain::IsTerrainCoreSha256(Vectors.SourcePackageContentId)
        || Vectors.TerrainCoreId != TerrainCore.ContentId
        || !SameBounds(Vectors.ExtentM, TerrainCore.OuterBounds)
        || Vectors.VectorJsonUtf8.IsEmpty()
        || static_cast<std::uint64_t>(Vectors.VectorJsonUtf8.Num()) > SiteContextMaxAssetBytes)
    {
        return Fail(TEXT("Converted OSM SiteContext asset is not bound to the verified TerrainCore or is empty/oversized."));
    }
    if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));

    for (int32 Index = 0; Index < Imagery.Tiles.Num(); ++Index)
    {
        const ImageryPyramidTile& Tile = Imagery.Tiles[Index];
        if (Tile.Path != ImageryPyramidPath(Tile))
        {
            OutError = FString::Printf(TEXT("Imagery tile %d path does not match the required lod/Y/X source convention."), Index);
            return false;
        }
    }

    ImageryPyramidVerificationReport ImageryVerification;
    if (!VerifyImageryPyramid(TerrainCore, Imagery, ImagerySource,
            CancellationValue, ImageryVerification))
    {
        OutError = ImageryVerification.FailureDetail.IsEmpty()
            ? FString::Printf(TEXT("Imagery pyramid verification failed (%d, tile %d)."),
                static_cast<int32>(ImageryVerification.Error), ImageryVerification.TileIndex)
            : ImageryVerification.FailureDetail;
        return false;
    }
    if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));

    const std::string GeneratorVersionUtf8 = Utf8(GeneratorVersion);
    if (!ValidateQualityBeforeWriting(GeneratorVersionUtf8, ProvenanceCounts, Quality,
            TerrainCore, Cover.Manifest, OutError)) return false;

    SiteContextManifest Manifest;
    Manifest.ContentId = std::string(64, '0');
    Manifest.SchemaVersion = SiteContextSchema;
    Manifest.GeneratorVersion = GeneratorVersionUtf8;
    Manifest.TerrainCoreId = TerrainCore.ContentId;
    Manifest.VectorEncoding = SiteContextVectorEncodingV2;
    Manifest.VectorAssetPath = "vectors/osm-enu-polylines.json";
    Manifest.VectorSource.SourcePackageContentId = Vectors.SourcePackageContentId;
    Manifest.VectorSource.TerrainCoreId = Vectors.TerrainCoreId;
    Manifest.VectorSource.ExtentM = Vectors.ExtentM;
    Manifest.VectorSource.Provider = Vectors.Source.Provider;
    Manifest.VectorSource.Endpoint = Vectors.Source.Endpoint;
    Manifest.VectorSource.SourceTimestampUtc = Vectors.Source.SourceTimestampUtc;
    Manifest.VectorSource.RetrievedAtUtc = Vectors.Source.RetrievedAtUtc;
    Manifest.VectorSource.License = Vectors.Source.License;
    Manifest.VectorSource.Attribution = Vectors.Source.Attribution;
    Manifest.VectorSource.AttributionUrl = Vectors.Source.AttributionUrl;
    Manifest.Attributions = {
        {"USGS", Imagery.Source.License, Imagery.Source.Attribution},
        {"OpenStreetMap contributors", Vectors.Source.License, Vectors.Source.Attribution},
    };

    std::unordered_map<std::string, std::string> SitePathToInputPath;
    SitePathToInputPath.reserve(static_cast<std::size_t>(Imagery.Tiles.Num()));
    Manifest.ImageryTiles.reserve(static_cast<std::size_t>(Imagery.Tiles.Num()));
    Manifest.Assets.reserve(static_cast<std::size_t>(Imagery.Tiles.Num()) + 1U);
    for (int32 Index = 0; Index < Imagery.Tiles.Num(); ++Index)
    {
        if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));
        const ImageryPyramidTile& SourceTile = Imagery.Tiles[Index];
        const std::string ExpectedInputPath = ImageryPyramidPath(SourceTile);
        if (SourceTile.Path != ExpectedInputPath)
        {
            OutError = FString::Printf(TEXT("Imagery tile %d path does not match the required lod/Y/X source convention."), Index);
            return false;
        }

        const std::string SitePath = SiteContextImageryPath(SourceTile);
        if (!SitePathToInputPath.emplace(SitePath, SourceTile.Path).second)
        {
            OutError = FString::Printf(TEXT("Imagery tile %d maps to a duplicate SiteContext lod/X/Y asset path."), Index);
            return false;
        }

        SiteContextImageryTile SiteTile;
        SiteTile.LodIndex = SourceTile.LodIndex;
        SiteTile.LodFactor = SourceTile.LodFactor;
        SiteTile.TileX = SourceTile.TileX;
        SiteTile.TileY = SourceTile.TileY;
        SiteTile.PixelWidth = SourceTile.RasterWidth;
        SiteTile.PixelHeight = SourceTile.RasterHeight;
        SiteTile.EastMetersPerPixel = SourceTile.MetersPerPixelEast;
        SiteTile.NorthMetersPerPixel = SourceTile.MetersPerPixelNorth;
        SiteTile.AssetPath = SitePath;
        Manifest.ImageryTiles.push_back(SiteTile);
        Manifest.Assets.push_back({SitePath, Imagery.TileEncoding,
            SourceTile.Sha256, SourceTile.Bytes});
    }

    const FString VectorPath = UTF8_TO_TCHAR(Manifest.VectorAssetPath.c_str());
    TArray<uint8> VectorBytes = Vectors.VectorJsonUtf8;
    Manifest.Assets.push_back({Manifest.VectorAssetPath, Manifest.VectorEncoding,
        Utf8(Sha256(MakeArrayView(VectorBytes))), static_cast<std::uint64_t>(VectorBytes.Num())});
    if (!ValidateSiteContextManifest(Manifest, TerrainCore).Ok())
        return Fail(TEXT("Generated SiteContext schema-2 manifest does not match the verified components or vector lineage."));

    SiteContextStore SiteStore(DataRoot);
    FString SiteContextDirectory;
    SiteContextManifest StoredSiteContext;
    const SiteContextAssetReader ReadSiteContextAsset =
        [&](const FString& RelativePath, TArray<uint8>& OutBytes, FString& ReadError)
    {
        if (CancellationValue.IsCancelled())
        {
            ReadError = TEXT("Composite assembly was cancelled while copying SiteContext assets.");
            return false;
        }
        const std::string RequestedPath = Utf8(RelativePath);
        if (RequestedPath == Manifest.VectorAssetPath)
        {
            OutBytes = VectorBytes;
            return true;
        }
        const auto SourcePath = SitePathToInputPath.find(RequestedPath);
        if (SourcePath == SitePathToInputPath.end())
        {
            ReadError = TEXT("Requested SiteContext path has no verified input asset mapping.");
            return false;
        }
        return ImagerySource.Read(SourcePath->second, CancellationValue, OutBytes, ReadError);
    };
    if (!SiteStore.WriteAndActivate(Manifest, TerrainCore, ReadSiteContextAsset,
            SiteContextDirectory, StoredSiteContext, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Verified SiteContext assets could not be stored and reopened.");
        return false;
    }
    if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));

    SiteContextPackageIndex ReopenedSiteContext;
    if (!SiteStore.Open(UTF8_TO_TCHAR(StoredSiteContext.ContentId.c_str()),
            TerrainCore, ReopenedSiteContext, OutError)
        || !SiteStore.Verify(ReopenedSiteContext, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Stored SiteContext failed its final reopen verification.");
        return false;
    }

    CompositeInstallReceipt Receipt;
    Receipt.GeneratorVersion = GeneratorVersionUtf8;
    Receipt.ProvenanceCounts = ProvenanceCounts;
    Receipt.Quality = Quality;
    const FString QualityId = ComputeTerrainQualityReportId(ProvenanceCounts, Quality);
    const std::string QualityIdUtf8 = Utf8(QualityId);
    Receipt.Components = {
        {CompositeInstallComponentKind::TerrainCore, CompositeInstallComponentStatus::Verified,
            TerrainCore.ContentId,
            Utf8(CanonicalJsonSha256(SerializeTerrainCoreManifest(TerrainCore, true)))},
        {CompositeInstallComponentKind::CoverEcology, CompositeInstallComponentStatus::Verified,
            Cover.Manifest.ContentId,
            Utf8(CanonicalJsonSha256(SerializeCoverEcologyManifest(Cover.Manifest, true)))},
        {CompositeInstallComponentKind::SiteContext, CompositeInstallComponentStatus::Verified,
            StoredSiteContext.ContentId,
            Utf8(CanonicalJsonSha256(SerializeSiteContextManifest(StoredSiteContext, true)))},
        {CompositeInstallComponentKind::QualityReport, CompositeInstallComponentStatus::Verified,
            QualityIdUtf8, QualityIdUtf8},
    };
    Receipt.ContentId = Utf8(ComputeCompositeInstallReceiptId(Receipt));
    if (!ValidateCompositeInstallReceipt(Receipt).Ok())
        return Fail(TEXT("Generated schema-3 composite receipt failed validation."));

    if (CancellationValue.IsCancelled()) return Fail(TEXT("Composite assembly was cancelled."));
    InstalledTerrainStore InstallationStore(DataRoot);
    FString ReceiptDirectory;
    CompositeInstallReceipt WrittenReceipt;
    if (!InstallationStore.WriteAndActivate(Receipt, ReceiptDirectory, WrittenReceipt, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Verified M5 composite could not be atomically activated.");
        return false;
    }

    InstalledTerrainIndex ReopenedInstallation;
    TArray<InstalledTerrainLibraryEntry> VerifiedEntries;
    if (!InstallationStore.Open(UTF8_TO_TCHAR(WrittenReceipt.ContentId.c_str()),
            ReopenedInstallation, OutError)
        || ReopenedInstallation.SchemaVersion != CompositeInstallReceiptSchema
        || !InstallationStore.ListVerified(VerifiedEntries, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Activated composite failed its final library reopen check.");
        return false;
    }
    const bool IsListed = VerifiedEntries.ContainsByPredicate([&](const InstalledTerrainLibraryEntry& Entry)
    {
        return Entry.ContentId == UTF8_TO_TCHAR(WrittenReceipt.ContentId.c_str());
    });
    if (!IsListed) return Fail(TEXT("Activated composite is not present in the verified installed-terrain library."));

    OutResult.SiteContext = MoveTemp(ReopenedSiteContext.Manifest);
    OutResult.Receipt = MoveTemp(ReopenedInstallation.CompositeReceipt);
    OutResult.SiteContextPackageDirectory = SiteContextDirectory;
    OutResult.InstalledReceiptDirectory = ReopenedInstallation.ReceiptDirectory;
    return true;
}
