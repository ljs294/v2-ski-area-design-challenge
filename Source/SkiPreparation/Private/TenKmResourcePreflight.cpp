#include "SkiPreparation/TenKmResourcePreflight.h"

#include "SkiDomain/CoverEcology.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiPreparation/CogTerrainSampler.h"
#include "SkiPreparation/ImageryAcquisition.h"
#include "SkiPreparation/ImageryPyramid.h"
#include "SkiPreparation/OsmVectorPackage.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainScratchStore.h"

#include <algorithm>
#include <limits>
#include <string>

namespace
{
using namespace SkiPreparation;

constexpr uint64 BytesPerGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64 MaximumSiteSamplesPerAxis = 10'001ULL;
constexpr uint64 MinimumSiteSamplesPerAxis = 2'001ULL;
constexpr uint64 OverpassResponseBytes = 32ULL * 1024ULL * 1024ULL;

bool CheckedAdd(const uint64 A, const uint64 B, uint64& Out) noexcept
{
    const uint64 Max = (std::numeric_limits<uint64>::max)();
    if (B > Max - A) return false;
    Out = A + B;
    return true;
}

bool CheckedMultiply(const uint64 A, const uint64 B, uint64& Out) noexcept
{
    const uint64 Max = (std::numeric_limits<uint64>::max)();
    if (A != 0 && B > Max / A) return false;
    Out = A * B;
    return true;
}

uint64 CeilDivide(const uint64 Numerator, const uint64 Denominator) noexcept
{
    return Numerator / Denominator + (Numerator % Denominator == 0 ? 0ULL : 1ULL);
}

FTenKmByteEstimate Estimate(const uint64 Bytes, const ETenKmStorageCertainty Certainty,
    const TCHAR* Basis)
{
    FTenKmByteEstimate Result;
    Result.Bytes = Bytes;
    Result.Certainty = Certainty;
    Result.Basis = Basis;
    return Result;
}

ETenKmStorageCertainty CombineCertainty(const ETenKmStorageCertainty A,
    const ETenKmStorageCertainty B) noexcept
{
    if (A == ETenKmStorageCertainty::Unknown || B == ETenKmStorageCertainty::Unknown)
        return ETenKmStorageCertainty::Unknown;
    return A == ETenKmStorageCertainty::Estimated || B == ETenKmStorageCertainty::Estimated
        ? ETenKmStorageCertainty::Estimated : ETenKmStorageCertainty::Exact;
}

bool AddEstimate(FTenKmByteEstimate& Total, const FTenKmByteEstimate& Value) noexcept
{
    uint64 Sum = 0;
    if (Value.Certainty == ETenKmStorageCertainty::Unknown
        || !CheckedAdd(Total.Bytes, Value.Bytes, Sum)) return false;
    Total.Bytes = Sum;
    Total.Certainty = CombineCertainty(Total.Certainty, Value.Certainty);
    return true;
}

void Fail(FTenKmResourcePreflightReport& Report,
    const ETenKmResourcePreflightStatus Status, const TCHAR* Code, const TCHAR* Detail)
{
    Report.Status = Status;
    Report.FailureCode = Code;
    Report.FailureDetail = Detail;
}

bool ExactResolvedCogTotal(const TerrainAvailabilityReport& Catalog, uint64& OutBytes)
{
    OutBytes = 0;
    if (Catalog.ResolvedSources.IsEmpty()
        || Catalog.ResolvedCandidateObjectBytes.Certainty != StorageSizeCertainty::Exact
        || Catalog.ResolvedCandidateObjectBytes.Bytes == 0)
    {
        return false;
    }

    uint64 Total = 0;
    for (const ElevationCatalogSource& Source : Catalog.ResolvedSources)
    {
        if (!Source.HasExactObjectBytes || Source.ExactObjectBytes == 0
            || !Source.HasVerifiedCogHeader || !Source.HasValidatedDownloadUrl
            || Source.ExactObjectBytes > (std::numeric_limits<uint64>::max)() - Total)
        {
            return false;
        }
        Total += Source.ExactObjectBytes;
    }
    if (Total != Catalog.ResolvedCandidateObjectBytes.Bytes) return false;
    OutBytes = Total;
    return true;
}

bool MakePlanningTerrainCore(const uint32 Width, const uint32 Height,
    SkiDomain::TerrainCoreManifest& OutCore)
{
    OutCore = {};
    OutCore.ContentId = std::string(64, 'a');
    OutCore.GeneratorVersion = "m6-resource-preflight";
    OutCore.LocalOrigin = {47.2, -121.4, 0.0};
    OutCore.Width = Width;
    OutCore.Height = Height;
    OutCore.DeliveredEastSpacingM = 1.0;
    OutCore.DeliveredNorthSpacingM = 1.0;
    OutCore.SampleCenterBounds = {0.0, 0.0,
        static_cast<double>(Width - 1U), static_cast<double>(Height - 1U)};
    return SkiDomain::ComputeTerrainCoreBounds(Width, Height, 1.0, 1.0,
        OutCore.SampleCenterBounds, OutCore.OuterBounds);
}

bool EstimateTerrainCorePackageUpperBound(const SkiDomain::TerrainCoreTilePlan& Plan,
    uint64& OutBytes, uint64& OutMaximumTilePayloadBytes)
{
    OutBytes = 0;
    OutMaximumTilePayloadBytes = 0;
    if (Plan.Tiles.empty() || Plan.Tiles.size() > SkiDomain::TerrainCoreMaxTiles)
        return false;

    uint64 EncodedTotal = 0;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Plan.Tiles)
    {
        const uint64 Samples = SkiPreparation::TerrainCoreStoredSampleCount(Tile);
        if (Samples == 0 || Samples > SkiDomain::TerrainCoreMaxStoredSamples
            || Samples > (std::numeric_limits<uint64>::max)() / sizeof(float))
        {
            return false;
        }
        const uint64 HeightRaw = Samples * sizeof(float);
        const uint64 ValidityRaw = CeilDivide(Samples, 8ULL);
        const uint64 HeightBound = SkiDomain::TerrainCoreDeflateBound(HeightRaw);
        const uint64 ValidityBound = SkiDomain::TerrainCoreDeflateBound(ValidityRaw);
        uint64 TilePayload = 0;
        if (!CheckedAdd(HeightBound, ValidityBound, TilePayload)
            || !CheckedAdd(EncodedTotal, TilePayload, EncodedTotal))
        {
            return false;
        }
        OutMaximumTilePayloadBytes = std::max(OutMaximumTilePayloadBytes, TilePayload);
    }

