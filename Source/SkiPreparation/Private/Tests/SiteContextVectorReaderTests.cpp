#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/SiteContextVectorReader.h"

#include <string>

namespace
{
using namespace SkiPreparation;

SkiDomain::TerrainCoreManifest MakeVectorReaderTerrainCore()
{
    SkiDomain::TerrainCoreManifest Core;
    Core.ContentId = std::string(64, 'a');
    Core.Width = 16;
    Core.Height = 16;
    Core.DeliveredEastSpacingM = 1.0;
    Core.DeliveredNorthSpacingM = 1.0;
    Core.OuterBounds = {100.0, 200.0, 115.0, 215.0};
    return Core;
}

SiteContextManifest MakeVectorReaderManifest(const bool bSchema2)
{
    SiteContextManifest Manifest;
    Manifest.SchemaVersion = bSchema2 ? SiteContextSchema : SiteContextLegacySchema;
    Manifest.VectorEncoding = bSchema2
        ? SiteContextVectorEncodingV2 : SiteContextVectorEncodingV1;
    return Manifest;
}

TArray<uint8> VectorJsonBytes(const FString& Json)
{
    const FTCHARToUTF8 Encoded(*Json);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
    return Bytes;
}

bool ParseVectorJson(const FString& Json, const bool bSchema2,
    FSiteContextVectorAsset& OutAsset, FString& OutError)
{
    const SkiDomain::TerrainCoreManifest Core = MakeVectorReaderTerrainCore();
    const SiteContextManifest Manifest = MakeVectorReaderManifest(bSchema2);
    return ParseSiteContextVectorAsset(VectorJsonBytes(Json), Manifest, Core,
        OutAsset, OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorReaderLegacyTest,
    "MountainPlanner.M5.SiteContextVectorReader.LegacyV1",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorReaderLegacyTest::RunTest(const FString&)
{
    FSiteContextVectorAsset Asset;
    FString Error;
    const FString Json = TEXT(
        "{\"schemaVersion\":1,\"features\":["
        "{\"osmId\":\"way/7\",\"kind\":\"road\","
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":103.5,\"northM\":204}]}]}" );

    TestTrue(TEXT("legacy v1 asset parses using its legacy encoding"),
        ParseVectorJson(Json, false, Asset, Error));
    TestEqual(TEXT("legacy v1 schema is retained"), Asset.SchemaVersion, 1U);
    TestEqual(TEXT("one legacy polyline is returned"), Asset.Polylines.Num(), 1);
    TestEqual(TEXT("legacy feature identity is preserved"), Asset.Polylines[0].OsmId,
        FString(TEXT("way/7")));
    TestEqual(TEXT("legacy feature kind is preserved"),
        static_cast<uint8>(Asset.Polylines[0].Kind),
        static_cast<uint8>(ESiteContextVectorKind::Road));
    TestEqual(TEXT("legacy v1 receives its implicit single part index zero"),
        Asset.Polylines[0].PartIndex, 0U);
    TestEqual(TEXT("legacy points are counted"), Asset.PointCount, 2ULL);
    TestEqual(TEXT("east coordinate remains in ENU metres"),
        Asset.Polylines[0].Points[1].EastM, 103.5);
    TestEqual(TEXT("north coordinate remains in ENU metres"),
        Asset.Polylines[0].Points[1].NorthM, 204.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorReaderSchema2OrderTest,
    "MountainPlanner.M5.SiteContextVectorReader.Schema2NamespacedOrderAndParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorReaderSchema2OrderTest::RunTest(const FString&)
{
    FSiteContextVectorAsset Asset;
    FString Error;
    const FString Json = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/10\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]},"
        "{\"osmId\":\"way/10\",\"kind\":\"road\",\"partIndex\":1,"
        "\"points\":[{\"eastM\":102,\"northM\":202},{\"eastM\":103,\"northM\":203}]},"
        "{\"osmId\":\"way/2\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":104,\"northM\":204},{\"eastM\":105,\"northM\":205}]},"
        "{\"osmId\":\"relation/8\",\"kind\":\"trail\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":106,\"northM\":206},{\"eastM\":107,\"northM\":207}]}]}" );

    TestTrue(TEXT("valid v2 asset parses"), ParseVectorJson(Json, true, Asset, Error));
    TestEqual(TEXT("schema 2 is retained"), Asset.SchemaVersion, 2U);
    TestEqual(TEXT("all multipart and independent features are retained"),
        Asset.Polylines.Num(), 4);
    TestEqual(TEXT("v2 identity keeps its way namespace"), Asset.Polylines[0].OsmId,
        FString(TEXT("way/10")));
    TestEqual(TEXT("v2 identity is sorted lexically, not numerically"),
        Asset.Polylines[2].OsmId, FString(TEXT("way/2")));
    TestEqual(TEXT("multipart continuation keeps its part index"),
        Asset.Polylines[1].PartIndex, 1U);
    TestEqual(TEXT("relation namespace remains distinct from way namespace"),
        Asset.Polylines[3].OsmId, FString(TEXT("relation/8")));
    TestEqual(TEXT("total points are deterministic"), Asset.PointCount, 8ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorReaderRejectsMalformedTest,
    "MountainPlanner.M5.SiteContextVectorReader.RejectsMalformedAndMisboundAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorReaderRejectsMalformedTest::RunTest(const FString&)
{
    FSiteContextVectorAsset Asset;
    FString Error;
    const FString ValidJson = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}]}" );
    TestTrue(TEXT("valid asset seeds the output"), ParseVectorJson(ValidJson, true, Asset, Error));

    const auto Reject = [&](const TCHAR* Label, const FString& Json,
        const bool bSchema2 = true)
    {
        TestFalse(Label, ParseVectorJson(Json, bSchema2, Asset, Error));
        TestTrue(TEXT("failed parse clears the output transactionally"), Asset.Polylines.IsEmpty());
    };

    Reject(TEXT("malformed JSON is rejected"), TEXT("{bad"));
    Reject(TEXT("JSON schema must match the declared encoding"),
        TEXT("{\"schemaVersion\":1,\"features\":[]}"));
    Reject(TEXT("v1 JSON cannot be paired with the v2 encoding"),
        TEXT("{\"schemaVersion\":1,\"features\":[]}"), true);
    Reject(TEXT("v2 JSON cannot be paired with the v1 encoding"),
        TEXT("{\"schemaVersion\":2,\"features\":[]}"), false);
    Reject(TEXT("unsupported feature kinds are rejected"), TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"stream\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}]}"));
    Reject(TEXT("non-namespaced schema 2 ids are rejected"), TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}]}"));
    Reject(TEXT("out of extent points are rejected"), TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":99.999,\"northM\":200},{\"eastM\":101,\"northM\":201}]}]}"));
    Reject(TEXT("non-finite coordinates are rejected"), TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":1e999,\"northM\":200},{\"eastM\":101,\"northM\":201}]}]}"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorReaderRejectsPartErrorsTest,
    "MountainPlanner.M5.SiteContextVectorReader.RejectsDuplicateGapAndUnsortedParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorReaderRejectsPartErrorsTest::RunTest(const FString&)
{
    FSiteContextVectorAsset Asset;
    FString Error;
    const auto Reject = [&](const TCHAR* Label, const FString& Features)
    {
        const FString Json = TEXT("{\"schemaVersion\":2,\"features\":[")
            + Features + TEXT("]}");
        TestFalse(Label, ParseVectorJson(Json, true, Asset, Error));
    };
    const FString Part0 = TEXT(
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}");
    const FString Part1 = TEXT(
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":1,"
        "\"points\":[{\"eastM\":102,\"northM\":202},{\"eastM\":103,\"northM\":203}]}");
    const FString Part2 = TEXT(
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":2,"
        "\"points\":[{\"eastM\":104,\"northM\":204},{\"eastM\":105,\"northM\":205}]}");
    const FString OtherId = TEXT(
        "{\"osmId\":\"way/2\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":106,\"northM\":206},{\"eastM\":107,\"northM\":207}]}");

    Reject(TEXT("duplicate part index is rejected"), Part0 + TEXT(",") + Part0);
    Reject(TEXT("part-index gap is rejected"), Part0 + TEXT(",") + Part2);
    Reject(TEXT("parts cannot restart after a different identity"),
        Part0 + TEXT(",") + OtherId + TEXT(",") + Part1);
    Reject(TEXT("ids are ordered lexically within a kind"), OtherId + TEXT(",") + Part0);
    Reject(TEXT("kind ordering is lexical"),
        OtherId.Replace(TEXT("\"kind\":\"road\""), TEXT("\"kind\":\"trail\""))
            + TEXT(",") + OtherId);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorReaderRejectsLegacyAmbiguityTest,
    "MountainPlanner.M5.SiteContextVectorReader.RejectsAmbiguousLegacyFeatureParts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorReaderRejectsLegacyAmbiguityTest::RunTest(const FString&)
{
    FSiteContextVectorAsset Asset;
    FString Error;
    const FString Feature = TEXT(
        "{\"osmId\":\"legacy/1\",\"kind\":\"road\","
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}");
    const auto RejectFeatures = [&](const TCHAR* Label, const FString& Features)
    {
        const FString Json = TEXT("{\"schemaVersion\":1,\"features\":[")
            + Features + TEXT("]}");
        TestFalse(Label, ParseVectorJson(Json, false, Asset, Error));
    };
    RejectFeatures(TEXT("legacy duplicate identities are rejected"),
        Feature + TEXT(",") + Feature);
    RejectFeatures(TEXT("legacy assets cannot smuggle an explicit part index"), TEXT(
        "{\"osmId\":\"legacy/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":100,\"northM\":200},{\"eastM\":101,\"northM\":201}]}"));
    return true;
}

#endif
