#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "SkiPreparation/OsmVectorPackage.h"
#include "SkiPreparation/OsmVectorSiteContextConverter.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <algorithm>
#include <string>

namespace
{
SkiDomain::TerrainCoreManifest MakeOsmVectorTerrainCore()
{
    SkiDomain::TerrainCoreManifest Core;
    Core.ContentId = std::string(64, 'a');
    Core.Width = 4;
    Core.Height = 4;
    Core.DeliveredEastSpacingM = 1.0;
    Core.DeliveredNorthSpacingM = 1.0;
    Core.LocalOrigin = {47.2, -121.4, 1500.0};
    Core.SampleCenterBounds = {0.0, 0.0, 3.0, 3.0};
    SkiDomain::ComputeTerrainCoreBounds(Core.Width, Core.Height,
        Core.DeliveredEastSpacingM, Core.DeliveredNorthSpacingM,
        Core.SampleCenterBounds, Core.OuterBounds);
    return Core;
}

SkiPreparation::OsmVectorPackage MakeOsmVectorPackage(
    const SkiDomain::TerrainCoreManifest& Core)
{
    using namespace SkiPreparation;
    OsmVectorPackage Package;
    Package.TerrainCoreId = Core.ContentId;
    Package.LocalOrigin = Core.LocalOrigin;
    Package.TerrainCoreWidth = Core.Width;
    Package.TerrainCoreHeight = Core.Height;
    Package.TerrainCoreEastSpacingM = Core.DeliveredEastSpacingM;
    Package.TerrainCoreNorthSpacingM = Core.DeliveredNorthSpacingM;
    Package.ExtentM = Core.OuterBounds;
    Package.Source.SourceTimestampUtc = "2026-09-24T12:00:00Z";
    Package.Source.RetrievedAtUtc = "2026-09-24T12:01:00.125Z";
    for (std::uint8_t Index = 0; Index < 3; ++Index)
    {
        OsmVectorLayer Layer;
        Layer.Kind = static_cast<OsmVectorLayerKind>(Index);
        OsmVectorFeature Feature;
        Feature.ElementKind = OsmElementKind::Way;
        Feature.OsmId = 100U + Index;
        Feature.Points = {
            {Core.OuterBounds.WestM, Core.OuterBounds.SouthM},
            {Core.OuterBounds.EastM, Core.OuterBounds.NorthM},
        };
        Layer.Features.push_back(std::move(Feature));
        Package.Layers.push_back(std::move(Layer));
    }
    Package.ContentId = TCHAR_TO_UTF8(*ComputeOsmVectorPackageContentId(Package));
    return Package;
}

class FScriptedOsmVectorProvider final : public SkiPreparation::IOsmVectorProvider
{
public:
    explicit FScriptedOsmVectorProvider(SkiPreparation::OsmVectorProviderResponse InResponse)
        : Response(std::move(InResponse)) {}

    bool Acquire(const SkiPreparation::OsmVectorProviderQuery& Query,
        const SkiPreparation::Cancellation& CancellationValue,
        SkiPreparation::OsmVectorProviderResponse& OutResponse, FString& OutError) override
    {
        ++Calls;
        LastQuery = Query;
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("cancelled");
            return false;
        }
        if (CancelAfterResponse != nullptr) CancelAfterResponse->Cancel();
        OutResponse = Response;
        return bReturnResponse;
    }

    SkiPreparation::OsmVectorProviderResponse Response;
    SkiPreparation::OsmVectorProviderQuery LastQuery;
    SkiPreparation::Cancellation* CancelAfterResponse = nullptr;
    int32 Calls = 0;
    bool bReturnResponse = true;
};