    if (!CheckedAdd(EncodedTotal, SkiDomain::TerrainCoreMaxManifestBytes, OutBytes)
        || OutBytes > SkiDomain::TerrainCoreMaxInstalledBytes)
    {
        return false;
    }
    return true;
}

bool EstimateCoverPackageUpperBound(const uint32 Width, const uint32 Height,
    uint64& OutBytes, uint64& OutCells)
{
    OutBytes = 0;
    OutCells = 0;
    uint64 Cells = 0;
    if (Width < 2 || Height < 2 || !CheckedMultiply(Width, Height, Cells)
        || Cells > SkiDomain::CoverEcologyMaxCells)
    {
        return false;
    }
    const uint64 ValidityBytes = CeilDivide(Cells, 8ULL);
    uint64 Channels = 0;
    if (!CheckedAdd(Cells, ValidityBytes, Channels)
        || !CheckedAdd(Channels, SkiDomain::CoverEcologyMaxManifestBytes, OutBytes)
        || OutBytes > SkiDomain::CoverEcologyMaxPackageBytes)
    {
        return false;
    }
    OutCells = Cells;
    return true;
}

bool SumLedgerColumn(const TArray<FTenKmResourceLedgerRow>& Ledger,
    const FTenKmByteEstimate FTenKmResourceLedgerRow::* Member,
    FTenKmByteEstimate& OutTotal)
{
    OutTotal = {};
    OutTotal.Certainty = ETenKmStorageCertainty::Exact;
    for (const FTenKmResourceLedgerRow& Row : Ledger)
    {
        if (!AddEstimate(OutTotal, Row.*Member)) return false;
    }
    return true;
}

