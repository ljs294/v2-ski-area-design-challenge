#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "Misc/AutomationTest.h"
#include "SkiPreparation/StagedTerrainAcquisition.h"

namespace
{
using namespace SkiPreparation;

FStagedTerrainAcquisitionRequest MakeRequest()
{
    FStagedTerrainAcquisitionRequest Request;
    Request.JobId = TEXT("staged-terrain-fixture");
    Request.Bounds = {-121.0, 46.0, -120.99, 46.01};
    Request.Width = 2;
    Request.Height = 2;
    return Request;
}

FString Digest(const TCHAR C = TEXT('a'))
{
    return FString::ChrN(64, C);
}

FStagedTerrainObjectPin ElevationPin(const FString& ETag = TEXT("\"s1m-v1\""))
{
    FStagedTerrainObjectPin Pin;
    Pin.ProductCode = TEXT("S1M");
    Pin.SourceId = TEXT("s1m-fixture-01");
    Pin.Url = TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/n26e19/n2620e1940/S1M_fixture.tif");
    Pin.ETag = ETag;
    Pin.ObjectBytes = 4096;
    return Pin;
}

FStagedTerrainSelectedSource SelectedSource()
{
    FStagedTerrainSelectedSource Source;
    Source.Candidate.Product = SkiDomain::ElevationProduct::S1M;
    Source.Candidate.SourceId = "s1m-fixture-01";
    Source.Candidate.SupportedHorizontalCrs = true;
    Source.Candidate.Navd88Proven = true;
    Source.Candidate.SupportedEncoding = true;
    Source.DownloadUrl = ElevationPin().Url;
    Source.HorizontalCrs = TEXT("EPSG:6350");
    Source.VerticalDatum = TEXT("NAVD88");
    Source.bSiteCoverageVerified = true;
    Source.CoverageEvidenceId = TEXT("coverage-fixture-01");
    return Source;
}

class FScriptedStagedAdapter final : public IStagedTerrainAcquisitionAdapter
{
public:
    TArray<EStagedTerrainStage> Calls;
    bool bCancelCanonicalOnce = false;
    bool bChangeCanonicalETag = false;
    bool bReportLibraryWrite = false;
    bool bReportMountainTransition = false;
    bool bExceedNetworkBudget = false;
    FString LastCheckpoint;
    FString CanonicalCheckpoint;

