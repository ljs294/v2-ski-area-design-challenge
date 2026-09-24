#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "SkiDomain/Coordinates.h"
#include "SkiPreparation/OverpassVectorProvider.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainAcquisition.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
using namespace SkiPreparation;

class FScriptedOverpassTransport final : public IAcquisitionTransport
{
public:
    HttpAcquisitionResult Response;
    HttpAcquisitionRequest LastRequest;
    Cancellation* ExternalCancellation = nullptr;
    int32 Calls = 0;
    bool bReturnOversized = false;
    bool bWaitForTransportCancellation = false;

    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& CancellationValue) override
    {
        ++Calls;
        LastRequest = Request;
        if (bReturnOversized)
        {
            HttpAcquisitionResult Oversized;
            Oversized.HttpStatus = 200;
            Oversized.ContentType = TEXT("application/json");
            Oversized.Bytes.SetNum(static_cast<int32>(Request.MaximumResponseBytes + 1));
            Oversized.BytesReceived = static_cast<uint64>(Oversized.Bytes.Num());
            return Oversized;
        }
        if (bWaitForTransportCancellation)
        {
            if (ExternalCancellation) ExternalCancellation->Cancel();
            while (!CancellationValue->IsCancelled()) FPlatformProcess::SleepNoStats(0.001F);
            HttpAcquisitionResult Cancelled;
            Cancelled.FailureReason = TransportFailureReason::Cancelled;
            Cancelled.RequestStatus = TEXT("Cancelled");
            return Cancelled;
        }
        return Response;
    }
};