bool MakeImageryAndVectorEstimates(const FTenKmResourcePreflightRequest& Request,
    SkiDomain::TerrainCoreManifest& Core, ImageryPyramidManifest& Imagery,
    ImageryStorageEstimate& ImageryStorage, OsmVectorStorageEstimate& VectorStorage)
{
    if (!MakePlanningTerrainCore(Request.WidthSamples, Request.HeightSamples, Core)) return false;
    const ImageryPyramidValidation ImageryBuild = BuildRequiredImageryPyramid(
        Core, MakeUsgsImageryOnlyMetadata(), Imagery);
    if (!ImageryBuild.Ok() || Request.ImageryMinimumBytesPerTile == 0
        || Request.ImageryMaximumBytesPerTile < Request.ImageryMinimumBytesPerTile
        || Request.ImageryMaximumBytesPerTile > ImageryAcquisitionBudget{}.MaximumOutputTileBytes
        || !EstimateImageryPyramidStorage(Core, Imagery,
            Request.ImageryMinimumBytesPerTile, Request.ImageryMaximumBytesPerTile,
            ImageryStorage))
    {
        return false;
    }
    return EstimateOsmVectorStorage(Request.ExpectedVectorFeatureCount,
        Request.ExpectedVectorPointCount, VectorStorage);
}

uint64 MemoryMaximum(const uint64 A, const uint64 B) noexcept
{
    return std::max(A, B);
}
}

