#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SkiPreparation/PlaceSearch.h"
#include "SkiPreparation/SkiNetGateway.h"

#include <atomic>

namespace
{
class FScriptedPlaceSearchTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    TArray<SkiPreparation::HttpAcquisitionRequest> Requests;
    TArray<double> RequestStartTimes;
    TArray<uint8> ResponseBytes;
    int32 HttpStatus = 200;
    FString ContentType = TEXT("application/json; charset=utf-8");
    bool bCancelOnRequest = false;
    bool bReturnOversized = false;

    SkiPreparation::HttpAcquisitionResult Get(
        const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation) override
    {
        Requests.Add(Request);
        RequestStartTimes.Add(FPlatformTime::Seconds());
        if (bCancelOnRequest) Cancellation->Cancel();

        SkiPreparation::HttpAcquisitionResult Result;
        Result.HttpStatus = HttpStatus;
        Result.ContentType = ContentType;
        Result.Bytes = ResponseBytes;
        if (bReturnOversized)
        {
            Result.Bytes.SetNum(static_cast<int32>(Request.MaximumResponseBytes + 1));
            Result.BytesReceived = Result.Bytes.Num();
        }
        else
        {
            Result.BytesReceived = Result.Bytes.Num();
        }
        return Result;
    }
};

TArray<uint8> Utf8Bytes(const FString& Text)
{
    const FTCHARToUTF8 Utf8(*Text);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

FString SixPlaceFixture()
{
    FString Json = TEXT("[");
    for (int32 Index = 0; Index < 6; ++Index)
    {
        if (Index > 0) Json += TEXT(",");
        const FString Name = Index == 1 ? FString() : FString::Printf(TEXT("Mountain %d"), Index + 1);
        const FString NameField = Name.IsEmpty() ? FString() : FString::Printf(TEXT("\"name\":\"%s\","), *Name);
        Json += FString::Printf(
            TEXT("{%s\"display_name\":\"Mountain %d, Example State, Example Country\",\"lat\":\"46.5\",\"lon\":\"-121.5\",\"boundingbox\":[\"46.0\",\"47.0\",\"-122.0\",\"-121.0\"],\"address\":{\"state\":\"Example State\",\"country\":\"Example Country\"}}"),
            *NameField, Index + 1);
    }
    Json += TEXT("]");
    return Json;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceSearchNormalizationCacheAndBoundsTest,
    "MountainPlanner.P1.Preparation.PlaceSearch.NormalizationCacheAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaceSearchNormalizationCacheAndBoundsTest::RunTest(const FString&)
{
    const TSharedPtr<FScriptedPlaceSearchTransport, ESPMode::ThreadSafe> Transport =
        MakeShared<FScriptedPlaceSearchTransport, ESPMode::ThreadSafe>();
    Transport->ResponseBytes = Utf8Bytes(SixPlaceFixture());
    SkiPreparation::NominatimSearchProvider Provider(Transport);
    const TSharedRef<SkiPreparation::Cancellation> Cancellation =
        MakeShared<SkiPreparation::Cancellation>();
    TArray<SkiPreparation::PlaceSearchResult> Results;
    FString Error;

    TestFalse(TEXT("Empty query is rejected"), Provider.Search(TEXT("   "), Cancellation, Results, Error));
    TestEqual(TEXT("Empty query reports stable reason"), Error, FString(TEXT("PLACE_SEARCH_EMPTY_QUERY")));
    TestFalse(TEXT("Control characters are rejected"), Provider.Search(TEXT("Crystal\nMountain"), Cancellation, Results, Error));
    TestEqual(TEXT("Control input reports stable reason"), Error, FString(TEXT("PLACE_SEARCH_INVALID_QUERY")));
    TestFalse(TEXT("Input that would violate gateway URL policy is rejected"),
        Provider.Search(TEXT("bad\\search"), Cancellation, Results, Error));
    TestEqual(TEXT("Gateway-unsafe input reports stable reason"), Error, FString(TEXT("PLACE_SEARCH_INVALID_QUERY")));
    TestFalse(TEXT("Long query is rejected"), Provider.Search(FString::ChrN(201, TEXT('x')), Cancellation, Results, Error));
    TestEqual(TEXT("Long query reports stable reason"), Error, FString(TEXT("PLACE_SEARCH_QUERY_TOO_LONG")));
    TestEqual(TEXT("Invalid queries never reach the transport"), Transport->Requests.Num(), 0);

    TestTrue(TEXT("Valid query returns a parsed result set"),
        Provider.Search(TEXT("  Crystal   + Ridge & Lake  "), Cancellation, Results, Error));
    TestEqual(TEXT("Provider caps response at five places"), Results.Num(), 5);
    TestEqual(TEXT("Name is normalized"), Results[0].Name, FString(TEXT("Mountain 1")));
    TestEqual(TEXT("Point latitude is normalized"), Results[0].Point.LatitudeDeg, 46.5);
    TestEqual(TEXT("Point longitude is normalized"), Results[0].Point.LongitudeDeg, -121.5);
    TestEqual(TEXT("Nominatim south bound maps to SouthDeg"), Results[0].BoundingBox.SouthDeg, 46.0);
    TestEqual(TEXT("Nominatim north bound maps to NorthDeg"), Results[0].BoundingBox.NorthDeg, 47.0);
    TestEqual(TEXT("Nominatim west bound maps to WestDeg"), Results[0].BoundingBox.WestDeg, -122.0);
    TestEqual(TEXT("Nominatim east bound maps to EastDeg"), Results[0].BoundingBox.EastDeg, -121.0);
    TestEqual(TEXT("Region is normalized from address state"), Results[0].Region, FString(TEXT("Example State")));
    TestEqual(TEXT("Country is normalized from address"), Results[0].Country, FString(TEXT("Example Country")));
    TestEqual(TEXT("Missing feature name falls back to display-name prefix"), Results[1].Name, FString(TEXT("Mountain 2")));
    TestEqual(TEXT("One request is issued for explicit submit"), Transport->Requests.Num(), 1);
    TestTrue(TEXT("Search URL stays inside the gateway allow-list"),
        SkiPreparation::SkiNetGateway::ValidateUrl(Transport->Requests[0].Url, Error));
    TestTrue(TEXT("Query whitespace and reserved characters are safely encoded"),
        Transport->Requests[0].Url.Contains(TEXT("q=Crystal%20%2B%20Ridge%20%26%20Lake")));
    TestEqual(TEXT("Nominatim response cap is applied to the request"),
        Transport->Requests[0].MaximumResponseBytes, uint64(256 * 1024));

    TArray<SkiPreparation::PlaceSearchResult> CachedResults;
    TestTrue(TEXT("Normalized equivalent query is served from cache"),
        Provider.Search(TEXT("crystal + ridge & lake"), Cancellation, CachedResults, Error));
    TestEqual(TEXT("Cache preserves the normalized result set"), CachedResults.Num(), 5);
    TestEqual(TEXT("Cache prevents a second provider request"), Transport->Requests.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaceSearchPacingCancellationAndBoundsTest,
    "MountainPlanner.P1.Preparation.PlaceSearch.PacingCancellationAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPlaceSearchPacingCancellationAndBoundsTest::RunTest(const FString&)
{
    const TSharedPtr<FScriptedPlaceSearchTransport, ESPMode::ThreadSafe> Transport =
        MakeShared<FScriptedPlaceSearchTransport, ESPMode::ThreadSafe>();
    Transport->ResponseBytes = Utf8Bytes(TEXT("[]"));
    SkiPreparation::NominatimSearchProvider FirstProvider(Transport);
    SkiPreparation::NominatimSearchProvider SecondProvider(Transport);
    const TSharedRef<SkiPreparation::Cancellation> FirstCancellation =
        MakeShared<SkiPreparation::Cancellation>();
    const TSharedRef<SkiPreparation::Cancellation> SecondCancellation =
        MakeShared<SkiPreparation::Cancellation>();
    TArray<SkiPreparation::PlaceSearchResult> Results;
    FString Error;

    TestTrue(TEXT("First provider search succeeds"),
        FirstProvider.Search(TEXT("pacing probe one"), FirstCancellation, Results, Error));
    TestTrue(TEXT("Separate provider instance shares the process pacing gate"),
        SecondProvider.Search(TEXT("pacing probe two"), SecondCancellation, Results, Error));
    TestEqual(TEXT("Two explicit submissions make two requests"), Transport->Requests.Num(), 2);
    TestTrue(TEXT("Process-wide request starts are separated by at least one second"),
        Transport->RequestStartTimes[1] - Transport->RequestStartTimes[0] >= 0.99);

    const int32 RequestsBeforeOversize = Transport->Requests.Num();
    Transport->bReturnOversized = true;
    TestFalse(TEXT("Provider independently rejects an oversized transport result"),
        FirstProvider.Search(TEXT("oversize probe"), MakeShared<SkiPreparation::Cancellation>(), Results, Error));
    TestEqual(TEXT("Oversized response has a stable reason"), Error, FString(TEXT("PLACE_SEARCH_RESPONSE_TOO_LARGE")));
    TestEqual(TEXT("Oversized response still uses one bounded request"),
        Transport->Requests.Num(), RequestsBeforeOversize + 1);
    Transport->bReturnOversized = false;

    Transport->bCancelOnRequest = true;
    const TSharedRef<SkiPreparation::Cancellation> DuringRequestCancellation =
        MakeShared<SkiPreparation::Cancellation>();
    TestFalse(TEXT("Cancellation received during transport suppresses result publication"),
        FirstProvider.Search(TEXT("cancel in transport"), DuringRequestCancellation, Results, Error));
    TestEqual(TEXT("Transport cancellation is reported"), Error, FString(TEXT("PLACE_SEARCH_CANCELLED")));
    Transport->bCancelOnRequest = false;

    const int32 RequestsBeforeWaitCancel = Transport->Requests.Num();
    const TSharedRef<SkiPreparation::Cancellation> WaitCancellation =
        MakeShared<SkiPreparation::Cancellation>();
    TFuture<void> CancelWait = Async(EAsyncExecution::ThreadPool, [WaitCancellation]()
    {
        FPlatformProcess::SleepNoStats(0.025F);
        WaitCancellation->Cancel();
    });
    const double CancelBegan = FPlatformTime::Seconds();
    TestFalse(TEXT("Cancellation interrupts the one-second pacing wait"),
        SecondProvider.Search(TEXT("cancel while paced"), WaitCancellation, Results, Error));
    CancelWait.Get();
    TestEqual(TEXT("Cancelled pacing wait makes no network request"),
        Transport->Requests.Num(), RequestsBeforeWaitCancel);
    TestTrue(TEXT("Pacing cancellation is acknowledged quickly"),
        FPlatformTime::Seconds() - CancelBegan < 0.25);
    return true;
}

#endif
