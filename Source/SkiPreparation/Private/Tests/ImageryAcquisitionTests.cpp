#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "SkiPreparation/ImageryAcquisition.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainAcquisition.h"

#include <map>
#include <string>
#include <vector>

namespace
{
constexpr int32 FixtureTilePixels = 256;
constexpr int32 FixtureTileBytes = FixtureTilePixels * FixtureTilePixels * 4;

SkiDomain::TerrainCoreManifest MakeTerrainCore()
{
    SkiDomain::TerrainCoreManifest Core;
    Core.ContentId = std::string(64, 'a');
    Core.Width = FixtureTilePixels;
    Core.Height = FixtureTilePixels;
    Core.DeliveredEastSpacingM = 1.0;
    Core.DeliveredNorthSpacingM = 1.0;
    Core.LocalOrigin = {47.2, -121.4, 1500.0};
    Core.SampleCenterBounds = {-127.5, -127.5, 127.5, 127.5};
    SkiDomain::ComputeTerrainCoreBounds(Core.Width, Core.Height,
        Core.DeliveredEastSpacingM, Core.DeliveredNorthSpacingM,
        Core.SampleCenterBounds, Core.OuterBounds);
    return Core;
}

TArray<uint8> EncodeFixture(const EImageFormat Format, const uint8 Red,
    const uint8 Green, const uint8 Blue, const uint8 Alpha)
{
    TArray<uint8> Pixels;
    Pixels.SetNumUninitialized(FixtureTileBytes);
    for (int32 Pixel = 0; Pixel < FixtureTilePixels * FixtureTilePixels; ++Pixel)
    {
        const int32 Offset = Pixel * 4;
        Pixels[Offset] = Blue;
        Pixels[Offset + 1] = Green;
        Pixels[Offset + 2] = Red;
        Pixels[Offset + 3] = Alpha;
    }
    IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(Format);
    if (!Wrapper.IsValid()
        || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num(), FixtureTilePixels,
            FixtureTilePixels, ERGBFormat::BGRA, 8))
    {
        return {};
    }
    const TArray64<uint8>& Encoded = Wrapper->GetCompressed(95);
    TArray<uint8> Result;
    if (!Encoded.IsEmpty() && Encoded.Num() <= MAX_int32)
        Result.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
    return Result;
}

class FScriptedImageryTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    TArray<uint8> SourceBytes;
    FString ContentType = TEXT("image/jpeg");
    int32 Status = 200;
    bool bCancelDuringRequest = false;
    std::vector<FString> Urls;
    std::vector<SkiPreparation::HttpAcquisitionRequest> Requests;

    SkiPreparation::HttpAcquisitionResult Get(
        const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation) override
    {
        SkiPreparation::HttpAcquisitionResult Result;
        Result.Attempt = Request.Attempt;
        Result.HttpStatus = Status;
        Result.ContentType = ContentType;
        Result.Bytes.Append(SourceBytes);
        Result.BytesReceived = Result.Bytes.Num();
        if (Status == 200) Result.FailureReason = SkiPreparation::TransportFailureReason::None;
        else Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
        Urls.push_back(Request.Url);
        SkiPreparation::HttpAcquisitionRequest Snapshot = Request;
        Snapshot.BackendLifetime.Reset();
        Requests.push_back(MoveTemp(Snapshot));
        if (bCancelDuringRequest)
            Cancellation->Cancel();
        return Result;
    }
};

class FMemoryImageryWriter final : public SkiPreparation::IImageryPyramidAssetWriter
{
public:
    std::map<std::string, TArray<uint8>> Assets;
    bool bFailWrite = false;
    int32 Calls = 0;

    bool Write(const std::string& RelativePath, const TArrayView<const uint8> Bytes,
        const SkiPreparation::Cancellation& CancellationValue, FString& OutError) override
    {
        ++Calls;
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("cancelled");
            return false;
        }
        if (bFailWrite)
        {
            OutError = TEXT("scripted writer failure");
            return false;
        }
        TArray<uint8> Copy;
        Copy.Append(Bytes.GetData(), Bytes.Num());
        Assets[RelativePath] = MoveTemp(Copy);
        return true;
    }
};