SkiPreparation::ETenKmResourcePreflightStatus SkiPreparation::PlanTenKmResourcePreflight(
    const FTenKmResourcePreflightRequest& Request,
    FTenKmResourcePreflightReport& OutReport)
{
    using namespace SkiPreparation;
    OutReport = {};
    OutReport.AvailableFreeDiskBytes = Request.FreeDiskBytes;
    OutReport.Resume.bPartialCompositeActivationProhibited = true;
    OutReport.Resume.bValidatedChunksAndCheckpointsRequiredForResume = true;
    OutReport.Resume.bCanonicalScratchDurableResumeImplemented = false;
    OutReport.Resume.bResumeStorageSemanticsQualified = false;
    OutReport.Resume.QualificationGap = TEXT("TerrainScratchStore currently removes its owned directory during cleanup/destruction; durable 10 km scratch resume and full composite cancellation recovery still need end-to-end proof.");

    if (Request.WidthSamples < MinimumSiteSamplesPerAxis
        || Request.HeightSamples < MinimumSiteSamplesPerAxis
        || Request.WidthSamples > MaximumSiteSamplesPerAxis
        || Request.HeightSamples > MaximumSiteSamplesPerAxis)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::InvalidSiteSize,
            TEXT("M6_SITE_DIMENSIONS_OUT_OF_RANGE"),
            TEXT("Both projected site axes must be between 2 km and 10 km inclusive at 1 m sample spacing."));
        return OutReport.Status;
    }

    const TerrainAvailabilityReport& Elevation = Request.Elevation;
    if (!Elevation.IsSupportedGeography || !Elevation.HasElevationCoverage
        || !Elevation.HasVerifiedSiteCoverage || !Elevation.HasCompleteSourceLineage
        || !Elevation.HasVerifiedCogHeaders || !Elevation.HasQualityReport
        || !Elevation.DownloadEnabled)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::CoverageUnproven,
            TEXT("M6_REQUIRED_COVERAGE_OR_ELEVATION_PROOF_MISSING"),
            TEXT("Elevation coverage, complete lineage, COG identity, sampled quality and download eligibility must all be proven before storage is approved."));
        return OutReport.Status;
    }
    if (!Request.bWorldCoverCoverageVerified)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::CoverageUnproven,
            TEXT("M6_WORLDCOVER_COVERAGE_UNPROVEN"),
            TEXT("WorldCover coverage for the complete site must be verified before storage is approved."));
        return OutReport.Status;
    }

    uint64 ElevationObjectBytes = 0;
    bool bHasS1mSource = false;
    for (const ElevationCatalogSource& Source : Elevation.ResolvedSources)
        bHasS1mSource = bHasS1mSource || Source.Candidate.Product == SkiDomain::ElevationProduct::S1M;
    if (!ExactResolvedCogTotal(Elevation, ElevationObjectBytes)
        || Request.ElevationSidecarSizeCertainty == StorageSizeCertainty::Unknown
        || (bHasS1mSource && Request.ElevationSidecarObjectBytes == 0)
        || Request.WorldCoverSourceObjectBytes == 0
        || Request.WorldCoverSourceSizeCertainty == StorageSizeCertainty::Unknown
        || Request.CoverWidth == 0 || Request.CoverHeight == 0
        || !Request.bVectorCountEstimateAvailable
        || Request.ImageryMinimumBytesPerTile == 0
        || Request.ImageryMaximumBytesPerTile == 0)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::RequiredSizeUnknown,
            TEXT("M6_REQUIRED_COMPONENT_SIZE_UNKNOWN"),
            TEXT("Every required COG, cover window, imagery tile interval and vector estimate needs a positive, source-backed size before planning."));
        return OutReport.Status;
    }
    if (Request.WorldCoverSourceSizeCertainty != StorageSizeCertainty::Exact
        || Request.WorldCoverSourceObjectBytes == 0)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::RequiredSizeUnknown,
            TEXT("M6_WORLDCOVER_OBJECT_SIZE_NOT_EXACT"),
            TEXT("The WorldCover COG object length must be proven by its identity-bound header before planning."));
        return OutReport.Status;
    }

    SkiDomain::TerrainCoreTilePlan CorePlan;
    if (!SkiDomain::PlanTerrainCoreTiles(Request.WidthSamples, Request.HeightSamples, CorePlan)
        || CorePlan.Tiles.empty())
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::InvalidSiteSize,
            TEXT("M6_TERRAINCORE_TILE_PLAN_FAILED"),
            TEXT("TerrainCore could not produce a bounded tile plan for the projected sample grid."));
        return OutReport.Status;
    }
    OutReport.TerrainCoreTileCount = static_cast<uint64>(CorePlan.Tiles.size());

    uint64 ScratchBytes = 0;
    if (!TerrainScratchStore::TryCalculateRequiredStorageBytes(Request.WidthSamples,
            Request.HeightSamples, ScratchBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_SCRATCH_SIZE_OVERFLOW"),
            TEXT("The canonical scratch size exceeds the checked TerrainCore and file-offset limits."));
        return OutReport.Status;
    }

    SkiDomain::TerrainCoreManifest PlanningCore;
    ImageryPyramidManifest Imagery;
    ImageryStorageEstimate ImageryStorage;
    OsmVectorStorageEstimate VectorStorage;
    if (!MakeImageryAndVectorEstimates(Request, PlanningCore, Imagery,
            ImageryStorage, VectorStorage))
    {
        Fail(OutReport, Request.bVectorCountEstimateAvailable
                ? ETenKmResourcePreflightStatus::InvalidEvidence
                : ETenKmResourcePreflightStatus::RequiredSizeUnknown,
            TEXT("M6_IMAGERY_OR_VECTOR_ESTIMATE_INVALID"),
            TEXT("The complete imagery layout and site-specific OSM feature/point estimates must fit their existing model limits."));
        return OutReport.Status;
    }
    OutReport.ImageryPyramidTileCount = static_cast<uint64>(Imagery.Tiles.Num());

    uint64 CorePackageBytes = 0;
    uint64 MaximumTerrainTilePayloadBytes = 0;
    uint64 CoverPackageBytes = 0;
    uint64 CoverCells = 0;
    if (!EstimateTerrainCorePackageUpperBound(CorePlan, CorePackageBytes,
            MaximumTerrainTilePayloadBytes)
        || !EstimateCoverPackageUpperBound(Request.CoverWidth, Request.CoverHeight,
            CoverPackageBytes, CoverCells))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ComponentLimitExceeded,
            TEXT("M6_COMPONENT_PACKAGE_LIMIT_EXCEEDED"),
            TEXT("A required TerrainCore or CoverEcology component exceeds its existing package cap."));
        return OutReport.Status;
    }

    const uint64 SiteContextManifestBytes = SiteContextMaxManifestBytes;
    const uint64 CompositeReceiptBytes = CompositeInstallReceiptMaxBytes;
    if (ImageryStorage.MaximumBytes == 0
        || ImageryStorage.MaximumBytes > SiteContextMaxPackageBytes
        || ImageryStorage.MaximumBytes > (std::numeric_limits<uint64>::max)() - VectorStorage.MaximumBytes
        || VectorStorage.MaximumBytes > SiteContextMaxAssetBytes
        || ImageryStorage.MaximumBytes > SiteContextMaxPackageBytes - VectorStorage.MaximumBytes)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ComponentLimitExceeded,
            TEXT("M6_SITECONTEXT_PACKAGE_LIMIT_EXCEEDED"),
            TEXT("The estimated imagery or vector assets exceed existing SiteContext package or asset caps."));
        return OutReport.Status;
    }

    uint64 SiteContextAssetsBytes = 0;
    uint64 SiteContextPackageBytes = 0;
    if (!CheckedAdd(ImageryStorage.MaximumBytes, VectorStorage.MaximumBytes,
            SiteContextAssetsBytes)
        || !CheckedAdd(SiteContextAssetsBytes, SiteContextManifestBytes,
            SiteContextPackageBytes)
        || SiteContextPackageBytes > SiteContextMaxPackageBytes
        || CorePackageBytes > SkiDomain::TerrainCoreMaxInstalledBytes
        || CoverPackageBytes > SkiDomain::CoverEcologyMaxPackageBytes)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_PACKAGE_SIZE_OVERFLOW"),
            TEXT("A checked component or composite package byte total overflowed."));
        return OutReport.Status;
    }

    FTenKmResourceLedgerRow TerrainRow;
    TerrainRow.Component = TEXT("TerrainCore");
    TerrainRow.Download = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No additional bytes."));
    TerrainRow.Stage = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    TerrainRow.Scratch = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    TerrainRow.Install = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    uint64 ElevationDownloadObjectBytes = 0;
    if (!CheckedAdd(ElevationObjectBytes, Request.ElevationSidecarObjectBytes,
            ElevationDownloadObjectBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_ELEVATION_OBJECT_SIZE_OVERFLOW"),
            TEXT("The resolved elevation COG and lineage sidecar object sizes overflowed the checked total."));
        return OutReport.Status;
    }
    TerrainRow.Download = Estimate(ElevationDownloadObjectBytes,
        ETenKmStorageCertainty::Estimated,
        TEXT("Estimated full-object cache ceiling from exact resolver COG lengths; actual range transfer may be smaller."));
    TerrainRow.Stage = Estimate(CorePackageBytes, ETenKmStorageCertainty::Estimated,
        TEXT("Estimated conservative zlib deflateBound for every planned tile plus the maximum TerrainCore manifest."));
    TerrainRow.Scratch = Estimate(ScratchBytes, ETenKmStorageCertainty::Exact,
        TEXT("Exact raw canonical scratch planes: float32 height, validity, provenance and source index (7 bytes/sample)."));
    TerrainRow.Install = TerrainRow.Stage;
    OutReport.Ledger.Add(TerrainRow);

    FTenKmResourceLedgerRow CoverRow;
    CoverRow.Component = TEXT("CoverEcology");
    CoverRow.Download = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No additional bytes."));
    CoverRow.Stage = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    CoverRow.Scratch = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    CoverRow.Install = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    CoverRow.Download = Estimate(Request.WorldCoverSourceObjectBytes,
        ETenKmStorageCertainty::Estimated,
        TEXT("Estimated full-object cache ceiling from the exact identity-bound WorldCover COG length; range transfer may be smaller."));
    CoverRow.Stage = Estimate(CoverPackageBytes, ETenKmStorageCertainty::Estimated,
        TEXT("Exact class and packed-validity asset lengths plus the maximum CoverEcology manifest."));
    CoverRow.Install = CoverRow.Stage;
    OutReport.Ledger.Add(CoverRow);

    FTenKmResourceLedgerRow ImageryRow;
    ImageryRow.Component = TEXT("SiteContext imagery");
    ImageryRow.Download = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No additional bytes."));
    ImageryRow.Stage = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    ImageryRow.Scratch = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    ImageryRow.Install = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    ImageryRow.Download = Estimate(ImageryAcquisitionBudget{}.MaximumSourceTransferBytes,
        ETenKmStorageCertainty::Estimated,
        TEXT("Current acquisition hard transfer ceiling for source imagery tiles."));
    ImageryRow.Stage = Estimate(ImageryStorage.MaximumBytes,
        ETenKmStorageCertainty::Estimated, *ImageryStorage.Basis);
    ImageryRow.Install = ImageryRow.Stage;
    OutReport.Ledger.Add(ImageryRow);

    FTenKmResourceLedgerRow VectorRow;
    VectorRow.Component = TEXT("SiteContext vectors");
    VectorRow.Download = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No additional bytes."));
    VectorRow.Stage = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    VectorRow.Scratch = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    VectorRow.Install = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    VectorRow.Download = Estimate(OverpassResponseBytes, ETenKmStorageCertainty::Estimated,
        TEXT("Current Overpass adapter response-byte ceiling (32 MiB)."));
    VectorRow.Stage = Estimate(VectorStorage.MaximumBytes, ETenKmStorageCertainty::Estimated,
        *VectorStorage.Basis);
    VectorRow.Install = VectorRow.Stage;
    OutReport.Ledger.Add(VectorRow);

    FTenKmResourceLedgerRow SiteContextRow;
    SiteContextRow.Component = TEXT("SiteContext and composite metadata");
    SiteContextRow.Download = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No additional bytes."));
    SiteContextRow.Stage = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    SiteContextRow.Scratch = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    SiteContextRow.Install = Estimate(0, ETenKmStorageCertainty::Exact, TEXT("No bytes."));
    SiteContextRow.Stage = Estimate(SiteContextManifestBytes,
        ETenKmStorageCertainty::Exact,
        TEXT("Maximum SiteContext manifest and composite receipt size reserved before activation."));
    SiteContextRow.Stage.Bytes += CompositeReceiptBytes;
    SiteContextRow.Install = Estimate(SiteContextManifestBytes + CompositeReceiptBytes,
        ETenKmStorageCertainty::Exact,
        TEXT("Maximum SiteContext manifest plus maximum atomic composite install receipt."));
    OutReport.Ledger.Add(SiteContextRow);

    if (!SumLedgerColumn(OutReport.Ledger, &FTenKmResourceLedgerRow::Download,
            OutReport.DownloadBytes)
        || !SumLedgerColumn(OutReport.Ledger, &FTenKmResourceLedgerRow::Stage,
            OutReport.StageBytes)
        || !SumLedgerColumn(OutReport.Ledger, &FTenKmResourceLedgerRow::Scratch,
            OutReport.ScratchBytes)
        || !SumLedgerColumn(OutReport.Ledger, &FTenKmResourceLedgerRow::Install,
            OutReport.InstallBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_LEDGER_TOTAL_OVERFLOW"),
            TEXT("A checked download, stage, scratch or install ledger sum overflowed."));
        return OutReport.Status;
    }

    uint64 OperationalPeak = 0;
    if (!CheckedAdd(OutReport.DownloadBytes.Bytes, OutReport.StageBytes.Bytes,
            OperationalPeak)
        || !CheckedAdd(OperationalPeak, OutReport.ScratchBytes.Bytes,
            OperationalPeak)
        || !CheckedAdd(OperationalPeak, OutReport.InstallBytes.Bytes,
            OperationalPeak))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_PEAK_DISK_OVERFLOW"),
            TEXT("The simultaneous download, staging, scratch and install reservation overflowed."));
        return OutReport.Status;
    }
    const uint64 TenPercentReserve = CeilDivide(OperationalPeak, 10ULL);
    const uint64 RollbackReserve = std::max(BytesPerGiB, TenPercentReserve);
    OutReport.RollbackReserveBytes = Estimate(RollbackReserve,
        ETenKmStorageCertainty::Estimated,
        TEXT("Required safety/rollback reserve: max(1 GiB, 10% of simultaneous download + stage + scratch + install peak)."));
    uint64 RequiredDisk = 0;
    if (!CheckedAdd(OperationalPeak, RollbackReserve, RequiredDisk))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_REQUIRED_DISK_OVERFLOW"),
            TEXT("The safety reserve overflowed the checked peak-disk total."));
        return OutReport.Status;
    }
    OutReport.RequiredFreeDiskBytes = Estimate(RequiredDisk,
        ETenKmStorageCertainty::Estimated,
        TEXT("Conservative peak including simultaneous staging/install copies, canonical scratch and the required reserve."));
    OutReport.bHasEnoughFreeDisk = Request.FreeDiskBytes >= RequiredDisk;
    OutReport.Resume.RetainedWorkspaceUpperBoundBytes = 0;
    if (!CheckedAdd(OutReport.DownloadBytes.Bytes, OutReport.StageBytes.Bytes,
            OutReport.Resume.RetainedWorkspaceUpperBoundBytes)
        || !CheckedAdd(OutReport.Resume.RetainedWorkspaceUpperBoundBytes,
            OutReport.ScratchBytes.Bytes, OutReport.Resume.RetainedWorkspaceUpperBoundBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_RESUME_WORKSPACE_OVERFLOW"),
            TEXT("The retained download, stage and scratch workspace total overflowed."));
        return OutReport.Status;
    }

    const uint64 MaxSamples = SkiDomain::TerrainCoreMaxStoredSamples;
    uint64 TerrainTilePayloadBytes = 0;
    if (!CheckedMultiply(MaxSamples, sizeof(float) + 1ULL, TerrainTilePayloadBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_MEMORY_BOUND_OVERFLOW"),
            TEXT("The runtime tile payload bound overflowed."));
        return OutReport.Status;
    }
    uint64 RuntimeWorkerBytes = 0;
    if (!CheckedMultiply(TerrainTilePayloadBytes, 2ULL, RuntimeWorkerBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_WORKER_BOUND_OVERFLOW"),
            TEXT("The runtime tile-worker reservation overflowed."));
        return OutReport.Status;
    }

    uint64 SamplerPeak = 0;
    uint64 ScratchTileBytes = 0;
    uint64 CoreBuilderPeak = 0;
    uint64 CoverWorkingBytes = 0;
    uint64 PlanMetadataBytes = 0;
    const FCogTerrainSamplerLimits SamplerLimits;
    const ImageryAcquisitionBudget ImageryLimits;
    if (!CheckedMultiply(static_cast<uint64>(TerrainScratchTileSamples)
                * TerrainScratchTileSamples, TerrainScratchBytesPerSample,
            ScratchTileBytes)
        || !CheckedAdd(SamplerLimits.MaxResidentBytes,
            SamplerLimits.Preflight.MaxCachedBytes, SamplerPeak)
        || !CheckedAdd(SamplerPeak, SamplerLimits.MaxDecodedTileBytes, SamplerPeak)
        || !CheckedAdd(SamplerPeak, SamplerLimits.MaxEncodedTileBytes, SamplerPeak)
        || !CheckedAdd(SamplerPeak, ScratchTileBytes, SamplerPeak)
        || !CheckedAdd(TerrainTilePayloadBytes, MaximumTerrainTilePayloadBytes,
            CoreBuilderPeak)
        || !CheckedAdd(CoreBuilderPeak, ScratchTileBytes, CoreBuilderPeak)
        || !CheckedMultiply(CoverCells, 3ULL, CoverWorkingBytes)
        || !CheckedAdd(CoverWorkingBytes, SamplerLimits.Preflight.MaxCachedBytes,
            CoverWorkingBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_MEMORY_BOUND_OVERFLOW"),
            TEXT("A checked preparation-stage memory bound overflowed."));
        return OutReport.Status;
    }
    uint64 TerrainMetadataBytes = 0;
    uint64 ImageryMetadataBytes = 0;
    if (!CheckedMultiply(static_cast<uint64>(CorePlan.Tiles.size()),
            sizeof(SkiDomain::TerrainCoreTileDescriptor), TerrainMetadataBytes)
        || !CheckedMultiply(static_cast<uint64>(Imagery.Tiles.Num()),
            sizeof(ImageryPyramidTile) + 128ULL, ImageryMetadataBytes)
        || !CheckedAdd(TerrainMetadataBytes, ImageryMetadataBytes, PlanMetadataBytes))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_PLAN_METADATA_OVERFLOW"),
            TEXT("The bounded tile-plan metadata estimate overflowed."));
        return OutReport.Status;
    }
    OutReport.Memory.MaximumTerrainTilePayloadBytes = TerrainTilePayloadBytes;
    OutReport.Memory.MaximumTerrainPlanMetadataBytes = PlanMetadataBytes;
    uint64 PreparationPeak = MemoryMaximum(SamplerPeak,
        MemoryMaximum(CoreBuilderPeak,
            MemoryMaximum(CoverWorkingBytes, ImageryLimits.MaximumWorkingMemoryBytes)));
    if (!CheckedAdd(PreparationPeak, PlanMetadataBytes, PreparationPeak))
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ArithmeticOverflow,
            TEXT("M6_MEMORY_BOUND_OVERFLOW"),
            TEXT("The tile-plan metadata exceeded the checked preparation peak bound."));
        return OutReport.Status;
    }
    // These are the existing SkiTerrainRuntime defaults (512 MiB cache, two workers).
    // Keep this mapping explicit because SkiPreparation must not depend on the runtime module.
    OutReport.Memory.PreparationPeakBytes = PreparationPeak;
    OutReport.Memory.RuntimeTileCacheBudgetBytes = 512ULL * 1024ULL * 1024ULL;
    OutReport.Memory.RuntimeWorkerReservationBytes = RuntimeWorkerBytes;
    OutReport.Memory.PeakAcrossSequentialPhasesBytes = std::max(PreparationPeak,
        OutReport.Memory.RuntimeTileCacheBudgetBytes);
    OutReport.Memory.RuntimeMaximumWorkerJobs = 2;
    OutReport.Memory.MaximumConcurrentAcquisitionRequests = 1;

    OutReport.bWithinCurrentImageryAcquisitionLimits =
        OutReport.ImageryPyramidTileCount <= ImageryAcquisitionBudget{}.MaximumPyramidTiles
        && ImageryStorage.MaximumBytes <= ImageryAcquisitionBudget{}.MaximumOutputBytes;
    if (!OutReport.bWithinCurrentImageryAcquisitionLimits)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::ComponentLimitExceeded,
            TEXT("M6_CURRENT_IMAGERY_ACQUISITION_CAP_EXCEEDED"),
            TEXT("The required imagery pyramid exceeds the current 512-tile or 256 MiB acquisition cap; M6 is not qualified by this plan."));
        return OutReport.Status;
    }
    if (!OutReport.bHasEnoughFreeDisk)
    {
        Fail(OutReport, ETenKmResourcePreflightStatus::InsufficientDisk,
            TEXT("M6_INSUFFICIENT_FREE_DISK"),
            TEXT("Available free space is below the conservative peak plus the max(1 GiB, 10%) reserve."));
        return OutReport.Status;
    }

    OutReport.Status = ETenKmResourcePreflightStatus::Ready;
    OutReport.FailureCode = TEXT("M6_PREFLIGHT_ESTIMATE_READY_NOT_QUALIFICATION");
    OutReport.FailureDetail = TEXT("Static source-backed storage and application-memory bounds pass current component caps. A real 10 km acquisition, cancel/resume, atomic install and cold offline reopen are still required for qualification.");
    OutReport.bTenKmQualified = false;
    return OutReport.Status;
}
