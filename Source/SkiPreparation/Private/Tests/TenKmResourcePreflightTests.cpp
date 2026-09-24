#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiPreparation/ImageryAcquisition.h"
#include "SkiPreparation/TenKmResourcePreflight.h"

#include <limits>

namespace
{
SkiPreparation::FTenKmResourcePreflightRequest MakePreflightRequest(
    const uint32 SiteSamples, const uint64 FreeDiskBytes)
{
    using namespace SkiPreparation;
    FTenKmResourcePreflightRequest Request;
    Request.WidthSamples = SiteSamples;
    Request.HeightSamples = SiteSamples;
    Request.CoverWidth = (SiteSamples - 1U) / 10U + 1U;
    Request.CoverHeight = Request.CoverWidth;
    Request.WorldCoverSourceObjectBytes = 64ULL * 1024ULL * 1024ULL;
    Request.WorldCoverSourceSizeCertainty = StorageSizeCertainty::Exact;
    Request.bWorldCoverCoverageVerified = true;
    Request.ImageryMinimumBytesPerTile = 16ULL * 1024ULL;
    Request.ImageryMaximumBytesPerTile = 64ULL * 1024ULL;
    Request.bVectorCountEstimateAvailable = true;
    Request.ExpectedVectorFeatureCount = 2'000;
    Request.ExpectedVectorPointCount = 10'000;
    Request.FreeDiskBytes = FreeDiskBytes;

    Request.Elevation.Status = ElevationCatalogStatus::Ready;
    Request.Elevation.IsSupportedGeography = true;
    Request.Elevation.CatalogComplete = true;
    Request.Elevation.HasElevationCoverage = true;
    Request.Elevation.DownloadEnabled = true;
    Request.Elevation.HasQualityReport = true;
    Request.Elevation.HasCompleteSourceLineage = true;
    Request.Elevation.HasVerifiedCogHeaders = true;
    Request.Elevation.HasVerifiedSiteCoverage = true;
    Request.Elevation.CoverageStatusCode = TEXT("RASTER_FOOTPRINT_VERIFIED");
    Request.Elevation.ResolvedCandidateObjectBytes.Bytes = 128ULL * 1024ULL * 1024ULL;
    Request.Elevation.ResolvedCandidateObjectBytes.Certainty = StorageSizeCertainty::Exact;
    Request.Elevation.ResolvedCandidateObjectBytes.Basis = TEXT("offline fixture exact COG lengths");
    Request.ElevationSidecarObjectBytes = 8ULL * 1024ULL * 1024ULL;
    Request.ElevationSidecarSizeCertainty = StorageSizeCertainty::Exact;
    ElevationCatalogSource Source;
    Source.Candidate.Product = SkiDomain::ElevationProduct::S1M;
    Source.HasExactObjectBytes = true;
    Source.ExactObjectBytes = Request.Elevation.ResolvedCandidateObjectBytes.Bytes;
    Source.HasVerifiedCogHeader = true;
    Source.HasValidatedDownloadUrl = true;
    Request.Elevation.ResolvedSources.Add(Source);
    return Request;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTenKmResourcePreflightTwoAndFourKmTest,
    "SkiPreparation.M6.TenKmResourcePreflight.LedgerAtTwoAndFourKm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTenKmResourcePreflightTwoAndFourKmTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    constexpr uint64 FreeDisk = 64ULL * 1024ULL * 1024ULL * 1024ULL;
    for (const uint32 SideSamples : {2001U, 4001U})
    {
        FTenKmResourcePreflightReport Report;
        const ETenKmResourcePreflightStatus Status = PlanTenKmResourcePreflight(
            MakePreflightRequest(SideSamples, FreeDisk), Report);
        TestEqual(TEXT("site fits the modeled storage and current component limits"),
            static_cast<uint8>(Status), static_cast<uint8>(ETenKmResourcePreflightStatus::Ready));
        const uint64 ExpectedScratch = static_cast<uint64>(SideSamples) * SideSamples * 7ULL;
        TestEqual(TEXT("canonical scratch counts all four raw planes"),
            Report.ScratchBytes.Bytes, ExpectedScratch);
        TestEqual(TEXT("scratch bytes are exact"),
            static_cast<uint8>(Report.ScratchBytes.Certainty),
            static_cast<uint8>(ETenKmStorageCertainty::Exact));
        TestEqual(TEXT("COG object cache ceiling is labeled estimated network usage"),
            static_cast<uint8>(Report.Ledger[0].Download.Certainty),
            static_cast<uint8>(ETenKmStorageCertainty::Estimated));
        TestEqual(TEXT("TerrainCore stage bytes are estimated from the compression ceiling"),
            static_cast<uint8>(Report.Ledger[0].Stage.Certainty),
            static_cast<uint8>(ETenKmStorageCertainty::Estimated));
        TestEqual(TEXT("install size is separately exposed"),
            static_cast<uint8>(Report.Ledger[0].Install.Certainty),
            static_cast<uint8>(ETenKmStorageCertainty::Estimated));
        TestTrue(TEXT("the 1 GiB minimum reserve is included"),
            Report.RollbackReserveBytes.Bytes >= 1024ULL * 1024ULL * 1024ULL);
        TestEqual(TEXT("reserve certainty reflects the estimated peak"),
            static_cast<uint8>(Report.RollbackReserveBytes.Certainty),
            static_cast<uint8>(ETenKmStorageCertainty::Estimated));
        TestTrue(TEXT("required free disk includes the reserve"),
            Report.RequiredFreeDiskBytes.Bytes > Report.DownloadBytes.Bytes
                + Report.StageBytes.Bytes + Report.ScratchBytes.Bytes
                + Report.InstallBytes.Bytes);
        TestTrue(TEXT("available disk comparison passes for the supplied capacity"),
            Report.bHasEnoughFreeDisk);
        TestTrue(TEXT("the plan does not claim the live 10 km qualification"),
            !Report.bTenKmQualified);
        TestTrue(TEXT("all serial acquisition paths are bounded to one concurrent request"),
            Report.Memory.MaximumConcurrentAcquisitionRequests == 1);
        TestTrue(TEXT("cancel/resume capacity is part of the computed workspace"),
            Report.Resume.RetainedWorkspaceUpperBoundBytes > 0
                && Report.Resume.bValidatedChunksAndCheckpointsRequiredForResume);
        TestTrue(TEXT("partial composite activation remains prohibited"),
            Report.Resume.bPartialCompositeActivationProhibited);
        TestTrue(TEXT("runtime cache workers have an explicit bounded reservation"),
            Report.Memory.RuntimeMaximumWorkerJobs == 2
                && Report.Memory.RuntimeWorkerReservationBytes > 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTenKmResourcePreflightTenKmLimitTest,
    "SkiPreparation.M6.TenKmResourcePreflight.TenKmLedgerReportsCurrentImageryCap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTenKmResourcePreflightTenKmLimitTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    FTenKmResourcePreflightRequest Request = MakePreflightRequest(
        10'001U, 256ULL * 1024ULL * 1024ULL * 1024ULL);
    Request.ImageryMaximumBytesPerTile = 32ULL * 1024ULL;
    FTenKmResourcePreflightReport Report;
    const ETenKmResourcePreflightStatus Status = PlanTenKmResourcePreflight(Request, Report);
    TestEqual(TEXT("10 km estimate does not imply qualification when the current imagery cap blocks it"),
        static_cast<uint8>(Status),
        static_cast<uint8>(ETenKmResourcePreflightStatus::ComponentLimitExceeded));
    TestTrue(TEXT("the full required imagery tile count is retained in the report"),
        Report.ImageryPyramidTileCount > ImageryAcquisitionBudget{}.MaximumPyramidTiles);
    TestFalse(TEXT("the current imagery acquisition limit is reported as exceeded"),
        Report.bWithinCurrentImageryAcquisitionLimits);
    TestTrue(TEXT("the full 10 km canonical scratch is included"),
        Report.ScratchBytes.Bytes == 10'001ULL * 10'001ULL * 7ULL);
    TestTrue(TEXT("the supplied disk capacity passes despite the imagery component cap"),
        Report.bHasEnoughFreeDisk);
    TestFalse(TEXT("no static plan marks M6 qualified"), Report.bTenKmQualified);
    TestFalse(TEXT("durable canonical scratch resume is not claimed"),
        Report.Resume.bCanonicalScratchDurableResumeImplemented);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTenKmResourcePreflightFailClosedEvidenceTest,
    "SkiPreparation.M6.TenKmResourcePreflight.FailsClosedOnUnknownCoverageAndSize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTenKmResourcePreflightFailClosedEvidenceTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    FTenKmResourcePreflightReport Report;

    FTenKmResourcePreflightRequest NoElevationCoverage = MakePreflightRequest(
        2001U, 64ULL * 1024ULL * 1024ULL * 1024ULL);
    NoElevationCoverage.Elevation.HasVerifiedSiteCoverage = false;
    TestEqual(TEXT("unproven raster coverage rejects the plan"),
        static_cast<uint8>(PlanTenKmResourcePreflight(NoElevationCoverage, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::CoverageUnproven));

    FTenKmResourcePreflightRequest UnknownCover = MakePreflightRequest(
        2001U, 64ULL * 1024ULL * 1024ULL * 1024ULL);
    UnknownCover.WorldCoverSourceSizeCertainty = StorageSizeCertainty::Unknown;
    TestEqual(TEXT("unknown required COG size rejects the plan"),
        static_cast<uint8>(PlanTenKmResourcePreflight(UnknownCover, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::RequiredSizeUnknown));

    FTenKmResourcePreflightRequest UnknownCatalogSize = MakePreflightRequest(
        2001U, 64ULL * 1024ULL * 1024ULL * 1024ULL);
    UnknownCatalogSize.Elevation.ResolvedCandidateObjectBytes.Certainty = StorageSizeCertainty::Unknown;
    TestEqual(TEXT("missing exact resolver object sizes reject the plan"),
        static_cast<uint8>(PlanTenKmResourcePreflight(UnknownCatalogSize, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::RequiredSizeUnknown));

    FTenKmResourcePreflightRequest UnknownVectors = MakePreflightRequest(
        2001U, 64ULL * 1024ULL * 1024ULL * 1024ULL);
    UnknownVectors.bVectorCountEstimateAvailable = false;
    TestEqual(TEXT("unknown required vector size evidence rejects the plan"),
        static_cast<uint8>(PlanTenKmResourcePreflight(UnknownVectors, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::RequiredSizeUnknown));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTenKmResourcePreflightOverflowAndDiskTest,
    "SkiPreparation.M6.TenKmResourcePreflight.DetectsOverflowAndInsufficientDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTenKmResourcePreflightOverflowAndDiskTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    FTenKmResourcePreflightReport Report;

    FTenKmResourcePreflightRequest Overflow = MakePreflightRequest(
        2001U, 64ULL * 1024ULL * 1024ULL * 1024ULL);
    Overflow.Elevation.ResolvedSources[0].ExactObjectBytes =
        (std::numeric_limits<uint64>::max)();
    Overflow.Elevation.ResolvedCandidateObjectBytes.Bytes =
        (std::numeric_limits<uint64>::max)();
    TestEqual(TEXT("summing required bytes uses checked arithmetic"),
        static_cast<uint8>(PlanTenKmResourcePreflight(Overflow, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::ArithmeticOverflow));

    FTenKmResourcePreflightRequest TightDisk = MakePreflightRequest(2001U, 1ULL);
    TestEqual(TEXT("insufficient free disk rejects an otherwise complete estimate"),
        static_cast<uint8>(PlanTenKmResourcePreflight(TightDisk, Report)),
        static_cast<uint8>(ETenKmResourcePreflightStatus::InsufficientDisk));
    TestTrue(TEXT("disk failure still returns the calculated required amount"),
        Report.RequiredFreeDiskBytes.Bytes > 1ULL);
    TestFalse(TEXT("disk comparison reports insufficient capacity"),
        Report.bHasEnoughFreeDisk);
    return true;
}

#endif