bool RunScriptedImagery(FScriptedImageryTransport& Transport, FMemoryImageryWriter& Writer,
    SkiPreparation::ImageryAcquisitionReport& Report,
    const SkiPreparation::ImageryAcquisitionBudget& Budget = {})
{
    const TSharedRef<SkiPreparation::Cancellation> Cancellation =
        MakeShared<SkiPreparation::Cancellation>();
    return SkiPreparation::AcquireUsgsImageryPyramidForTest(MakeTerrainCore(),
        Transport, Writer, Cancellation, Report, Budget);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryAcquisitionProductionGatewayEntryPointCancellationTest,
    "MountainPlanner.M5.ImageryAcquisition.ProductionGatewayEntryPointCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryAcquisitionProductionGatewayEntryPointCancellationTest::RunTest(const FString&)
{
    FMemoryImageryWriter Writer;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation =
        MakeShared<SkiPreparation::Cancellation>();
    Cancellation->Cancel();
    SkiPreparation::ImageryAcquisitionReport Report;

    TestFalse(TEXT("pre-cancelled production acquisition exits before gateway work"),
        SkiPreparation::AcquireUsgsImageryPyramid(MakeTerrainCore(), Writer,
            Cancellation, Report));
    TestEqual(TEXT("production entrypoint reports cancellation explicitly"),
        static_cast<uint8>(Report.Error),
        static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::Cancelled));
    TestEqual(TEXT("pre-cancellation issues no gateway requests"), Report.Requests, 0U);
    TestEqual(TEXT("pre-cancellation writes no staged assets"), Writer.Calls, 0);
    TestTrue(TEXT("pre-cancellation leaves the verified output manifest empty"),
        Report.Manifest.Tiles.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryAcquisitionScriptedReprojectionTest,
    "MountainPlanner.M5.ImageryAcquisition.ScriptedReprojectionAndHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryAcquisitionScriptedReprojectionTest::RunTest(const FString&)
{
    FScriptedImageryTransport FirstTransport;
    FirstTransport.SourceBytes = EncodeFixture(EImageFormat::JPEG, 226, 61, 24, 255);
    TestTrue(TEXT("scripted 256 by 256 USGS source tile is encoded"),
        !FirstTransport.SourceBytes.IsEmpty());
    FMemoryImageryWriter FirstWriter;
    SkiPreparation::ImageryAcquisitionReport First;
    if (!RunScriptedImagery(FirstTransport, FirstWriter, First))
    {
        AddError(TEXT("Scripted imagery bridge did not produce a complete TerrainCore pyramid."));
        return false;
    }
    TestTrue(TEXT("acquisition report is successful"), First.Ok());
    TestEqual(TEXT("the 256 by 256 core has one JPEG per TerrainCore LOD"),
        First.Manifest.Tiles.Num(), 5);
    TestEqual(TEXT("exact output is a five-tile image pyramid"),
        FirstWriter.Assets.size(), static_cast<size_t>(5));
    TestTrue(TEXT("a bounded source request was issued"), First.Requests > 0);
    TestTrue(TEXT("transfer bytes are accounted"), First.SourceBytes > 0);
    TestTrue(TEXT("output bytes are accounted"), First.OutputBytes > 0);
    TestTrue(TEXT("estimated live buffers stay under the declared working-memory cap"),
        First.BoundedWorkingMemoryBytes
            <= SkiPreparation::ImageryAcquisitionBudget{}.MaximumWorkingMemoryBytes);
    TestTrue(TEXT("every source URL is the allow-listed USGS imagery tile template"),
        !FirstTransport.Urls.empty());
    for (const FString& Url : FirstTransport.Urls)
    {
        FString Reason;
        TestTrue(TEXT("generated USGS tile URL passes gateway policy"),
            SkiPreparation::SkiNetGateway::ValidateUrl(Url, Reason));
        TestTrue(TEXT("tile request uses zoom 17"), Url.Contains(TEXT("/tile/17/")));
    }
    for (const SkiPreparation::HttpAcquisitionRequest& Request : FirstTransport.Requests)
    {
        TestTrue(TEXT("transport response ceiling never exceeds the configured tile cap"),
            Request.MaximumResponseBytes <= SkiPreparation::ImageryAcquisitionBudget{}.MaximumSourceTileBytes);
    }

    TestTrue(TEXT("the generated manifest verifies exact TerrainCore keys and hashes"),
        SkiPreparation::ValidateImageryPyramidManifest(MakeTerrainCore(), First.Manifest).Ok());
    TestEqual(TEXT("source attribution is recorded"),
        FString(UTF8_TO_TCHAR(First.Manifest.Source.Attribution.c_str())),
        FString(TEXT("USGS The National Map")));
    TestEqual(TEXT("source terms are recorded"),
        FString(UTF8_TO_TCHAR(First.Manifest.Source.TermsUrl.c_str())),
        FString(TEXT("https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits")));

    const auto FirstAsset = FirstWriter.Assets.find(First.Manifest.Tiles[0].Path);
    TestTrue(TEXT("first LOD asset exists at its canonical path"), FirstAsset != FirstWriter.Assets.end());
    if (FirstAsset == FirstWriter.Assets.end()) return false;
    {
        IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
            TEXT("ImageWrapper"));
        const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::JPEG);
        TArray<uint8> Decoded;
        TestTrue(TEXT("stored target is a decodable JPEG"), Wrapper.IsValid()
            && Wrapper->SetCompressed(FirstAsset->second.GetData(), FirstAsset->second.Num())
            && Wrapper->GetWidth() == FixtureTilePixels
            && Wrapper->GetHeight() == FixtureTilePixels
            && Wrapper->GetRaw(ERGBFormat::BGRA, 8, Decoded));
        if (Decoded.Num() == FixtureTileBytes)
        {
            TestTrue(TEXT("source color survived the geographic resampling"),
                Decoded[2] > 180 && Decoded[1] < 110 && Decoded[0] < 80);
        }
    }

    FScriptedImageryTransport SecondTransport;
    SecondTransport.SourceBytes = FirstTransport.SourceBytes;
    FMemoryImageryWriter SecondWriter;
    SkiPreparation::ImageryAcquisitionReport Second;
    if (!RunScriptedImagery(SecondTransport, SecondWriter, Second))
    {
        AddError(TEXT("Repeated scripted imagery acquisition did not complete."));
        return false;
    }
    TestEqual(TEXT("identical encoded input produces identical LOD0 content hash"),
        FString(UTF8_TO_TCHAR(First.Manifest.Tiles[0].Sha256.c_str())),
        FString(UTF8_TO_TCHAR(Second.Manifest.Tiles[0].Sha256.c_str())));
    TestTrue(TEXT("identical encoded input produces identical asset bytes"),
        FirstWriter.Assets.at(First.Manifest.Tiles[0].Path)
            == SecondWriter.Assets.at(Second.Manifest.Tiles[0].Path));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FImageryAcquisitionBoundedFailuresTest,
    "MountainPlanner.M5.ImageryAcquisition.BudgetsCancellationAndNoData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageryAcquisitionBoundedFailuresTest::RunTest(const FString&)
{
    const TArray<uint8> OpaqueJpeg = EncodeFixture(EImageFormat::JPEG, 226, 61, 24, 255);
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        SkiPreparation::ImageryAcquisitionBudget Budget;
        Budget.MaximumPyramidTiles = 4;
        TestFalse(TEXT("oversized TerrainCore pyramid is rejected before requests"),
            RunScriptedImagery(Transport, Writer, Report, Budget));
        TestEqual(TEXT("tile count has a stable failure code"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::TooManyPyramidTiles));
        TestEqual(TEXT("rejected tile plan starts no source requests"), Report.Requests, 0U);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        SkiPreparation::ImageryAcquisitionBudget Budget;
        Budget.MaximumDecodedSourceBytes = static_cast<uint64>(FixtureTileBytes);
        TestFalse(TEXT("source working set exceeding decoded memory is rejected"),
            RunScriptedImagery(Transport, Writer, Report, Budget));
        TestEqual(TEXT("memory limit has a stable failure code"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::DecodedMemoryBudgetExceeded));
        TestEqual(TEXT("impossible source working set is found before requests"), Report.Requests, 0U);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        Transport.Status = 404;
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        TestFalse(TEXT("a missing required USGS tile prevents a partial pyramid"),
            RunScriptedImagery(Transport, Writer, Report));
        TestEqual(TEXT("missing imagery has an explicit no-data failure"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::MissingSourceTile));
        TestEqual(TEXT("missing tile is reported as HTTP 404"), Report.HttpStatus, 404);
        TestEqual(TEXT("missing tile emits no JPEG output"), Writer.Calls, 0);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = EncodeFixture(EImageFormat::PNG, 0, 0, 0, 0);
        Transport.ContentType = TEXT("image/png");
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        TestFalse(TEXT("fully transparent source pixels cannot become invented opaque imagery"),
            RunScriptedImagery(Transport, Writer, Report));
        TestEqual(TEXT("transparent coverage has an explicit no-data failure"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::NoDataInOutputTile));
        TestTrue(TEXT("report counts uncovered target samples"), Report.NoDataSamples > 0);
        TestEqual(TEXT("no-data output tile is not sent to storage"), Writer.Calls, 0);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        SkiPreparation::ImageryAcquisitionBudget Budget;
        Budget.MaximumSourceRequests = 1;
        TestFalse(TEXT("source retrieval stops at its network request budget"),
            RunScriptedImagery(Transport, Writer, Report, Budget));
        TestEqual(TEXT("network request exhaustion has an explicit code"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::RequestBudgetExceeded));
        TestEqual(TEXT("request count does not exceed its cap"), Report.Requests, 1U);
        TestEqual(TEXT("request budget exhaustion writes no partial output tile"), Writer.Calls, 0);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        FMemoryImageryWriter Writer;
        SkiPreparation::ImageryAcquisitionReport Report;
        SkiPreparation::ImageryAcquisitionBudget Budget;
        Budget.MaximumSourceTransferBytes = 32;
        TestFalse(TEXT("source bytes beyond the total transfer allowance are rejected"),
            RunScriptedImagery(Transport, Writer, Report, Budget));
        TestEqual(TEXT("transfer cap has a stable failure code"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::TransferBudgetExceeded));
        TestEqual(TEXT("transport receives the remaining total byte allowance"),
            Transport.Requests.empty() ? 0ULL : Transport.Requests[0].MaximumResponseBytes, 32ULL);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        FMemoryImageryWriter Writer;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation =
            MakeShared<SkiPreparation::Cancellation>();
        Cancellation->Cancel();
        SkiPreparation::ImageryAcquisitionReport Report;
        TestFalse(TEXT("pre-cancelled request does no work"),
            SkiPreparation::AcquireUsgsImageryPyramidForTest(MakeTerrainCore(),
                Transport, Writer, Cancellation, Report));
        TestEqual(TEXT("pre-cancel status is explicit"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::Cancelled));
        TestEqual(TEXT("pre-cancel issues no request"), Report.Requests, 0U);
    }
    {
        FScriptedImageryTransport Transport;
        Transport.SourceBytes = OpaqueJpeg;
        Transport.bCancelDuringRequest = true;
        FMemoryImageryWriter Writer;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation =
            MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::ImageryAcquisitionReport Report;
        TestFalse(TEXT("cancellation during retrieval prevents the staged pyramid"),
            SkiPreparation::AcquireUsgsImageryPyramidForTest(MakeTerrainCore(),
                Transport, Writer, Cancellation, Report));
        TestEqual(TEXT("in-flight cancellation is observed"),
            static_cast<uint8>(Report.Error),
            static_cast<uint8>(SkiPreparation::ImageryAcquisitionError::Cancelled));
        TestEqual(TEXT("cancelled retrieval emits no tile"), Writer.Calls, 0);
    }
    return true;
}

#endif