SkiPreparation::OsmVectorProviderResponse MakeProviderResponse(
    const SkiPreparation::OsmVectorPackage& Package)
{
    SkiPreparation::OsmVectorProviderResponse Response;
    Response.Source = Package.Source;
    Response.Layers = Package.Layers;
    return Response;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmVectorPackageContractTest,
    "MountainPlanner.M5.OsmVectorPackage.RequiredLayersAndLegacyIndependentParsing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmVectorPackageContractTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest Core = MakeOsmVectorTerrainCore();
    SkiPreparation::OsmVectorPackage Package = MakeOsmVectorPackage(Core);
    TestEqual(TEXT("new vector packages use multipart schema 2"), Package.SchemaVersion,
        SkiPreparation::OsmVectorPackageSchema);
    TestTrue(TEXT("fixture validates against its TerrainCore"),
        SkiPreparation::ValidateOsmVectorPackage(Core, Package).Ok());
    TestTrue(TEXT("normalized road layer uses highway"),
        std::string(SkiPreparation::OsmVectorLayerSelector(SkiPreparation::OsmVectorLayerKind::Road))
            == "highway");
    TestTrue(TEXT("normalized lift layer uses aerialway"),
        std::string(SkiPreparation::OsmVectorLayerSelector(SkiPreparation::OsmVectorLayerKind::Lift))
            == "aerialway");
    TestTrue(TEXT("normalized trail layer uses piste:type"),
        std::string(SkiPreparation::OsmVectorLayerSelector(SkiPreparation::OsmVectorLayerKind::Trail))
            == "piste:type");

    SkiPreparation::OsmVectorPackage MissingLayer = Package;
    MissingLayer.Layers.pop_back();
    TestEqual(TEXT("all three required normalized layers must be declared"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, MissingLayer).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::MissingLayer));

    const FString Canonical = SkiPreparation::SerializeOsmVectorPackage(Package, true);
    SkiPreparation::OsmVectorPackage Parsed;
    FString Error;
    TestTrue(TEXT("standalone OSM schema parses without a legacy SiteContext manifest"),
        SkiPreparation::ParseOsmVectorPackage(Canonical, Core, Parsed, Error));
    TestEqual(TEXT("parse preserves canonical package bytes"),
        SkiPreparation::SerializeOsmVectorPackage(Parsed, true), Canonical);

    SkiPreparation::OsmVectorPackage Legacy = Package;
    Legacy.SchemaVersion = SkiPreparation::OsmVectorPackageLegacySchema;
    Legacy.ContentId = TCHAR_TO_UTF8(*SkiPreparation::ComputeOsmVectorPackageContentId(Legacy));
    const FString LegacyCanonical = SkiPreparation::SerializeOsmVectorPackage(Legacy, true);
    TestFalse(TEXT("schema 1 keeps its original single-points representation"),
        LegacyCanonical.Contains(TEXT("partIndex")));
    TestTrue(TEXT("schema 1 package remains readable"),
        SkiPreparation::ParseOsmVectorPackage(LegacyCanonical, Core, Parsed, Error));
    TestEqual(TEXT("schema 1 read retains its version"), Parsed.SchemaVersion,
        SkiPreparation::OsmVectorPackageLegacySchema);
    TestEqual(TEXT("schema 1 read maps its single geometry to part zero"),
        Parsed.Layers[0].Features[0].PartIndex, uint32(0));
    TestEqual(TEXT("schema 1 round-trip retains the legacy canonical bytes"),
        SkiPreparation::SerializeOsmVectorPackage(Parsed, true), LegacyCanonical);

    TestFalse(TEXT("legacy SiteContext JSON is not interpreted as an OSM vector package"),
        SkiPreparation::ParseOsmVectorPackage(
            TEXT("{\"schemaVersion\":1,\"contentId\":\"legacy-site-context\"}"),
            Core, Parsed, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmVectorPackageGeometryAndProvenanceTest,
    "MountainPlanner.M5.OsmVectorPackage.GeometryIdsBoundsAndAttribution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmVectorPackageGeometryAndProvenanceTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest Core = MakeOsmVectorTerrainCore();
    const SkiPreparation::OsmVectorPackage Fixture = MakeOsmVectorPackage(Core);

    SkiPreparation::OsmVectorPackage Malformed = Fixture;
    Malformed.Layers[0].Features[0].Points.pop_back();
    TestEqual(TEXT("a one-point line is rejected"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, Malformed).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::MalformedGeometry));

    SkiPreparation::OsmVectorPackage DuplicateId = Fixture;
    DuplicateId.Layers[2].Features[0].OsmId = DuplicateId.Layers[0].Features[0].OsmId;
    TestEqual(TEXT("duplicate way/relation identity is rejected across layers"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, DuplicateId).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::DuplicateFeatureId));

    SkiPreparation::OsmVectorPackage Outside = Fixture;
    Outside.Layers[0].Features[0].Points[0].EastM = Outside.ExtentM.EastM + 0.01;
    TestEqual(TEXT("features cannot extend beyond SiteContext-aligned TerrainCore bounds"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, Outside).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::OutOfBounds));

    SkiPreparation::OsmVectorPackage BadLicense = Fixture;
    BadLicense.Source.License = "CC-BY-4.0";
    TestEqual(TEXT("non-ODbL license is rejected"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, BadLicense).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::InvalidLicense));

    SkiPreparation::OsmVectorPackage BadAttribution = Fixture;
    BadAttribution.Source.Attribution = "OpenStreetMap";
    TestEqual(TEXT("missing required OSM attribution is rejected"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, BadAttribution).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::InvalidAttribution));

    SkiPreparation::OsmVectorPackage BadTimestamp = Fixture;
    BadTimestamp.Source.SourceTimestampUtc = "yesterday";
    TestEqual(TEXT("unverifiable source timestamp is rejected"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, BadTimestamp).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::InvalidSourceTimestamp));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmVectorPackageHashAndStorageTest,
    "MountainPlanner.M5.OsmVectorPackage.HashStorageAndDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmVectorPackageHashAndStorageTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest Core = MakeOsmVectorTerrainCore();
    SkiPreparation::OsmVectorPackage Package = MakeOsmVectorPackage(Core);
    SkiPreparation::OsmVectorPackage Parsed;
    FString Error;
    SkiPreparation::OsmVectorStorageEstimate Estimate;
    TestTrue(TEXT("bounded feature/point request returns an estimate"),
        SkiPreparation::EstimateOsmVectorStorage(3, 6, Estimate));
    TestFalse(TEXT("pre-acquisition bytes remain estimated"), Estimate.IsExact());

    SkiPreparation::OsmVectorPackageVerificationReport Report;
    SkiPreparation::Cancellation Cancellation;
    TestTrue(TEXT("valid package verifies"), SkiPreparation::VerifyOsmVectorPackage(
        Core, Package, Cancellation, Report));
    TestTrue(TEXT("verified canonical asset size is exact"), Report.Storage.IsExact());
    TestEqual(TEXT("exact package bytes match canonical UTF-8 serialization"),
        Report.Storage.MinimumBytes,
        static_cast<uint64>(FTCHARToUTF8(*SkiPreparation::SerializeOsmVectorPackage(Package, true)).Length()));

    SkiPreparation::OsmVectorPackage HashMismatch = Package;
    HashMismatch.Layers[0].Features[0].Points[0].EastM += 0.25;
    TestEqual(TEXT("changing valid geometry without a new content ID is rejected"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, HashMismatch).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::ContentHashMismatch));

    SkiPreparation::OsmVectorPackage Reordered = Package;
    std::reverse(Reordered.Layers.begin(), Reordered.Layers.end());
    TestEqual(TEXT("canonical serialization is independent of input layer order"),
        SkiPreparation::SerializeOsmVectorPackage(Reordered, false),
        SkiPreparation::SerializeOsmVectorPackage(Package, false));

    SkiPreparation::OsmVectorPackage Multipart = Package;
    SkiPreparation::OsmVectorFeature SecondPart = Multipart.Layers[0].Features[0];
    SecondPart.PartIndex = 1;
    SecondPart.Points = {
        {Core.OuterBounds.WestM, Core.OuterBounds.NorthM},
        {Core.OuterBounds.EastM, Core.OuterBounds.SouthM},
    };
    Multipart.Layers[0].Features.push_back(SecondPart);
    Multipart.ContentId = TCHAR_TO_UTF8(*SkiPreparation::ComputeOsmVectorPackageContentId(Multipart));
    TestTrue(TEXT("multiple ordered parts may retain the same OSM element identity"),
        SkiPreparation::ValidateOsmVectorPackage(Core, Multipart).Ok());
    const FString MultipartCanonical = SkiPreparation::SerializeOsmVectorPackage(Multipart, true);
    TestTrue(TEXT("schema 2 serializes the part order"),
        MultipartCanonical.Contains(TEXT("\"partIndex\"")));
    const FString MissingPartIndex = MultipartCanonical.Replace(
        TEXT("\"partIndex\""), TEXT("\"unexpectedPartIndex\""));
    TestFalse(TEXT("schema 2 requires an explicit part index"),
        SkiPreparation::ParseOsmVectorPackage(MissingPartIndex, Core, Parsed, Error));
    TestTrue(TEXT("multipart schema parses and preserves its stable OSM ID"),
        SkiPreparation::ParseOsmVectorPackage(MultipartCanonical, Core, Parsed, Error));
    TestEqual(TEXT("multipart parts round-trip in part-index order"),
        Parsed.Layers[0].Features[1].PartIndex, uint32(1));
    TestEqual(TEXT("both parts retain the same OSM identity"),
        Parsed.Layers[0].Features[0].OsmId, Parsed.Layers[0].Features[1].OsmId);
    std::reverse(Multipart.Layers[0].Features.begin(), Multipart.Layers[0].Features.end());
    TestEqual(TEXT("canonical feature serialization sorts by OSM identity then part index"),
        SkiPreparation::SerializeOsmVectorPackage(Multipart, false),
        SkiPreparation::SerializeOsmVectorPackage(Parsed, false));

    SkiPreparation::OsmVectorPackage MissingPart = Package;
    SkiPreparation::OsmVectorFeature ThirdPart = MissingPart.Layers[0].Features[0];
    ThirdPart.PartIndex = 2;
    MissingPart.Layers[0].Features.push_back(ThirdPart);
    TestEqual(TEXT("part indices must be contiguous from zero"),
        static_cast<uint8>(SkiPreparation::ValidateOsmVectorPackage(Core, MissingPart).Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::InvalidPartIndex));

    SkiPreparation::OsmVectorPackage ConversionInput = Package;
    SkiPreparation::OsmVectorFeature RoadPart1 = ConversionInput.Layers[0].Features[0];
    RoadPart1.PartIndex = 1;
    RoadPart1.Points = {
        {Core.OuterBounds.WestM, Core.OuterBounds.NorthM},
        {Core.OuterBounds.EastM, Core.OuterBounds.SouthM},
    };
    ConversionInput.Layers[0].Features.push_back(RoadPart1);

    SkiPreparation::OsmVectorFeature LiftRelationPart0 = ConversionInput.Layers[1].Features[0];
    LiftRelationPart0.ElementKind = SkiPreparation::OsmElementKind::Relation;
    LiftRelationPart0.OsmId = 20;
    LiftRelationPart0.Points = {
        {Core.OuterBounds.WestM, Core.OuterBounds.SouthM},
        {Core.OuterBounds.EastM, Core.OuterBounds.NorthM},
    };
    ConversionInput.Layers[1].Features.push_back(LiftRelationPart0);
    SkiPreparation::OsmVectorFeature LiftRelationPart1 = LiftRelationPart0;
    LiftRelationPart1.PartIndex = 1;
    LiftRelationPart1.Points = {
        {Core.OuterBounds.EastM, Core.OuterBounds.SouthM},
        {Core.OuterBounds.WestM, Core.OuterBounds.NorthM},
    };
    ConversionInput.Layers[1].Features.push_back(LiftRelationPart1);
    ConversionInput.ContentId = TCHAR_TO_UTF8(
        *SkiPreparation::ComputeOsmVectorPackageContentId(ConversionInput));

    SkiPreparation::OsmVectorSiteContextAsset ConvertedAsset;
    FString ConversionError;
    TestTrue(TEXT("verified multipart relation package converts to SiteContext schema 2"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, ConversionInput, Cancellation, ConvertedAsset, ConversionError));
    TestTrue(TEXT("conversion returns bounded nonempty UTF-8 JSON"),
        !ConvertedAsset.VectorJsonUtf8.IsEmpty()
        && static_cast<std::uint64_t>(ConvertedAsset.VectorJsonUtf8.Num())
            <= SkiPreparation::SiteContextMaxAssetBytes);
    TestTrue(TEXT("conversion preserves package hash, TerrainCore binding and full provenance"),
        ConvertedAsset.SourcePackageContentId == ConversionInput.ContentId
        && ConvertedAsset.TerrainCoreId == ConversionInput.TerrainCoreId
        && ConvertedAsset.Source.Provider == ConversionInput.Source.Provider
        && ConvertedAsset.Source.Endpoint == ConversionInput.Source.Endpoint
        && ConvertedAsset.Source.SourceTimestampUtc == ConversionInput.Source.SourceTimestampUtc
        && ConvertedAsset.Source.RetrievedAtUtc == ConversionInput.Source.RetrievedAtUtc
        && ConvertedAsset.Source.License == ConversionInput.Source.License
        && ConvertedAsset.Source.Attribution == ConversionInput.Source.Attribution
        && ConvertedAsset.Source.AttributionUrl == ConversionInput.Source.AttributionUrl);
    TestTrue(TEXT("conversion preserves the verified TerrainCore extent exactly"),
        ConvertedAsset.ExtentM.WestM == ConversionInput.ExtentM.WestM
        && ConvertedAsset.ExtentM.SouthM == ConversionInput.ExtentM.SouthM
        && ConvertedAsset.ExtentM.EastM == ConversionInput.ExtentM.EastM
        && ConvertedAsset.ExtentM.NorthM == ConversionInput.ExtentM.NorthM);

    const std::string ConvertedUtf8(
        reinterpret_cast<const char*>(ConvertedAsset.VectorJsonUtf8.GetData()),
        static_cast<std::size_t>(ConvertedAsset.VectorJsonUtf8.Num()));
    const FString ConvertedJson = UTF8_TO_TCHAR(ConvertedUtf8.c_str());
    TSharedPtr<FJsonObject> ConvertedRoot;
    const TSharedRef<TJsonReader<>> ConvertedReader = TJsonReaderFactory<>::Create(ConvertedJson);
    TestTrue(TEXT("converted vector asset is valid JSON"),
        FJsonSerializer::Deserialize(ConvertedReader, ConvertedRoot) && ConvertedRoot.IsValid());
    if (!ConvertedRoot.IsValid()) return false;
    double ConvertedSchema = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* ConvertedFeatures = nullptr;
    TestTrue(TEXT("converted vector asset uses SiteContext schema 2"),
        ConvertedRoot->TryGetNumberField(TEXT("schemaVersion"), ConvertedSchema)
        && ConvertedSchema == 2.0
        && ConvertedRoot->TryGetArrayField(TEXT("features"), ConvertedFeatures)
        && ConvertedFeatures != nullptr);
    if (ConvertedFeatures == nullptr) return false;
    const char* ExpectedConvertedIds[] = {
        "relation/20", "relation/20", "way/101", "way/100", "way/100", "way/102"};
    const char* ExpectedConvertedKinds[] = {"lift", "lift", "lift", "road", "road", "trail"};
    const double ExpectedConvertedParts[] = {0.0, 1.0, 0.0, 0.0, 1.0, 0.0};
    TestEqual(TEXT("conversion preserves every input part without flattening topology"),
        ConvertedFeatures->Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedConvertedIds)));
    for (int32 FeatureIndex = 0; FeatureIndex < ConvertedFeatures->Num()
        && FeatureIndex < UE_ARRAY_COUNT(ExpectedConvertedIds); ++FeatureIndex)
    {
        const TSharedPtr<FJsonObject> FeatureObject = (*ConvertedFeatures)[FeatureIndex]
            ? (*ConvertedFeatures)[FeatureIndex]->AsObject() : nullptr;
        FString OsmId;
        FString Kind;
        double PartIndex = -1.0;
        const bool HasFields = FeatureObject
            && FeatureObject->TryGetStringField(TEXT("osmId"), OsmId)
            && FeatureObject->TryGetStringField(TEXT("kind"), Kind)
            && FeatureObject->TryGetNumberField(TEXT("partIndex"), PartIndex);
        const FTCHARToUTF8 ActualId(*OsmId);
        const FTCHARToUTF8 ActualKind(*Kind);
        TestTrue(FString::Printf(TEXT("converted feature %d has tuple-sorted identity"), FeatureIndex),
            HasFields
            && std::string(ActualId.Get(), ActualId.Length()) == ExpectedConvertedIds[FeatureIndex]
            && std::string(ActualKind.Get(), ActualKind.Length()) == ExpectedConvertedKinds[FeatureIndex]
            && PartIndex == ExpectedConvertedParts[FeatureIndex]);
        const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
        TestTrue(FString::Printf(TEXT("converted feature %d retains its own part geometry"), FeatureIndex),
            FeatureObject && FeatureObject->TryGetArrayField(TEXT("points"), Points)
            && Points != nullptr && Points->Num() == 2);
    }

    SkiPreparation::OsmVectorPackage ShuffledConversionInput = ConversionInput;
    std::reverse(ShuffledConversionInput.Layers.begin(), ShuffledConversionInput.Layers.end());
    for (SkiPreparation::OsmVectorLayer& Layer : ShuffledConversionInput.Layers)
        std::reverse(Layer.Features.begin(), Layer.Features.end());
    ShuffledConversionInput.ContentId = TCHAR_TO_UTF8(
        *SkiPreparation::ComputeOsmVectorPackageContentId(ShuffledConversionInput));
    SkiPreparation::OsmVectorSiteContextAsset ShuffledConvertedAsset;
    TestTrue(TEXT("shuffled verified package converts"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, ShuffledConversionInput, Cancellation, ShuffledConvertedAsset, ConversionError));
    TestTrue(TEXT("conversion JSON is deterministic across source layer and feature order"),
        ConvertedAsset.VectorJsonUtf8 == ShuffledConvertedAsset.VectorJsonUtf8);

    SkiPreparation::OsmVectorPackage LegacyConversionInput = Package;
    LegacyConversionInput.SchemaVersion = SkiPreparation::OsmVectorPackageLegacySchema;
    LegacyConversionInput.ContentId = TCHAR_TO_UTF8(
        *SkiPreparation::ComputeOsmVectorPackageContentId(LegacyConversionInput));
    SkiPreparation::OsmVectorSiteContextAsset LegacyConvertedAsset;
    TestTrue(TEXT("verified schema-1 package converts to SiteContext schema 2"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, LegacyConversionInput, Cancellation, LegacyConvertedAsset, ConversionError));
    const std::string LegacyUtf8(
        reinterpret_cast<const char*>(LegacyConvertedAsset.VectorJsonUtf8.GetData()),
        static_cast<std::size_t>(LegacyConvertedAsset.VectorJsonUtf8.Num()));
    const FString LegacyConvertedJson = UTF8_TO_TCHAR(LegacyUtf8.c_str());
    TestTrue(TEXT("schema-1 source emits an explicit schema-2 part index"),
        LegacyConvertedJson.Contains(TEXT("\"schemaVersion\":2"))
        && LegacyConvertedJson.Contains(TEXT("\"partIndex\":0")));

    SkiPreparation::OsmVectorPackage TamperedConversionInput = Package;
    TamperedConversionInput.ContentId[0] = TamperedConversionInput.ContentId[0] == '0' ? '1' : '0';
    SkiPreparation::OsmVectorSiteContextAsset RejectedAsset = ConvertedAsset;
    TestFalse(TEXT("conversion rejects a package with a mismatched content hash"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, TamperedConversionInput, Cancellation, RejectedAsset, ConversionError));
    TestTrue(TEXT("failed conversion clears its output"), RejectedAsset.VectorJsonUtf8.IsEmpty());

    SkiPreparation::Cancellation CancelledConversion;
    CancelledConversion.Cancel();
    SkiPreparation::OsmVectorSiteContextAsset CancelledAsset = ConvertedAsset;
    TestFalse(TEXT("conversion observes cancellation before producing an asset"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, ConversionInput, CancelledConversion, CancelledAsset, ConversionError));
    TestTrue(TEXT("cancelled conversion clears its output"), CancelledAsset.VectorJsonUtf8.IsEmpty());

    SkiPreparation::OsmVectorPackage TooManyFeatures = Package;
    const std::size_t ExtraFeatureCount = static_cast<std::size_t>(
        SkiPreparation::OsmVectorPackageMaxFeatures - 2ULL);
    TooManyFeatures.Layers[0].Features.reserve(1U + ExtraFeatureCount);
    for (std::size_t Index = 0; Index < ExtraFeatureCount; ++Index)
    {
        SkiPreparation::OsmVectorFeature ExtraFeature;
        ExtraFeature.OsmId = 10'000ULL + static_cast<std::uint64_t>(Index);
        ExtraFeature.Points = {
            {Core.OuterBounds.WestM, Core.OuterBounds.SouthM},
            {Core.OuterBounds.EastM, Core.OuterBounds.NorthM},
        };
        TooManyFeatures.Layers[0].Features.push_back(std::move(ExtraFeature));
    }
    SkiPreparation::OsmVectorSiteContextAsset BudgetRejectedAsset = ConvertedAsset;
    TestFalse(TEXT("conversion rejects packages beyond its verified feature budget"),
        SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
            Core, TooManyFeatures, Cancellation, BudgetRejectedAsset, ConversionError));
    TestTrue(TEXT("budget rejection clears its output"), BudgetRejectedAsset.VectorJsonUtf8.IsEmpty());

    Cancellation.Cancel();
    TestFalse(TEXT("cancelled verification reports cancellation"),
        SkiPreparation::VerifyOsmVectorPackage(Core, Package, Cancellation, Report));
    TestEqual(TEXT("cancelled report has no exact storage claim"),
        static_cast<uint8>(Report.Error),
        static_cast<uint8>(SkiPreparation::OsmVectorPackageError::Cancelled));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOsmVectorProviderSeamTest,
    "MountainPlanner.M5.OsmVectorPackage.ProviderGatewaySeamAndCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOsmVectorProviderSeamTest::RunTest(const FString& Parameters)
{
    const SkiDomain::TerrainCoreManifest Core = MakeOsmVectorTerrainCore();
    const SkiPreparation::OsmVectorPackage Fixture = MakeOsmVectorPackage(Core);
    FScriptedOsmVectorProvider Provider(MakeProviderResponse(Fixture));
    SkiPreparation::Cancellation Cancellation;
    SkiPreparation::OsmVectorPackage Package;
    FString Error;
    TestTrue(TEXT("provider response is bound, validated and hashed"),
        SkiPreparation::AcquireOsmVectorPackage(Provider, Core, Cancellation, Package, Error));
    TestEqual(TEXT("provider is invoked once for one logical query"), Provider.Calls, 1);
    TestEqual(TEXT("query carries the TerrainCore local ENU extent"), Provider.LastQuery.ExtentM.EastM,
        Core.OuterBounds.EastM);
    TestEqual(TEXT("query requests only the three normalized selectors"),
        static_cast<int32>(Provider.LastQuery.RequiredSelectors.size()), 3);
    TestTrue(TEXT("query selector order is stable"),
        Provider.LastQuery.RequiredSelectors[0] == "highway");
    TestEqual(TEXT("published response has a content identity"),
        static_cast<int32>(Package.ContentId.size()), 64);

    FScriptedOsmVectorProvider CancellingProvider(MakeProviderResponse(Fixture));
    SkiPreparation::Cancellation CancelDuringAcquire;
    CancellingProvider.CancelAfterResponse = &CancelDuringAcquire;
    TestFalse(TEXT("a cancellation arriving during acquisition prevents publication"),
        SkiPreparation::AcquireOsmVectorPackage(CancellingProvider, Core,
            CancelDuringAcquire, Package, Error));
    TestTrue(TEXT("cancelled acquisition leaves no package identity"), Package.ContentId.empty());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