    FStagedTerrainStageResult RunStage(const EStagedTerrainStage Stage,
        const FStagedTerrainAcquisitionRequest& Request,
        const FStagedTerrainAcquisitionReceipt&,
        SkiNetGateway&, const TSharedRef<Cancellation>&,
        const FString& CheckpointToken) override
    {
        Calls.Add(Stage);
        LastCheckpoint = CheckpointToken;
        if (Stage == EStagedTerrainStage::CanonicalLod0) CanonicalCheckpoint = CheckpointToken;
        FStagedTerrainStageResult Result;
        Result.Outcome = EStagedTerrainStageOutcome::Succeeded;
        Result.OutputSha256 = Digest();
        Result.TotalUnits = 1;
        Result.CompletedUnits = 1;

        switch (Stage)
        {
        case EStagedTerrainStage::Catalog:
            Result.Proof.bSupportedGeography = true;
            Result.Proof.bSiteCoverageVerified = true;
            Result.SelectedSources.Add(SelectedSource());
            break;
        case EStagedTerrainStage::SourcePreflight:
        {
            const FStagedTerrainSelectedSource Source = SelectedSource();
            FStagedTerrainCogObservation Observation;
            Observation.SourceId = UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str());
            Observation.Url = Source.DownloadUrl;
            Observation.StrongETag = TEXT("\"s1m-v1\"");
            Observation.Preflight.bPassed = true;
            Observation.Preflight.bStrongETagPinned = true;
            Observation.Preflight.HorizontalCrs = Source.HorizontalCrs;
            Observation.Preflight.VerticalDatum = TEXT("NAVD88");
            Observation.Preflight.ObjectBytes = 4096;
            Result.CogObservations.Add(Observation);
            Result.ObjectPins.Add(ElevationPin());
            break;
        }
        case EStagedTerrainStage::CanonicalLod0:
            if (bCancelCanonicalOnce)
            {
                bCancelCanonicalOnce = false;
                Result.Outcome = EStagedTerrainStageOutcome::Cancelled;
                Result.FailureCode = TEXT("fixture-cancelled");
                Result.CheckpointToken = TEXT("scratch-tile=0,0");
                return Result;
            }
            Result.Proof.bCanonicalLod0IsOneMetre = true;
            Result.Proof.bCanonicalLod0StoreSealed = true;
            Result.Proof.CanonicalLod0Samples = static_cast<uint64>(Request.Width) * Request.Height;
            Result.ObjectPins.Add(ElevationPin(bChangeCanonicalETag
                ? TEXT("\"s1m-v2\"") : TEXT("\"s1m-v1\"")));
            Result.Usage.ScratchBytesPresent = 28;
            break;
        case EStagedTerrainStage::CoarseLodCopy:
            Result.Proof.bCoarseLodsAreIndexedCopies = true;
            Result.Proof.bCoarseLodsBitExactVerified = true;
            break;
        case EStagedTerrainStage::WorldCover:
        {
            Result.Proof.bWorldCoverWindowVerified = true;
            FStagedTerrainObjectPin Pin;
            Pin.ProductCode = TEXT("WorldCover");
            Pin.SourceId = TEXT("worldcover-fixture-01");
            Pin.Url = TEXT("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N46W122_Map.tif");
            Pin.ETag = TEXT("\"wc-v1\"");
            Pin.ObjectBytes = 4096;
            Result.ObjectPins.Add(Pin);
            break;
        }
        case EStagedTerrainStage::TerrainCoreStaging:
            Result.Proof.bTerrainCoreStagedOnly = true;
            Result.Proof.bLibraryEntryWritten = bReportLibraryWrite;
            Result.Proof.bMountainTransitioned = bReportMountainTransition;
            Result.Usage.StagedTerrainBytesPresent = 1024;
            break;
        case EStagedTerrainStage::TerrainCoreVerification:
            Result.Proof.bTerrainCoreVerificationPassed = true;
            break;
        case EStagedTerrainStage::Complete:
        default:
            Result.Outcome = EStagedTerrainStageOutcome::FatalFailure;
            Result.FailureCode = TEXT("unexpected-stage");
            break;
        }
        if (bExceedNetworkBudget && Stage == EStagedTerrainStage::Catalog)
            Result.Usage.TransferredBytes = Request.Limits.MaxTransferredBytes + 1;
        return Result;
    }
};