TArray<uint8> Utf8Bytes(const FString& Value)
{
    const FTCHARToUTF8 Utf8(*Value);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

FString Number(const double Value)
{
    return FString::Printf(TEXT("%.10f"), Value);
}

FString GeometryPoint(const SkiDomain::GeodeticPoint& Point)
{
    return FString::Printf(TEXT("{\"lat\":%s,\"lon\":%s}"),
        *Number(Point.LatitudeDeg), *Number(Point.LongitudeDeg));
}

FString Way(const uint64 Id, const TCHAR* Tags, const TArray<uint64>& NodeIds,
    const TArray<SkiDomain::GeodeticPoint>& Geometry)
{
    FString Nodes = TEXT("[");
    for (int32 Index = 0; Index < NodeIds.Num(); ++Index)
    {
        if (Index > 0) Nodes += TEXT(",");
        Nodes += LexToString(NodeIds[Index]);
    }
    Nodes += TEXT("]");

    FString Points = TEXT("[");
    for (int32 Index = 0; Index < Geometry.Num(); ++Index)
    {
        if (Index > 0) Points += TEXT(",");
        Points += GeometryPoint(Geometry[Index]);
    }
    Points += TEXT("]");
    return FString::Printf(TEXT("{\"type\":\"way\",\"id\":%llu,\"nodes\":%s,\"tags\":{%s},\"geometry\":%s}"),
        Id, *Nodes, Tags, *Points);
}

FString RelationWayMember(const uint64 Ref, const TCHAR* Role,
    const TArray<SkiDomain::GeodeticPoint>& Geometry)
{
    FString Points = TEXT("[");
    for (int32 Index = 0; Index < Geometry.Num(); ++Index)
    {
        if (Index > 0) Points += TEXT(",");
        Points += GeometryPoint(Geometry[Index]);
    }
    Points += TEXT("]");
    return FString::Printf(TEXT("{\"type\":\"way\",\"ref\":%llu,\"role\":\"%s\",\"geometry\":%s}"),
        Ref, Role, *Points);
}

FString Relation(const uint64 Id, const TCHAR* Tags, const FString& Members)
{
    return FString::Printf(TEXT("{\"type\":\"relation\",\"id\":%llu,\"tags\":{%s},\"members\":[%s]}"),
        Id, Tags, *Members);
}

FString ResponseJson(const FString& Elements, const TCHAR* Timestamp = TEXT("2026-09-24T12:00:00Z"))
{
    return FString::Printf(TEXT("{\"version\":0.6,\"generator\":\"Overpass API\",\"osm3s\":{\"timestamp_osm_base\":\"%s\"},\"elements\":[%s]}"),
        Timestamp, *Elements);
}

OsmVectorProviderQuery MakeQuery()
{
    OsmVectorProviderQuery Query;
    Query.TerrainCoreId = std::string(64, 'a');
    Query.LocalOrigin = {47.2, -121.4, 1500.0};
    Query.ExtentM = {0.0, 0.0, 100.0, 100.0};
    Query.RequiredSelectors = {"highway", "aerialway", "piste:type"};
    return Query;
}

SkiDomain::GeodeticPoint PointAtEnu(const SkiDomain::GeodeticPoint& Origin,
    const double EastM, const double NorthM)
{
    SkiDomain::LocalFrame Frame;
    SkiDomain::GeodeticPoint Point;
    if (!SkiDomain::TryMakeLocalFrame(Origin, Frame)
        || !SkiDomain::TrySeaLevelGeodeticFromEnu(Frame, EastM, NorthM, Point))
        return {};
    return Point;
}

HttpAcquisitionResult JsonResponse(const FString& Json)
{
    HttpAcquisitionResult Response;
    Response.HttpStatus = 200;
    Response.ContentType = TEXT("application/json; charset=utf-8");
    Response.Bytes = Utf8Bytes(Json);
    Response.BytesReceived = static_cast<uint64>(Response.Bytes.Num());
    return Response;
}

TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> MakeValidTransport()
{
    const SkiDomain::GeodeticPoint Origin{47.2, -121.4, 1500.0};
    TArray<uint64> Nodes{301, 302};
    TArray<SkiDomain::GeodeticPoint> RoadGeometry{PointAtEnu(Origin, 20.0, 20.0),
        PointAtEnu(Origin, 40.0, 40.0)};
    TArray<SkiDomain::GeodeticPoint> LiftGeometry{PointAtEnu(Origin, 30.0, 20.0),
        PointAtEnu(Origin, 30.0, 60.0)};
    TArray<uint64> TrailNodes{201, 202, 203};
    TArray<SkiDomain::GeodeticPoint> TrailGeometry{PointAtEnu(Origin, 90.0, 80.0),
        PointAtEnu(Origin, 100.0, 80.0), PointAtEnu(Origin, 120.0, 80.0)};
    const FString Elements = Way(30, TEXT("\"highway\":\"track\""), Nodes, RoadGeometry)
        + TEXT(",") + Way(20, TEXT("\"highway\":\"path\",\"piste:type\":\"downhill\""), TrailNodes, TrailGeometry)
        + TEXT(",") + Way(10, TEXT("\"aerialway\":\"chair_lift\""), Nodes, LiftGeometry);
    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> Transport =
        MakeShared<FScriptedOverpassTransport, ESPMode::ThreadSafe>();
    Transport->Response = JsonResponse(ResponseJson(Elements));
    return Transport;
}

bool RunProvider(const TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe>& Transport,
    const FString& Body, FString& OutError, OsmVectorProviderResponse& OutResponse)
{
    if (!Body.IsEmpty()) Transport->Response = JsonResponse(Body);
    OverpassVectorProvider Provider(Transport);
    Cancellation CancellationValue;
    return Provider.Acquire(MakeQuery(), CancellationValue, OutResponse, OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverpassVectorProviderQueryAndNormalizationTest,
    "MountainPlanner.M5.OverpassVectorProvider.BoundedQueryAndDeterministicNormalization",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOverpassVectorProviderQueryAndNormalizationTest::RunTest(const FString&)
{
    const TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> Transport = MakeValidTransport();
    OverpassVectorProvider Provider(Transport);
    Cancellation CancellationValue;
    OsmVectorProviderResponse Result;
    FString Error;
    TestTrue(TEXT("scripted response parses and normalizes"),
        Provider.Acquire(MakeQuery(), CancellationValue, Result, Error));
    TestEqual(TEXT("exactly one logical Overpass query is issued"), Transport->Calls, 1);
    TestEqual(TEXT("provider requests vector context"),
        static_cast<uint8>(Transport->LastRequest.Product),
        static_cast<uint8>(ProviderProduct::VectorContext));
    TestEqual(TEXT("response bytes have a 32 MiB cap"),
        Transport->LastRequest.MaximumResponseBytes, uint64(32ULL * 1024ULL * 1024ULL));
    TestTrue(TEXT("request URL passes the existing gateway allow-list"),
        SkiNetGateway::ValidateUrl(Transport->LastRequest.Url, Error));
    TestTrue(TEXT("query is sent to the approved interpreter endpoint"),
        Transport->LastRequest.Url.StartsWith(TEXT("https://overpass-api.de/api/interpreter?data=")));
    TestTrue(TEXT("query carries a 25 second server timeout and complete geometry output"),
        Transport->LastRequest.Url.Contains(TEXT("%5Bout%3Ajson%5D%5Btimeout%3A25%5D"))
        && Transport->LastRequest.Url.Contains(TEXT("out%20body%20geom%3B"))
        && Transport->LastRequest.Url.Contains(TEXT("relation"))
        && !Transport->LastRequest.Url.Contains(TEXT("geom%28")));
    TestEqual(TEXT("three normalized layers are always explicit"),
        static_cast<int32>(Result.Layers.size()), 3);
    if (Result.Layers.size() != 3 || Result.Layers[0].Features.empty()
        || Result.Layers[1].Features.empty() || Result.Layers[2].Features.empty())
        return false;
    TestEqual(TEXT("road ways are sorted by OSM ID"), Result.Layers[0].Features[0].OsmId, uint64(30));
    TestEqual(TEXT("aerialway IDs are preserved"), Result.Layers[1].Features[0].OsmId, uint64(10));
    TestEqual(TEXT("piste tags take the trail category over generic highway"),
        Result.Layers[2].Features[0].OsmId, uint64(20));
    TestTrue(TEXT("geometry is clipped to the exact TerrainCore east edge"),
        FMath::IsNearlyEqual(Result.Layers[2].Features[0].Points.back().EastM, 100.0, 0.05));
    TestTrue(TEXT("OSM database timestamp is preserved"),
        Result.Source.SourceTimestampUtc == "2026-09-24T12:00:00Z");
    TestTrue(TEXT("retrieval time is recorded as UTC"),
        Result.Source.RetrievedAtUtc.size() == 20
        && Result.Source.RetrievedAtUtc.back() == 'Z');
    TestTrue(TEXT("ODbL attribution is retained"), Result.Source.License == OsmVectorOsmLicense
        && Result.Source.Attribution == OsmVectorAttribution);

    const SkiDomain::GeodeticPoint Origin{47.2, -121.4, 1500.0};
    const TArray<SkiDomain::GeodeticPoint> ReenteringGeometry{
        PointAtEnu(Origin, 10.0, 10.0), PointAtEnu(Origin, 20.0, 10.0),
        PointAtEnu(Origin, 200.0, 10.0), PointAtEnu(Origin, 200.0, 90.0),
        PointAtEnu(Origin, 80.0, 90.0), PointAtEnu(Origin, 20.0, 90.0),
    };
    const TArray<uint64> ReenteringNodes{351, 352, 353, 354, 355, 356};
    const FString ReenteringWay = Way(35, TEXT("\"highway\":\"track\""),
        ReenteringNodes, ReenteringGeometry);
    const FString TrailMembers = RelationWayMember(901, TEXT(""),
            {PointAtEnu(Origin, 30.0, 30.0), PointAtEnu(Origin, 40.0, 30.0)})
        + TEXT(",") + RelationWayMember(801, TEXT("backward"),
            {PointAtEnu(Origin, 40.0, 30.0), PointAtEnu(Origin, 50.0, 30.0)});
    const FString TrailRelation = Relation(700, TEXT("\"type\":\"route\",\"route\":\"piste\",\"piste:type\":\"downhill\""),
        TrailMembers);
    const FString LiftRelation = Relation(701,
        TEXT("\"type\":\"route\",\"route\":\"aerialway\""),
        RelationWayMember(902, TEXT(""),
            {PointAtEnu(Origin, 60.0, 20.0), PointAtEnu(Origin, 60.0, 80.0)}));
    const TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> MultipartTransport =
        MakeValidTransport();
    OsmVectorProviderResponse MultipartResult;
    FString MultipartError;
    TestTrue(TEXT("multipart ways and usable route relations normalize"),
        RunProvider(MultipartTransport,
            ResponseJson(ReenteringWay + TEXT(",") + TrailRelation + TEXT(",") + LiftRelation),
            MultipartError, MultipartResult));
    const auto& RoadParts = MultipartResult.Layers[0].Features;
    const auto& LiftFeatures = MultipartResult.Layers[1].Features;
    const auto& TrailFeatures = MultipartResult.Layers[2].Features;
    const auto RoadBegin = std::find_if(RoadParts.begin(), RoadParts.end(), [](const OsmVectorFeature& Feature)
    {
        return Feature.OsmId == 35;
    });
    TestTrue(TEXT("a way which leaves and re-enters is emitted as two segments"),
        RoadBegin != RoadParts.end() && std::distance(RoadBegin, RoadParts.end()) >= 2);
    if (RoadBegin != RoadParts.end() && std::distance(RoadBegin, RoadParts.end()) >= 2)
    {
        TestEqual(TEXT("the first clipped run keeps part index zero"), RoadBegin->PartIndex, uint32(0));
        TestEqual(TEXT("the second clipped run keeps part index one"), (RoadBegin + 1)->PartIndex, uint32(1));
        TestTrue(TEXT("the two clipped runs stay disconnected"),
            std::abs(RoadBegin->Points.back().NorthM - (RoadBegin + 1)->Points.front().NorthM) > 20.0);
    }
    const auto TrailRelationPart = std::find_if(TrailFeatures.begin(), TrailFeatures.end(),
        [](const OsmVectorFeature& Feature)
        {
            return Feature.ElementKind == OsmElementKind::Relation && Feature.OsmId == 700;
        });
    TestTrue(TEXT("piste relation retains the relation identity and member order"),
        TrailRelationPart != TrailFeatures.end());
    if (TrailRelationPart != TrailFeatures.end())
    {
        TestEqual(TEXT("first relation member part has index zero"), TrailRelationPart->PartIndex, uint32(0));
        const auto SecondTrailPart = std::find_if(TrailRelationPart + 1, TrailFeatures.end(),
            [](const OsmVectorFeature& Feature)
            {
                return Feature.ElementKind == OsmElementKind::Relation && Feature.OsmId == 700;
            });
        TestTrue(TEXT("second relation member remains a separate part"),
            SecondTrailPart != TrailFeatures.end());
        if (SecondTrailPart != TrailFeatures.end())
        {
            TestEqual(TEXT("second relation member part has index one"), SecondTrailPart->PartIndex, uint32(1));
            TestTrue(TEXT("backward relation role reverses member geometry"),
                SecondTrailPart->Points.front().EastM > SecondTrailPart->Points.back().EastM);
        }
    }
    TestTrue(TEXT("aerialway route relation is normalized into the lift layer"),
        std::any_of(LiftFeatures.begin(), LiftFeatures.end(), [](const OsmVectorFeature& Feature)
        {
            return Feature.ElementKind == OsmElementKind::Relation && Feature.OsmId == 701;
        }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverpassVectorProviderRejectsIncompleteAndDuplicateDataTest,
    "MountainPlanner.M5.OverpassVectorProvider.RejectsMalformedDuplicateAndMissingData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOverpassVectorProviderRejectsIncompleteAndDuplicateDataTest::RunTest(const FString&)
{
    auto ExpectFailure = [this](const TCHAR* Label, const FString& Json, const TCHAR* ExpectedError)
    {
        const TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> Transport = MakeValidTransport();
        OsmVectorProviderResponse Result;
        FString Error;
        TestFalse(Label, RunProvider(Transport, Json, Error, Result));
        TestEqual(FString(Label) + TEXT(" has a stable failure code"), Error, FString(ExpectedError));
        TestTrue(FString(Label) + TEXT(" publishes no partial layers"), Result.Layers.empty());
    };

    ExpectFailure(TEXT("invalid JSON"), TEXT("{"), TEXT("OVERPASS_INVALID_JSON"));
    ExpectFailure(TEXT("missing OSM source metadata"), TEXT("{\"elements\":[]}"),
        TEXT("OVERPASS_MISSING_OSM3S_METADATA"));
    ExpectFailure(TEXT("missing elements"), TEXT("{\"osm3s\":{\"timestamp_osm_base\":\"2026-09-24T12:00:00Z\"}}"),
        TEXT("OVERPASS_MISSING_ELEMENTS"));
    ExpectFailure(TEXT("invalid OSM timestamp"), ResponseJson(TEXT(""), TEXT("yesterday")),
        TEXT("OVERPASS_INVALID_SOURCE_TIMESTAMP"));

    const SkiDomain::GeodeticPoint Origin{47.2, -121.4, 1500.0};
    const TArray<uint64> Nodes{701, 702};
    const TArray<SkiDomain::GeodeticPoint> Points{PointAtEnu(Origin, 20.0, 20.0),
        PointAtEnu(Origin, 40.0, 40.0)};
    const FString MissingNodes = TEXT("{\"type\":\"way\",\"id\":70,\"tags\":{\"highway\":\"track\"},\"geometry\":[")
        + GeometryPoint(Points[0]) + TEXT(",") + GeometryPoint(Points[1]) + TEXT("]}");
    ExpectFailure(TEXT("missing node identities"), ResponseJson(MissingNodes), TEXT("OVERPASS_MISSING_WAY_NODES"));
    const FString MissingGeometry = TEXT("{\"type\":\"way\",\"id\":71,\"nodes\":[701,702],\"tags\":{\"highway\":\"track\"}}");
    ExpectFailure(TEXT("missing geometry"), ResponseJson(MissingGeometry), TEXT("OVERPASS_MISSING_WAY_GEOMETRY"));
    const FString DuplicateWay = Way(70, TEXT("\"highway\":\"track\""), Nodes, Points);
    ExpectFailure(TEXT("duplicate way identity"), ResponseJson(DuplicateWay + TEXT(",") + DuplicateWay),
        TEXT("OVERPASS_DUPLICATE_WAY_ID"));
    ExpectFailure(TEXT("unknown relation type fails closed"),
        ResponseJson(Relation(73, TEXT("\"type\":\"site\",\"site\":\"piste\""), TEXT(""))),
        TEXT("OVERPASS_UNSUPPORTED_RELATION_TYPE"));
    ExpectFailure(TEXT("unknown relation member type fails closed"),
        ResponseJson(Relation(74,
            TEXT("\"type\":\"route\",\"route\":\"piste\",\"piste:type\":\"downhill\""),
            TEXT("{\"type\":\"relation\",\"ref\":700,\"role\":\"\"}"))),
        TEXT("OVERPASS_UNSUPPORTED_RELATION_MEMBER_TYPE"));
    ExpectFailure(TEXT("unknown relation member role fails closed"),
        ResponseJson(Relation(75,
            TEXT("\"type\":\"route\",\"route\":\"piste\",\"piste:type\":\"downhill\""),
            RelationWayMember(703, TEXT("shortcut"), Points))),
        TEXT("OVERPASS_UNSUPPORTED_RELATION_MEMBER_ROLE"));
    const FString MalformedGeometry = TEXT("{\"type\":\"way\",\"id\":72,\"nodes\":[701,702],\"tags\":{\"highway\":\"track\"},\"geometry\":[{\"lat\":\"north\",\"lon\":-121.4},{\"lat\":47.2,\"lon\":-121.3}]}");
    ExpectFailure(TEXT("malformed coordinate"), ResponseJson(MalformedGeometry), TEXT("OVERPASS_MALFORMED_WAY_GEOMETRY"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOverpassVectorProviderResponseAndCancellationBoundsTest,
    "MountainPlanner.M5.OverpassVectorProvider.ResponseBoundsAndCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOverpassVectorProviderResponseAndCancellationBoundsTest::RunTest(const FString&)
{
    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> Oversized = MakeValidTransport();
    Oversized->bReturnOversized = true;
    OverpassVectorProvider OversizedProvider(Oversized);
    Cancellation NotCancelled;
    OsmVectorProviderResponse Result;
    FString Error;
    TestFalse(TEXT("oversized body is rejected even if a transport returns it"),
        OversizedProvider.Acquire(MakeQuery(), NotCancelled, Result, Error));
    TestEqual(TEXT("oversized response reports the stable cap failure"), Error,
        FString(TEXT("OVERPASS_RESPONSE_TOO_LARGE")));

    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> Blocking = MakeValidTransport();
    Blocking->bWaitForTransportCancellation = true;
    Cancellation ExternalCancellation;
    Blocking->ExternalCancellation = &ExternalCancellation;
    OverpassVectorProvider BlockingProvider(Blocking);
    const double Began = FPlatformTime::Seconds();
    TestFalse(TEXT("external cancellation is relayed into the active transport"),
        BlockingProvider.Acquire(MakeQuery(), ExternalCancellation, Result, Error));
    TestEqual(TEXT("transport cancellation suppresses all output"), Error,
        FString(TEXT("OVERPASS_CANCELLED")));
    TestTrue(TEXT("cancellation is acknowledged promptly"), FPlatformTime::Seconds() - Began < 1.0);
    TestTrue(TEXT("cancelled response publishes no partial layers"), Result.Layers.empty());

    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> RelationBudget = MakeValidTransport();
    FString ManyMembers;
    ManyMembers.Reserve(2'500'000);
    for (uint64 MemberId = 1; MemberId <= 50'001; ++MemberId)
    {
        if (MemberId > 1) ManyMembers += TEXT(",");
        ManyMembers += FString::Printf(TEXT("{\"type\":\"way\",\"ref\":%llu,\"role\":\"\"}"),
            MemberId);
    }
    OsmVectorProviderResponse RelationBudgetResult;
    FString RelationBudgetError;
    TestFalse(TEXT("relation members are bounded before geometry normalization"),
        RunProvider(RelationBudget,
            ResponseJson(Relation(76,
                TEXT("\"type\":\"route\",\"route\":\"piste\",\"piste:type\":\"downhill\""),
                ManyMembers)), RelationBudgetError, RelationBudgetResult));
    TestEqual(TEXT("relation member overflow has a stable budget failure"), RelationBudgetError,
        FString(TEXT("OVERPASS_RELATION_MEMBER_LIMIT")));
    TestTrue(TEXT("relation budget failure publishes no partial layers"),
        RelationBudgetResult.Layers.empty());

    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> NeverCalled = MakeValidTransport();
    OverpassVectorProvider InvalidBoundsProvider(NeverCalled);
    OsmVectorProviderQuery Invalid = MakeQuery();
    Invalid.ExtentM.EastM = 20'000.0;
    Cancellation InvalidQueryCancellation;
    TestFalse(TEXT("over-limit selection is rejected before transport"),
        InvalidBoundsProvider.Acquire(Invalid, InvalidQueryCancellation, Result, Error));
    TestEqual(TEXT("site extent remains capped at 10 km"), Error,
        FString(TEXT("OVERPASS_INVALID_QUERY_BOUNDS")));
    TestEqual(TEXT("invalid geometry never makes a request"), NeverCalled->Calls, 0);

    TSharedPtr<FScriptedOverpassTransport, ESPMode::ThreadSafe> PreCancelled = MakeValidTransport();
    OverpassVectorProvider PreCancelledProvider(PreCancelled);
    Cancellation AlreadyCancelled;
    AlreadyCancelled.Cancel();
    TestFalse(TEXT("pre-cancelled vector acquisition is rejected before transport"),
        PreCancelledProvider.Acquire(MakeQuery(), AlreadyCancelled, Result, Error));
    TestEqual(TEXT("pre-cancelled acquisition issues no Overpass request"), PreCancelled->Calls, 0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