bool RunToCompletion(FStagedTerrainAcquisitionJob& Job, FScriptedStagedAdapter& Adapter,
    FString& Error)
{
    for (int32 Index = 0; Index < 8 && !Job.IsComplete(); ++Index)
    {
        const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
        const bool bDone = Job.Run(Adapter, CancelToken, 1, Error);
        if (bDone) return true;
        if (Error != TEXT("STAGED_TERRAIN_STAGE_CALL_BUDGET_REACHED")) return false;
    }
    return Job.IsComplete();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionOrderingTest,
    "SkiPreparation.M4.StagedTerrain.StageOrderingAndNoActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionOrderingTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionJob Job;
    FString Error;
    TestTrue(TEXT("request is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    TestTrue(TEXT("job reaches verified staged terrain"), RunToCompletion(Job, Adapter, Error));

    const TArray<EStagedTerrainStage> Expected{
        EStagedTerrainStage::Catalog,
        EStagedTerrainStage::SourcePreflight,
        EStagedTerrainStage::CanonicalLod0,
        EStagedTerrainStage::CoarseLodCopy,
        EStagedTerrainStage::WorldCover,
        EStagedTerrainStage::TerrainCoreStaging,
        EStagedTerrainStage::TerrainCoreVerification,
    };
    TestEqual(TEXT("every M4 stage ran"), Adapter.Calls.Num(), Expected.Num());
    for (int32 Index = 0; Index < FMath::Min(Adapter.Calls.Num(), Expected.Num()); ++Index)
        TestTrue(TEXT("stages stay in contract order"), Adapter.Calls[Index] == Expected[Index]);
    const FStagedTerrainAcquisitionReceipt& Receipt = Job.GetReceipt();
    TestEqual(TEXT("receipt reaches complete state"), static_cast<int32>(Receipt.State),
        static_cast<int32>(EStagedTerrainJobState::Complete));
    TestTrue(TEXT("terrain component is staged"), Receipt.bTerrainCoreStaged);
    TestTrue(TEXT("staged component verified"), Receipt.bTerrainCoreVerified);
    TestFalse(TEXT("M4 never writes the installed library"), Receipt.bLibraryEntryWritten);
    TestFalse(TEXT("M4 never transitions to Mountain"), Receipt.bMountainTransitioned);

    FStagedTerrainAcquisitionReceipt ForgedLibraryReceipt = Receipt;
    ForgedLibraryReceipt.bLibraryEntryWritten = true;
    FStagedTerrainAcquisitionJob RejectedLibraryReceipt;
    TestFalse(TEXT("restoring a receipt that claims a library write is rejected"),
        FStagedTerrainAcquisitionJob::Restore(ForgedLibraryReceipt, RejectedLibraryReceipt, Error));
    FStagedTerrainAcquisitionReceipt ForgedMountainReceipt = Receipt;
    ForgedMountainReceipt.bMountainTransitioned = true;
    FStagedTerrainAcquisitionJob RejectedMountainReceipt;
    TestFalse(TEXT("restoring a receipt that claims a Mountain transition is rejected"),
        FStagedTerrainAcquisitionJob::Restore(ForgedMountainReceipt, RejectedMountainReceipt, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionResumeTest,
    "SkiPreparation.M4.StagedTerrain.CancelResumeReceipt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionResumeTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionJob Job;
    FString Error;
    TestTrue(TEXT("request is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    Adapter.bCancelCanonicalOnce = true;
    TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    TestFalse(TEXT("sampling cancellation pauses the job"), Job.Run(Adapter, CancelToken, 3, Error));
    TestEqual(TEXT("same stage is retained for resume"), static_cast<int32>(Job.GetNextStage()),
        static_cast<int32>(EStagedTerrainStage::CanonicalLod0));
    TestEqual(TEXT("resume cursor is durable in receipt"), Job.GetReceipt().CheckpointToken, FString(TEXT("scratch-tile=0,0")));

    FString Json;
    TestTrue(TEXT("paused receipt serializes"), FStagedTerrainAcquisitionJob::SerializeReceipt(
        Job.GetReceipt(), Json, Error));
    FStagedTerrainAcquisitionReceipt RestoredReceipt;
    TestTrue(TEXT("paused receipt deserializes"), FStagedTerrainAcquisitionJob::DeserializeReceipt(
        Json, RestoredReceipt, Error));
    FStagedTerrainAcquisitionJob Resumed;
    TestTrue(TEXT("deserialized receipt restores"), FStagedTerrainAcquisitionJob::Restore(
        RestoredReceipt, Resumed, Error));
    Adapter.Calls.Reset();
    Adapter.CanonicalCheckpoint.Reset();
    TestTrue(TEXT("resumed job completes"), RunToCompletion(Resumed, Adapter, Error));
    TestEqual(TEXT("adapter receives the saved cursor"), Adapter.CanonicalCheckpoint,
        FString(TEXT("scratch-tile=0,0")));
    TestEqual(TEXT("resumed run begins at canonical LOD0"), static_cast<int32>(Adapter.Calls[0]),
        static_cast<int32>(EStagedTerrainStage::CanonicalLod0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionEtagTest,
    "SkiPreparation.M4.StagedTerrain.RejectsStaleETag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionEtagTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionJob Job;
    FString Error;
    TestTrue(TEXT("request is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    Adapter.bChangeCanonicalETag = true;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    TestFalse(TEXT("new object version is rejected"), Job.Run(Adapter, CancelToken, 3, Error));
    TestEqual(TEXT("ETag mutation fails closed"), Job.GetReceipt().FailureCode,
        FString(TEXT("STAGED_TERRAIN_ETAG_OR_OBJECT_IDENTITY_CHANGED")));
    TestEqual(TEXT("failed sampling stage is not advanced"), static_cast<int32>(Job.GetNextStage()),
        static_cast<int32>(EStagedTerrainStage::CanonicalLod0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionNoInstallTest,
    "SkiPreparation.M4.StagedTerrain.RejectsLibraryActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionNoInstallTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionJob Job;
    FString Error;
    TestTrue(TEXT("request is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    Adapter.bReportLibraryWrite = true;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    TestFalse(TEXT("library write attestation is rejected"), Job.Run(Adapter, CancelToken, 6, Error));
    TestEqual(TEXT("staging stage is not accepted"), static_cast<int32>(Job.GetNextStage()),
        static_cast<int32>(EStagedTerrainStage::TerrainCoreStaging));
    TestFalse(TEXT("receipt has no installed library entry"), Job.GetReceipt().bLibraryEntryWritten);
    TestFalse(TEXT("receipt has no Mountain transition"), Job.GetReceipt().bMountainTransitioned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionNoMountainTest,
    "SkiPreparation.M4.StagedTerrain.RejectsMountainTransition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionNoMountainTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionJob Job;
    FString Error;
    TestTrue(TEXT("request is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    Adapter.bReportMountainTransition = true;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    TestFalse(TEXT("Mountain transition attestation is rejected"), Job.Run(Adapter, CancelToken, 6, Error));
    TestEqual(TEXT("staging stage is not accepted"), static_cast<int32>(Job.GetNextStage()),
        static_cast<int32>(EStagedTerrainStage::TerrainCoreStaging));
    TestFalse(TEXT("receipt has no installed library entry"), Job.GetReceipt().bLibraryEntryWritten);
    TestFalse(TEXT("receipt has no Mountain transition"), Job.GetReceipt().bMountainTransitioned);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStagedTerrainAcquisitionLimitsTest,
    "SkiPreparation.M4.StagedTerrain.EnforcesResourceLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FStagedTerrainAcquisitionLimitsTest::RunTest(const FString&)
{
    FStagedTerrainAcquisitionRequest TooSmall = MakeRequest();
    TooSmall.Limits.MaxScratchStorageBytes = 27;
    FStagedTerrainAcquisitionJob Rejected;
    FString Error;
    TestFalse(TEXT("scratch preflight rejects a tight disk cap"),
        FStagedTerrainAcquisitionJob::Create(TooSmall, Rejected, Error));
    TestEqual(TEXT("failure identifies resource limits"), Error,
        FString(TEXT("STAGED_TERRAIN_RESOURCE_LIMIT_INVALID")));

    FStagedTerrainAcquisitionJob Job;
    TestTrue(TEXT("normal resource plan is accepted"), FStagedTerrainAcquisitionJob::Create(MakeRequest(), Job, Error));
    FScriptedStagedAdapter Adapter;
    Adapter.bExceedNetworkBudget = true;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    TestFalse(TEXT("aggregate transfer overrun fails closed"), Job.Run(Adapter, CancelToken, 1, Error));
    TestEqual(TEXT("network usage cap is reported"), Job.GetReceipt().FailureCode,
        FString(TEXT("STAGED_TERRAIN_RESOURCE_BUDGET_EXCEEDED")));
    TestEqual(TEXT("catalog cannot complete over budget"), static_cast<int32>(Job.GetNextStage()),
        static_cast<int32>(EStagedTerrainStage::Catalog));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
