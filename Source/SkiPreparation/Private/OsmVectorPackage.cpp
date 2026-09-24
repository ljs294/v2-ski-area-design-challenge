#include "SkiPreparation/OsmVectorPackage.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace
{
using namespace SkiPreparation;

constexpr std::array<OsmVectorLayerKind, 3> RequiredLayerOrder{
    OsmVectorLayerKind::Road, OsmVectorLayerKind::Lift, OsmVectorLayerKind::Trail};
constexpr const char* ExpectedProvider = "openstreetmap-overpass";
constexpr const char* ExpectedEndpoint = "https://overpass-api.de/api/interpreter";
constexpr double MaximumSiteSideM = 10'000.0;
constexpr double MaximumLocalCoordinateM = 100'000.0;

double CanonicalDouble(const double Value) noexcept
{
    return Value == 0.0 ? 0.0 : Value;
}

std::string Utf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), Converted.Length());
}

bool ToUtf8Bytes(const FString& Value, TArray<uint8>& OutBytes)
{
    OutBytes.Reset();
    const FTCHARToUTF8 Converted(*Value);
    if (Converted.Length() < 0
        || static_cast<std::uint64_t>(Converted.Length()) > OsmVectorPackageMaxBytes)
    {
        return false;
    }
    OutBytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return true;
}

bool NearlyEqual(const double A, const double B) noexcept
{
    return std::isfinite(A) && std::isfinite(B)
        && std::abs(A - B) <= std::max({1.0, std::abs(A), std::abs(B)}) * 1.0e-9;
}

bool SamePoint(const SkiDomain::GeodeticPoint& A,
    const SkiDomain::GeodeticPoint& B) noexcept
{
    return NearlyEqual(A.LatitudeDeg, B.LatitudeDeg)
        && NearlyEqual(A.LongitudeDeg, B.LongitudeDeg)
        && NearlyEqual(A.HeightM, B.HeightM);
}

bool SameBounds(const SkiDomain::MetricBounds& A,
    const SkiDomain::MetricBounds& B) noexcept
{
    return NearlyEqual(A.WestM, B.WestM) && NearlyEqual(A.SouthM, B.SouthM)
        && NearlyEqual(A.EastM, B.EastM) && NearlyEqual(A.NorthM, B.NorthM);
}

bool FiniteBounds(const SkiDomain::MetricBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM < Bounds.EastM && Bounds.SouthM < Bounds.NorthM;
}

bool BoundedSiteExtent(const SkiDomain::MetricBounds& Bounds) noexcept
{
    return FiniteBounds(Bounds)
        && Bounds.EastM - Bounds.WestM <= MaximumSiteSideM
        && Bounds.NorthM - Bounds.SouthM <= MaximumSiteSideM
        && std::abs(Bounds.WestM) <= MaximumLocalCoordinateM
        && std::abs(Bounds.EastM) <= MaximumLocalCoordinateM
        && std::abs(Bounds.SouthM) <= MaximumLocalCoordinateM
        && std::abs(Bounds.NorthM) <= MaximumLocalCoordinateM;
}

bool IsSafeText(const std::string& Value, const std::size_t Maximum) noexcept
{
    return !Value.empty() && Value.size() <= Maximum
        && std::none_of(Value.begin(), Value.end(), [](const unsigned char Character)
        {
            return Character < 0x20U || Character == 0x7fU;
        });
}

bool IsLeapYear(const unsigned Year) noexcept
{
    return (Year % 4U == 0U && Year % 100U != 0U) || Year % 400U == 0U;
}

bool IsUtcTimestamp(const std::string& Value) noexcept
{
    // YYYY-MM-DDTHH:MM:SS[.fraction]Z, with a real calendar date and UTC only.
    if (Value.size() < 20 || Value.size() > 30
        || Value[4] != '-' || Value[7] != '-' || Value[10] != 'T'
        || Value[13] != ':' || Value[16] != ':' || Value.back() != 'Z')
        return false;
    const auto Digits = [&Value](const std::size_t Begin, const std::size_t Count)
    {
        for (std::size_t Index = Begin; Index < Begin + Count; ++Index)
            if (Value[Index] < '0' || Value[Index] > '9') return false;
        return true;
    };
    if (!Digits(0, 4) || !Digits(5, 2) || !Digits(8, 2)
        || !Digits(11, 2) || !Digits(14, 2) || !Digits(17, 2))
        return false;
    const unsigned Year = static_cast<unsigned>((Value[0] - '0') * 1000
        + (Value[1] - '0') * 100 + (Value[2] - '0') * 10 + Value[3] - '0');
    const unsigned Month = static_cast<unsigned>((Value[5] - '0') * 10 + Value[6] - '0');
    const unsigned Day = static_cast<unsigned>((Value[8] - '0') * 10 + Value[9] - '0');
    const unsigned Hour = static_cast<unsigned>((Value[11] - '0') * 10 + Value[12] - '0');
    const unsigned Minute = static_cast<unsigned>((Value[14] - '0') * 10 + Value[15] - '0');
    const unsigned Second = static_cast<unsigned>((Value[17] - '0') * 10 + Value[18] - '0');
    if (Month < 1U || Month > 12U || Hour > 23U || Minute > 59U || Second > 60U)
        return false;
    constexpr unsigned DaysByMonth[] = {31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U};
    const unsigned MaximumDay = Month == 2U && IsLeapYear(Year)
        ? 29U : DaysByMonth[Month - 1U];
    if (Day < 1U || Day > MaximumDay) return false;
    if (Value.size() == 20) return true;
    if (Value[19] != '.') return false;
    const std::size_t FractionDigits = Value.size() - 21U;
    return FractionDigits >= 1U && FractionDigits <= 9U
        && Digits(20, FractionDigits);
}

bool CoreGeometryIsValid(const SkiDomain::TerrainCoreManifest& Core) noexcept
{
    if (Core.SchemaVersion != SkiDomain::TerrainCoreSchema
        || !SkiDomain::IsTerrainCoreSha256(Core.ContentId)
        || Core.Width < 2U || Core.Height < 2U
        || !std::isfinite(Core.DeliveredEastSpacingM)
        || !std::isfinite(Core.DeliveredNorthSpacingM)
        || Core.DeliveredEastSpacingM <= 0.0 || Core.DeliveredNorthSpacingM <= 0.0
        || !SkiDomain::IsValidGeodetic(Core.LocalOrigin)
        || !FiniteBounds(Core.SampleCenterBounds) || !FiniteBounds(Core.OuterBounds)
        || Core.Registration != SkiDomain::PixelRegistration::SampleCenter
        || Core.RowOrientation != "north-to-south")
        return false;
    SkiDomain::MetricBounds ExpectedOuter;
    return SkiDomain::ComputeTerrainCoreBounds(Core.Width, Core.Height,
        Core.DeliveredEastSpacingM, Core.DeliveredNorthSpacingM,
        Core.SampleCenterBounds, ExpectedOuter) && SameBounds(ExpectedOuter, Core.OuterBounds)
        && BoundedSiteExtent(Core.OuterBounds);
}

bool HasFeatureBudget(const std::vector<OsmVectorLayer>& Layers,
    std::uint64_t& OutFeatures, std::uint64_t& OutPoints) noexcept
{
    OutFeatures = 0;
    OutPoints = 0;
    for (const OsmVectorLayer& Layer : Layers)
    {
        if (Layer.Features.size() > OsmVectorPackageMaxFeatures - OutFeatures) return false;
        OutFeatures += static_cast<std::uint64_t>(Layer.Features.size());
        for (const OsmVectorFeature& Feature : Layer.Features)
        {
            if (Feature.Points.size() > OsmVectorPackageMaxPoints - OutPoints) return false;
            OutPoints += static_cast<std::uint64_t>(Feature.Points.size());
        }
    }
    return true;
}

void SetGeodeticObject(const TSharedRef<FJsonObject>& Object,
    const SkiDomain::GeodeticPoint& Point)
{
    Object->SetNumberField(TEXT("latitudeDeg"), CanonicalDouble(Point.LatitudeDeg));
    Object->SetNumberField(TEXT("longitudeDeg"), CanonicalDouble(Point.LongitudeDeg));
    Object->SetNumberField(TEXT("heightM"), CanonicalDouble(Point.HeightM));
}

void SetBoundsObject(const TSharedRef<FJsonObject>& Object, const TCHAR* Name,
    const SkiDomain::MetricBounds& Bounds)
{
    TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetNumberField(TEXT("westM"), CanonicalDouble(Bounds.WestM));
    Value->SetNumberField(TEXT("southM"), CanonicalDouble(Bounds.SouthM));
    Value->SetNumberField(TEXT("eastM"), CanonicalDouble(Bounds.EastM));
    Value->SetNumberField(TEXT("northM"), CanonicalDouble(Bounds.NorthM));
    Object->SetObjectField(Name, Value);
}

TSharedRef<FJsonObject> MakePackageObject(const OsmVectorPackage& Package,
    const bool IncludeContentId, const Cancellation* CancellationValue, bool& bCancelled)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Package.SchemaVersion);
    if (IncludeContentId)
        Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Package.ContentId.c_str()));
    Root->SetStringField(TEXT("terrainCoreId"), UTF8_TO_TCHAR(Package.TerrainCoreId.c_str()));
    Root->SetStringField(TEXT("coordinateFrame"), UTF8_TO_TCHAR(OsmVectorCoordinateFrame));

    TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
    SetGeodeticObject(Origin, Package.LocalOrigin);
    Root->SetObjectField(TEXT("localOrigin"), Origin);

    TSharedRef<FJsonObject> Core = MakeShared<FJsonObject>();
    Core->SetNumberField(TEXT("width"), Package.TerrainCoreWidth);
    Core->SetNumberField(TEXT("height"), Package.TerrainCoreHeight);
    Core->SetNumberField(TEXT("eastSpacingM"), Package.TerrainCoreEastSpacingM);
    Core->SetNumberField(TEXT("northSpacingM"), Package.TerrainCoreNorthSpacingM);
    Root->SetObjectField(TEXT("terrainCoreGrid"), Core);
    SetBoundsObject(Root, TEXT("extentM"), Package.ExtentM);

    TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("provider"), UTF8_TO_TCHAR(Package.Source.Provider.c_str()));
    Source->SetStringField(TEXT("endpoint"), UTF8_TO_TCHAR(Package.Source.Endpoint.c_str()));
    Source->SetStringField(TEXT("sourceTimestampUtc"), UTF8_TO_TCHAR(Package.Source.SourceTimestampUtc.c_str()));
    Source->SetStringField(TEXT("retrievedAtUtc"), UTF8_TO_TCHAR(Package.Source.RetrievedAtUtc.c_str()));
    Source->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Package.Source.License.c_str()));
    Source->SetStringField(TEXT("attribution"), UTF8_TO_TCHAR(Package.Source.Attribution.c_str()));
    Source->SetStringField(TEXT("attributionUrl"), UTF8_TO_TCHAR(Package.Source.AttributionUrl.c_str()));
    Root->SetObjectField(TEXT("source"), Source);

    std::vector<const OsmVectorLayer*> OrderedLayers;
    OrderedLayers.reserve(Package.Layers.size());
    for (const OsmVectorLayer& Layer : Package.Layers) OrderedLayers.push_back(&Layer);
    std::sort(OrderedLayers.begin(), OrderedLayers.end(), [](const OsmVectorLayer* A,
        const OsmVectorLayer* B)
    {
        return static_cast<std::uint8_t>(A->Kind) < static_cast<std::uint8_t>(B->Kind);
    });

    TArray<TSharedPtr<FJsonValue>> LayerValues;
    LayerValues.Reserve(static_cast<int32>(OrderedLayers.size()));
    std::uint64_t VisitedPoints = 0;
    for (const OsmVectorLayer* Layer : OrderedLayers)
    {
        if (CancellationValue != nullptr && CancellationValue->IsCancelled())
        {
            bCancelled = true;
            return Root;
        }
        TSharedRef<FJsonObject> LayerObject = MakeShared<FJsonObject>();
        LayerObject->SetStringField(TEXT("layer"), UTF8_TO_TCHAR(OsmVectorLayerName(Layer->Kind)));
        LayerObject->SetStringField(TEXT("selector"), UTF8_TO_TCHAR(OsmVectorLayerSelector(Layer->Kind)));

        std::vector<const OsmVectorFeature*> OrderedFeatures;
        OrderedFeatures.reserve(Layer->Features.size());
        for (const OsmVectorFeature& Feature : Layer->Features) OrderedFeatures.push_back(&Feature);
        std::sort(OrderedFeatures.begin(), OrderedFeatures.end(), [](const OsmVectorFeature* A,
            const OsmVectorFeature* B)
        {
            if (A->ElementKind != B->ElementKind)
                return static_cast<std::uint8_t>(A->ElementKind) < static_cast<std::uint8_t>(B->ElementKind);
            if (A->OsmId != B->OsmId) return A->OsmId < B->OsmId;
            return A->PartIndex < B->PartIndex;
        });

        TArray<TSharedPtr<FJsonValue>> FeatureValues;
        FeatureValues.Reserve(static_cast<int32>(OrderedFeatures.size()));
        for (const OsmVectorFeature* Feature : OrderedFeatures)
        {
            if (CancellationValue != nullptr && CancellationValue->IsCancelled())
            {
                bCancelled = true;
                return Root;
            }
            TSharedRef<FJsonObject> FeatureObject = MakeShared<FJsonObject>();
            FeatureObject->SetStringField(TEXT("element"), Feature->ElementKind == OsmElementKind::Way
                ? TEXT("way") : TEXT("relation"));
            FeatureObject->SetStringField(TEXT("osmId"), LexToString(Feature->OsmId));
            if (Package.SchemaVersion >= OsmVectorPackageSchema)
                FeatureObject->SetNumberField(TEXT("partIndex"), Feature->PartIndex);
            TArray<TSharedPtr<FJsonValue>> PointValues;
            PointValues.Reserve(static_cast<int32>(Feature->Points.size()));
            for (const OsmVectorPoint& Point : Feature->Points)
            {
                if (((++VisitedPoints) & 1023ULL) == 0ULL && CancellationValue != nullptr
                    && CancellationValue->IsCancelled())
                {
                    bCancelled = true;
                    return Root;
                }
                TSharedRef<FJsonObject> PointObject = MakeShared<FJsonObject>();
                PointObject->SetNumberField(TEXT("eastM"), Point.EastM == 0.0 ? 0.0 : Point.EastM);
                PointObject->SetNumberField(TEXT("northM"), Point.NorthM == 0.0 ? 0.0 : Point.NorthM);
                PointValues.Add(MakeShared<FJsonValueObject>(PointObject));
            }
            FeatureObject->SetArrayField(TEXT("points"), PointValues);
            FeatureValues.Add(MakeShared<FJsonValueObject>(FeatureObject));
        }
        LayerObject->SetArrayField(TEXT("features"), FeatureValues);
        LayerValues.Add(MakeShared<FJsonValueObject>(LayerObject));
    }
    Root->SetArrayField(TEXT("layers"), LayerValues);
    return Root;
}

FString SerializeInternal(const OsmVectorPackage& Package, const bool IncludeContentId,
    const Cancellation* CancellationValue, bool& bCancelled)
{
    bCancelled = false;
    std::uint64_t FeatureCount = 0;
    std::uint64_t PointCount = 0;
    if (Package.Layers.size() > 3U || Package.TerrainCoreId.size() > 64U
        || Package.ContentId.size() > 64U || (IncludeContentId && Package.ContentId.empty())
        || !IsSafeText(Package.Source.Provider, 128)
        || !IsSafeText(Package.Source.Endpoint, 256)
        || Package.Source.SourceTimestampUtc.size() > 30U
        || Package.Source.RetrievedAtUtc.size() > 30U
        || Package.Source.License.size() > 128U || Package.Source.Attribution.size() > 128U
        || Package.Source.AttributionUrl.size() > 256U
        || !HasFeatureBudget(Package.Layers, FeatureCount, PointCount))
        return {};
    FString Output;
    const TSharedRef<FJsonObject> Root = MakePackageObject(Package, IncludeContentId,
        CancellationValue, bCancelled);
    if (bCancelled) return {};
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
    if (!FJsonSerializer::Serialize(Root, Writer)) return {};
    const FTCHARToUTF8 Encoded(*Output);
    if (Encoded.Length() <= 0
        || static_cast<std::uint64_t>(Encoded.Length()) > OsmVectorPackageMaxBytes)
        return {};
    return Output;
}

FString ContentIdInternal(const OsmVectorPackage& Package,
    const Cancellation* CancellationValue, bool& bCancelled)
{
    const FString Json = SerializeInternal(Package, false, CancellationValue, bCancelled);
    if (Json.IsEmpty() || bCancelled) return {};
    TArray<uint8> Bytes;
    if (!ToUtf8Bytes(Json, Bytes) || Bytes.IsEmpty()) return {};
    return Sha256(TArrayView<const uint8>(Bytes.GetData(), Bytes.Num()));
}

bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
    std::string& Out)
{
    FString Value;
    if (!Object || !Object->TryGetStringField(Name, Value)) return false;
    Out = Utf8(Value);
    return true;
}

bool ReadUint32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
    std::uint32_t& Out)
{
    double Value = 0.0;
    if (!Object || !Object->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value)
        || Value < 0.0 || Value > static_cast<double>(MAX_uint32)
        || FMath::FloorToDouble(Value) != Value)
        return false;
    Out = static_cast<std::uint32_t>(Value);
    return true;
}

bool ReadFiniteNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
    double& Out)
{
    return Object && Object->TryGetNumberField(Name, Out) && FMath::IsFinite(Out);
}

bool ReadObjectField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name,
    TSharedPtr<FJsonObject>& Out)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    if (!Parent || !Parent->TryGetObjectField(Name, Value) || !Value || !Value->IsValid())
        return false;
    Out = *Value;
    return true;
}

bool ReadGeodeticPoint(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::GeodeticPoint& Out)
{
    return ReadFiniteNumber(Object, TEXT("latitudeDeg"), Out.LatitudeDeg)
        && ReadFiniteNumber(Object, TEXT("longitudeDeg"), Out.LongitudeDeg)
        && ReadFiniteNumber(Object, TEXT("heightM"), Out.HeightM);
}

bool ReadBounds(const TSharedPtr<FJsonObject>& Object, SkiDomain::MetricBounds& Out)
{
    return ReadFiniteNumber(Object, TEXT("westM"), Out.WestM)
        && ReadFiniteNumber(Object, TEXT("southM"), Out.SouthM)
        && ReadFiniteNumber(Object, TEXT("eastM"), Out.EastM)
        && ReadFiniteNumber(Object, TEXT("northM"), Out.NorthM);
}

bool ParseLayer(const FString& Name, OsmVectorLayerKind& Out)
{
    if (Name == TEXT("road")) Out = OsmVectorLayerKind::Road;
    else if (Name == TEXT("lift")) Out = OsmVectorLayerKind::Lift;
    else if (Name == TEXT("trail")) Out = OsmVectorLayerKind::Trail;
    else return false;
    return true;
}

bool ParseFeature(const TSharedPtr<FJsonObject>& Object, const std::uint32_t SchemaVersion,
    OsmVectorFeature& Out, FString& OutError)
{
    FString Element;
    std::string Id;
    if (!Object || !Object->TryGetStringField(TEXT("element"), Element)
        || !ReadString(Object, TEXT("osmId"), Id) || Id.empty()
        || !LexTryParseString(Out.OsmId, UTF8_TO_TCHAR(Id.c_str())))
    {
        OutError = TEXT("OSM vector feature identity is missing or malformed.");
        return false;
    }
    if (Element == TEXT("way")) Out.ElementKind = OsmElementKind::Way;
    else if (Element == TEXT("relation")) Out.ElementKind = OsmElementKind::Relation;
    else
    {
        OutError = TEXT("OSM vector features must identify a way or relation.");
        return false;
    }
    if (SchemaVersion >= OsmVectorPackageSchema
        && !ReadUint32(Object, TEXT("partIndex"), Out.PartIndex))
    {
        OutError = TEXT("OSM vector multipart index is missing or malformed.");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
    if (!Object->TryGetArrayField(TEXT("points"), Points) || !Points
        || Points->Num() > static_cast<int32>(OsmVectorFeatureMaxPoints))
    {
        OutError = TEXT("OSM vector geometry is missing or oversized.");
        return false;
    }
    Out.Points.reserve(static_cast<std::size_t>(Points->Num()));
    for (const TSharedPtr<FJsonValue>& Item : *Points)
    {
        const TSharedPtr<FJsonObject> PointObject = Item ? Item->AsObject() : nullptr;
        double East = 0.0;
        double North = 0.0;
        if (!ReadFiniteNumber(PointObject, TEXT("eastM"), East)
            || !ReadFiniteNumber(PointObject, TEXT("northM"), North))
        {
            OutError = TEXT("OSM vector point must contain two finite ENU coordinates.");
            return false;
        }
        Out.Points.push_back({East, North});
    }
    return true;
}

OsmVectorPackageValidation ValidateShapeAndBinding(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation* CancellationValue, std::uint64_t& OutFeatureCount,
    std::uint64_t& OutPointCount) noexcept
{
    OutFeatureCount = 0;
    OutPointCount = 0;
    if (CancellationValue != nullptr && CancellationValue->IsCancelled())
        return {OsmVectorPackageError::Cancelled};
    if (!CoreGeometryIsValid(TerrainCore)) return {OsmVectorPackageError::InvalidTerrainCore};
    if (Package.SchemaVersion != OsmVectorPackageLegacySchema
        && Package.SchemaVersion != OsmVectorPackageSchema)
        return {OsmVectorPackageError::UnsupportedSchema};
    if (Package.TerrainCoreId != TerrainCore.ContentId
        || !SamePoint(Package.LocalOrigin, TerrainCore.LocalOrigin)
        || Package.TerrainCoreWidth != TerrainCore.Width
        || Package.TerrainCoreHeight != TerrainCore.Height
        || !NearlyEqual(Package.TerrainCoreEastSpacingM, TerrainCore.DeliveredEastSpacingM)
        || !NearlyEqual(Package.TerrainCoreNorthSpacingM, TerrainCore.DeliveredNorthSpacingM)
        || !SameBounds(Package.ExtentM, TerrainCore.OuterBounds))
        return {OsmVectorPackageError::TerrainCoreMismatch};

    if (Package.Layers.size() > RequiredLayerOrder.size())
        return {OsmVectorPackageError::InvalidLayer};
    if (!IsSafeText(Package.Source.Provider, 128)
        || !IsSafeText(Package.Source.Endpoint, 256)
        || Package.Source.Provider != ExpectedProvider
        || Package.Source.Endpoint != ExpectedEndpoint
        || !IsSafeText(Package.Source.SourceTimestampUtc, 30)
        || !IsSafeText(Package.Source.RetrievedAtUtc, 30))
        return {OsmVectorPackageError::InvalidMetadata};
    if (!IsUtcTimestamp(Package.Source.SourceTimestampUtc)
        || !IsUtcTimestamp(Package.Source.RetrievedAtUtc))
        return {OsmVectorPackageError::InvalidSourceTimestamp};
    if (Package.Source.License != OsmVectorOsmLicense)
        return {OsmVectorPackageError::InvalidLicense};
    if (Package.Source.Attribution != OsmVectorAttribution
        || Package.Source.AttributionUrl != OsmVectorAttributionUrl)
        return {OsmVectorPackageError::InvalidAttribution};
    if (!FiniteBounds(Package.ExtentM)) return {OsmVectorPackageError::TerrainCoreMismatch};

    bool Present[3] = {false, false, false};
    for (std::size_t LayerIndex = 0; LayerIndex < Package.Layers.size(); ++LayerIndex)
    {
        if (CancellationValue != nullptr && CancellationValue->IsCancelled())
            return {OsmVectorPackageError::Cancelled, static_cast<int32>(LayerIndex)};
        const OsmVectorLayer& Layer = Package.Layers[LayerIndex];
        const auto Kind = static_cast<std::uint8_t>(Layer.Kind);
        if (Kind >= RequiredLayerOrder.size())
            return {OsmVectorPackageError::InvalidLayer, static_cast<int32>(LayerIndex)};
        if (Present[Kind])
            return {OsmVectorPackageError::DuplicateLayer, static_cast<int32>(LayerIndex)};
        Present[Kind] = true;
    }
    for (std::size_t Index = 0; Index < 3U; ++Index)
        if (!Present[Index]) return {OsmVectorPackageError::MissingLayer, static_cast<int32>(Index)};

    using FFeatureKey = std::tuple<std::uint8_t, std::uint64_t, std::uint32_t, std::uint8_t>;
    std::vector<FFeatureKey> FeatureKeys;
    FeatureKeys.reserve(static_cast<std::size_t>(OsmVectorPackageMaxFeatures));
    for (std::size_t LayerIndex = 0; LayerIndex < Package.Layers.size(); ++LayerIndex)
    {
        if (CancellationValue != nullptr && CancellationValue->IsCancelled())
            return {OsmVectorPackageError::Cancelled, static_cast<int32>(LayerIndex)};
        const OsmVectorLayer& Layer = Package.Layers[LayerIndex];
        for (std::size_t FeatureIndex = 0; FeatureIndex < Layer.Features.size(); ++FeatureIndex)
        {
            if (CancellationValue != nullptr && CancellationValue->IsCancelled())
                return {OsmVectorPackageError::Cancelled, static_cast<int32>(LayerIndex),
                    static_cast<int32>(FeatureIndex)};
            if (OutFeatureCount >= OsmVectorPackageMaxFeatures)
                return {OsmVectorPackageError::TooManyFeatures, static_cast<int32>(LayerIndex),
                    static_cast<int32>(FeatureIndex)};
            ++OutFeatureCount;
            const OsmVectorFeature& Feature = Layer.Features[FeatureIndex];
            if ((Feature.ElementKind != OsmElementKind::Way
                    && Feature.ElementKind != OsmElementKind::Relation) || Feature.OsmId == 0)
                return {OsmVectorPackageError::InvalidFeatureId, static_cast<int32>(LayerIndex),
                    static_cast<int32>(FeatureIndex)};
            if (Package.SchemaVersion == OsmVectorPackageLegacySchema && Feature.PartIndex != 0)
                return {OsmVectorPackageError::InvalidPartIndex, static_cast<int32>(LayerIndex),
                    static_cast<int32>(FeatureIndex)};
            FeatureKeys.emplace_back(static_cast<std::uint8_t>(Feature.ElementKind), Feature.OsmId,
                Feature.PartIndex, static_cast<std::uint8_t>(Layer.Kind));
            if (Feature.Points.size() < 2U || Feature.Points.size() > OsmVectorFeatureMaxPoints)
                return {OsmVectorPackageError::MalformedGeometry, static_cast<int32>(LayerIndex),
                    static_cast<int32>(FeatureIndex)};
            for (std::size_t PointIndex = 0; PointIndex < Feature.Points.size(); ++PointIndex)
            {
                if (CancellationValue != nullptr && (PointIndex & 1023U) == 0U
                    && CancellationValue->IsCancelled())
                    return {OsmVectorPackageError::Cancelled, static_cast<int32>(LayerIndex),
                        static_cast<int32>(FeatureIndex), static_cast<int32>(PointIndex)};
                if (OutPointCount >= OsmVectorPackageMaxPoints)
                    return {OsmVectorPackageError::TooManyPoints, static_cast<int32>(LayerIndex),
                        static_cast<int32>(FeatureIndex), static_cast<int32>(PointIndex)};
                ++OutPointCount;
                const OsmVectorPoint& Point = Feature.Points[PointIndex];
                if (!std::isfinite(Point.EastM) || !std::isfinite(Point.NorthM))
                    return {OsmVectorPackageError::MalformedGeometry, static_cast<int32>(LayerIndex),
                        static_cast<int32>(FeatureIndex), static_cast<int32>(PointIndex)};
                if (Point.EastM < Package.ExtentM.WestM || Point.EastM > Package.ExtentM.EastM
                    || Point.NorthM < Package.ExtentM.SouthM || Point.NorthM > Package.ExtentM.NorthM)
                    return {OsmVectorPackageError::OutOfBounds, static_cast<int32>(LayerIndex),
                        static_cast<int32>(FeatureIndex), static_cast<int32>(PointIndex)};
                if (PointIndex > 0U)
                {
                    const OsmVectorPoint& Previous = Feature.Points[PointIndex - 1U];
                    if (Point.EastM == Previous.EastM && Point.NorthM == Previous.NorthM)
                        return {OsmVectorPackageError::MalformedGeometry, static_cast<int32>(LayerIndex),
                            static_cast<int32>(FeatureIndex), static_cast<int32>(PointIndex)};
                }
            }
        }
    }
    std::sort(FeatureKeys.begin(), FeatureKeys.end());
    std::uint8_t PreviousKind = 0;
    std::uint8_t PreviousLayer = 0;
    std::uint64_t PreviousId = 0;
    std::uint32_t ExpectedPartIndex = 0;
    bool bHavePreviousIdentity = false;
    for (const FFeatureKey& Key : FeatureKeys)
    {
        const std::uint8_t Kind = std::get<0>(Key);
        const std::uint64_t Id = std::get<1>(Key);
        const std::uint32_t PartIndex = std::get<2>(Key);
        const std::uint8_t Layer = std::get<3>(Key);
        if (!bHavePreviousIdentity || Kind != PreviousKind || Id != PreviousId)
        {
            PreviousKind = Kind;
            PreviousId = Id;
            PreviousLayer = Layer;
            ExpectedPartIndex = 0;
            bHavePreviousIdentity = true;
        }
        else if (Layer != PreviousLayer)
        {
            return {OsmVectorPackageError::DuplicateFeatureId};
        }
        if (PartIndex < ExpectedPartIndex)
            return {OsmVectorPackageError::DuplicateFeatureId};
        if (PartIndex != ExpectedPartIndex)
            return {OsmVectorPackageError::InvalidPartIndex};
        ++ExpectedPartIndex;
    }
    return {};
}

FString ErrorText(const OsmVectorPackageError Error)
{
    switch (Error)
    {
    case OsmVectorPackageError::Cancelled: return TEXT("OSM vector package operation was cancelled.");
    case OsmVectorPackageError::UnsupportedSchema: return TEXT("OSM vector package schema is unsupported.");
    case OsmVectorPackageError::PackageTooLarge: return TEXT("OSM vector package exceeds its byte limit.");
    case OsmVectorPackageError::InvalidTerrainCore: return TEXT("TerrainCore geometry is invalid.");
    case OsmVectorPackageError::TerrainCoreMismatch: return TEXT("OSM vectors do not match the TerrainCore identity and extent.");
    case OsmVectorPackageError::InvalidMetadata: return TEXT("OSM source provenance is missing or malformed.");
    case OsmVectorPackageError::InvalidSourceTimestamp: return TEXT("OSM source timestamps must be valid ISO-8601 UTC instants.");
    case OsmVectorPackageError::InvalidLicense: return TEXT("OSM source license must be ODbL-1.0.");
    case OsmVectorPackageError::InvalidAttribution: return TEXT("Required OpenStreetMap attribution is missing or altered.");
    case OsmVectorPackageError::MissingLayer: return TEXT("A required normalized OSM layer is missing.");
    case OsmVectorPackageError::DuplicateLayer: return TEXT("A normalized OSM layer is duplicated.");
    case OsmVectorPackageError::InvalidLayer: return TEXT("An OSM vector layer is invalid.");
    case OsmVectorPackageError::TooManyFeatures: return TEXT("OSM vector feature count exceeds its limit.");
    case OsmVectorPackageError::TooManyPoints: return TEXT("OSM vector point count exceeds its limit.");
    case OsmVectorPackageError::DuplicateFeatureId: return TEXT("An OSM element appears more than once.");
    case OsmVectorPackageError::InvalidFeatureId: return TEXT("OSM way and relation IDs must be positive.");
    case OsmVectorPackageError::MalformedGeometry: return TEXT("OSM polyline geometry is malformed.");
    case OsmVectorPackageError::OutOfBounds: return TEXT("OSM geometry escapes the TerrainCore extent.");
    case OsmVectorPackageError::InvalidContentId: return TEXT("OSM vector content ID is not a SHA-256 value.");
    case OsmVectorPackageError::ContentHashMismatch: return TEXT("OSM vector content hash does not match its canonical package.");
    case OsmVectorPackageError::InvalidSerializedPackage: return TEXT("OSM vector package serialization is invalid.");
    case OsmVectorPackageError::InvalidPartIndex: return TEXT("OSM multipart indices must be unique and contiguous from zero.");
    default: return {};
    }
}
}

const char* SkiPreparation::OsmVectorLayerName(const OsmVectorLayerKind Layer) noexcept
{
    switch (Layer)
    {
    case OsmVectorLayerKind::Road: return "road";
    case OsmVectorLayerKind::Lift: return "lift";
    case OsmVectorLayerKind::Trail: return "trail";
    default: return "invalid";
    }
}

const char* SkiPreparation::OsmVectorLayerSelector(const OsmVectorLayerKind Layer) noexcept
{
    switch (Layer)
    {
    case OsmVectorLayerKind::Road: return "highway";
    case OsmVectorLayerKind::Lift: return "aerialway";
    case OsmVectorLayerKind::Trail: return "piste:type";
    default: return "invalid";
    }
}

SkiPreparation::OsmVectorPackageValidation SkiPreparation::ValidateOsmVectorPackage(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation* CancellationValue) noexcept
{
    std::uint64_t FeatureCount = 0;
    std::uint64_t PointCount = 0;
    const OsmVectorPackageValidation Shape = ValidateShapeAndBinding(TerrainCore, Package,
        CancellationValue, FeatureCount, PointCount);
    if (!Shape.Ok()) return Shape;
    if (!SkiDomain::IsTerrainCoreSha256(Package.ContentId))
        return {OsmVectorPackageError::InvalidContentId};
    bool bCancelled = false;
    const FString Computed = ContentIdInternal(Package, CancellationValue, bCancelled);
    if (bCancelled) return {OsmVectorPackageError::Cancelled};
    if (Computed.IsEmpty()) return {OsmVectorPackageError::PackageTooLarge};
    if (Computed != UTF8_TO_TCHAR(Package.ContentId.c_str()))
        return {OsmVectorPackageError::ContentHashMismatch};
    return {};
}

FString SkiPreparation::SerializeOsmVectorPackage(const OsmVectorPackage& Package,
    const bool IncludeContentId)
{
    bool bCancelled = false;
    return SerializeInternal(Package, IncludeContentId, nullptr, bCancelled);
}

FString SkiPreparation::ComputeOsmVectorPackageContentId(const OsmVectorPackage& Package)
{
    bool bCancelled = false;
    return ContentIdInternal(Package, nullptr, bCancelled);
}

bool SkiPreparation::ParseOsmVectorPackage(const FString& Json,
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    OsmVectorPackage& OutPackage, FString& OutError,
    const Cancellation* CancellationValue)
{
    OutPackage = {};
    OutError.Reset();
    if (CancellationValue != nullptr && CancellationValue->IsCancelled())
    {
        OutError = ErrorText(OsmVectorPackageError::Cancelled);
        return false;
    }
    const FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0
        || static_cast<std::uint64_t>(Encoded.Length()) > OsmVectorPackageMaxBytes)
    {
        OutError = ErrorText(OsmVectorPackageError::PackageTooLarge);
        return false;
    }
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root)
    {
        OutError = ErrorText(OsmVectorPackageError::InvalidSerializedPackage);
        return false;
    }
    if (CancellationValue != nullptr && CancellationValue->IsCancelled())
    {
        OutError = ErrorText(OsmVectorPackageError::Cancelled);
        return false;
    }

    OsmVectorPackage Value;
    FString Frame;
    if (!ReadUint32(Root, TEXT("schemaVersion"), Value.SchemaVersion)
        || !ReadString(Root, TEXT("contentId"), Value.ContentId)
        || !ReadString(Root, TEXT("terrainCoreId"), Value.TerrainCoreId)
        || !Root->TryGetStringField(TEXT("coordinateFrame"), Frame)
        || Frame != UTF8_TO_TCHAR(OsmVectorCoordinateFrame))
    {
        OutError = ErrorText(OsmVectorPackageError::InvalidSerializedPackage);
        return false;
    }
    if (Value.SchemaVersion != OsmVectorPackageLegacySchema
        && Value.SchemaVersion != OsmVectorPackageSchema)
    {
        OutError = ErrorText(OsmVectorPackageError::UnsupportedSchema);
        return false;
    }
    TSharedPtr<FJsonObject> OriginObject;
    TSharedPtr<FJsonObject> CoreObject;
    TSharedPtr<FJsonObject> ExtentObject;
    TSharedPtr<FJsonObject> SourceObject;
    if (!ReadObjectField(Root, TEXT("localOrigin"), OriginObject)
        || !ReadGeodeticPoint(OriginObject, Value.LocalOrigin)
        || !ReadObjectField(Root, TEXT("terrainCoreGrid"), CoreObject)
        || !ReadUint32(CoreObject, TEXT("width"), Value.TerrainCoreWidth)
        || !ReadUint32(CoreObject, TEXT("height"), Value.TerrainCoreHeight)
        || !ReadFiniteNumber(CoreObject, TEXT("eastSpacingM"), Value.TerrainCoreEastSpacingM)
        || !ReadFiniteNumber(CoreObject, TEXT("northSpacingM"), Value.TerrainCoreNorthSpacingM)
        || !ReadObjectField(Root, TEXT("extentM"), ExtentObject)
        || !ReadBounds(ExtentObject, Value.ExtentM)
        || !ReadObjectField(Root, TEXT("source"), SourceObject)
        || !ReadString(SourceObject, TEXT("provider"), Value.Source.Provider)
        || !ReadString(SourceObject, TEXT("endpoint"), Value.Source.Endpoint)
        || !ReadString(SourceObject, TEXT("sourceTimestampUtc"), Value.Source.SourceTimestampUtc)
        || !ReadString(SourceObject, TEXT("retrievedAtUtc"), Value.Source.RetrievedAtUtc)
        || !ReadString(SourceObject, TEXT("license"), Value.Source.License)
        || !ReadString(SourceObject, TEXT("attribution"), Value.Source.Attribution)
        || !ReadString(SourceObject, TEXT("attributionUrl"), Value.Source.AttributionUrl))
    {
        OutError = ErrorText(OsmVectorPackageError::InvalidSerializedPackage);
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
    if (!Root->TryGetArrayField(TEXT("layers"), Layers) || !Layers || Layers->Num() > 3)
    {
        OutError = ErrorText(OsmVectorPackageError::MissingLayer);
        return false;
    }
    std::uint64_t ParsedFeatures = 0;
    std::uint64_t ParsedPoints = 0;
    Value.Layers.reserve(static_cast<std::size_t>(Layers->Num()));
    for (const TSharedPtr<FJsonValue>& Item : *Layers)
    {
        if (CancellationValue != nullptr && CancellationValue->IsCancelled())
        {
            OutError = ErrorText(OsmVectorPackageError::Cancelled);
            return false;
        }
        const TSharedPtr<FJsonObject> LayerObject = Item ? Item->AsObject() : nullptr;
        FString Name;
        FString Selector;
        OsmVectorLayer Layer;
        if (!LayerObject || !LayerObject->TryGetStringField(TEXT("layer"), Name)
            || !LayerObject->TryGetStringField(TEXT("selector"), Selector)
            || !ParseLayer(Name, Layer.Kind) || Selector != UTF8_TO_TCHAR(OsmVectorLayerSelector(Layer.Kind)))
        {
            OutError = ErrorText(OsmVectorPackageError::InvalidLayer);
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Features = nullptr;
        if (!LayerObject->TryGetArrayField(TEXT("features"), Features) || !Features
            || static_cast<std::uint64_t>(Features->Num()) > OsmVectorPackageMaxFeatures - ParsedFeatures)
        {
            OutError = ErrorText(OsmVectorPackageError::TooManyFeatures);
            return false;
        }
        ParsedFeatures += static_cast<std::uint64_t>(Features->Num());
        Layer.Features.reserve(static_cast<std::size_t>(Features->Num()));
        for (const TSharedPtr<FJsonValue>& FeatureValue : *Features)
        {
            if (CancellationValue != nullptr && CancellationValue->IsCancelled())
            {
                OutError = ErrorText(OsmVectorPackageError::Cancelled);
                return false;
            }
            const TSharedPtr<FJsonObject> FeatureObject = FeatureValue ? FeatureValue->AsObject() : nullptr;
            OsmVectorFeature Feature;
            if (!ParseFeature(FeatureObject, Value.SchemaVersion, Feature, OutError)) return false;
            if (Feature.Points.size() > OsmVectorPackageMaxPoints - ParsedPoints)
            {
                OutError = ErrorText(OsmVectorPackageError::TooManyPoints);
                return false;
            }
            ParsedPoints += static_cast<std::uint64_t>(Feature.Points.size());
            Layer.Features.push_back(std::move(Feature));
        }
        Value.Layers.push_back(std::move(Layer));
    }
    const OsmVectorPackageValidation Validation = ValidateOsmVectorPackage(
        TerrainCore, Value, CancellationValue);
    if (!Validation.Ok())
    {
        OutError = ErrorText(Validation.Error);
        return false;
    }
    OutPackage = std::move(Value);
    return true;
}

bool SkiPreparation::AcquireOsmVectorPackage(IOsmVectorProvider& Provider,
    const SkiDomain::TerrainCoreManifest& TerrainCore, const Cancellation& CancellationValue,
    OsmVectorPackage& OutPackage, FString& OutError)
{
    OutPackage = {};
    OutError.Reset();
    if (CancellationValue.IsCancelled())
    {
        OutError = ErrorText(OsmVectorPackageError::Cancelled);
        return false;
    }
    if (!CoreGeometryIsValid(TerrainCore))
    {
        OutError = ErrorText(OsmVectorPackageError::InvalidTerrainCore);
        return false;
    }
    OsmVectorProviderQuery Query;
    Query.TerrainCoreId = TerrainCore.ContentId;
    Query.LocalOrigin = TerrainCore.LocalOrigin;
    Query.ExtentM = TerrainCore.OuterBounds;
    for (const OsmVectorLayerKind Kind : RequiredLayerOrder)
        Query.RequiredSelectors.emplace_back(OsmVectorLayerSelector(Kind));

    OsmVectorProviderResponse Response;
    if (!Provider.Acquire(Query, CancellationValue, Response, OutError))
    {
        if (CancellationValue.IsCancelled()) OutError = ErrorText(OsmVectorPackageError::Cancelled);
        return false;
    }
    if (CancellationValue.IsCancelled())
    {
        OutError = ErrorText(OsmVectorPackageError::Cancelled);
        return false;
    }

    OsmVectorPackage Value;
    Value.TerrainCoreId = TerrainCore.ContentId;
    Value.LocalOrigin = TerrainCore.LocalOrigin;
    Value.TerrainCoreWidth = TerrainCore.Width;
    Value.TerrainCoreHeight = TerrainCore.Height;
    Value.TerrainCoreEastSpacingM = TerrainCore.DeliveredEastSpacingM;
    Value.TerrainCoreNorthSpacingM = TerrainCore.DeliveredNorthSpacingM;
    Value.ExtentM = TerrainCore.OuterBounds;
    Value.Source = std::move(Response.Source);
    Value.Layers = std::move(Response.Layers);
    std::uint64_t FeatureCount = 0;
    std::uint64_t PointCount = 0;
    const OsmVectorPackageValidation Shape = ValidateShapeAndBinding(
        TerrainCore, Value, &CancellationValue, FeatureCount, PointCount);
    if (!Shape.Ok())
    {
        OutError = ErrorText(Shape.Error);
        return false;
    }
    const FString ContentId = ComputeOsmVectorPackageContentId(Value);
    if (ContentId.IsEmpty())
    {
        OutError = ErrorText(OsmVectorPackageError::PackageTooLarge);
        return false;
    }
    Value.ContentId = Utf8(ContentId);
    const OsmVectorPackageValidation Validation = ValidateOsmVectorPackage(
        TerrainCore, Value, &CancellationValue);
    if (!Validation.Ok())
    {
        OutError = ErrorText(Validation.Error);
        return false;
    }
    OutPackage = std::move(Value);
    return true;
}

bool SkiPreparation::EstimateOsmVectorStorage(const std::uint64_t ExpectedFeatureCount,
    const std::uint64_t ExpectedPointCount, OsmVectorStorageEstimate& OutEstimate) noexcept
{
    OutEstimate = {};
    if (ExpectedFeatureCount > OsmVectorPackageMaxFeatures
        || ExpectedPointCount > OsmVectorPackageMaxPoints
        || ExpectedPointCount < ExpectedFeatureCount * 2ULL)
        return false;
    constexpr std::uint64_t JsonFixedBytes = 1024ULL;
    constexpr std::uint64_t MinFeatureBytes = 32ULL;
    constexpr std::uint64_t MaxFeatureBytes = 128ULL;
    constexpr std::uint64_t MinPointBytes = 18ULL;
    constexpr std::uint64_t MaxPointBytes = 64ULL;
    if (ExpectedFeatureCount > (OsmVectorPackageMaxBytes - JsonFixedBytes) / MaxFeatureBytes)
        return false;
    const std::uint64_t FeatureMaximum = ExpectedFeatureCount * MaxFeatureBytes;
    if (ExpectedPointCount > (OsmVectorPackageMaxBytes - JsonFixedBytes - FeatureMaximum)
        / MaxPointBytes)
        return false;
    OutEstimate.MinimumBytes = JsonFixedBytes + ExpectedFeatureCount * MinFeatureBytes
        + ExpectedPointCount * MinPointBytes;
    OutEstimate.MaximumBytes = JsonFixedBytes + FeatureMaximum + ExpectedPointCount * MaxPointBytes;
    OutEstimate.Certainty = OsmVectorStorageCertainty::Estimated;
    OutEstimate.Basis = TEXT("Estimated from canonical JSON framing with 32–128 bytes per feature and 18–64 bytes per ENU point; excludes SiteContext and installer overhead.");
    return true;
}

bool SkiPreparation::VerifyOsmVectorPackage(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation& CancellationValue, OsmVectorPackageVerificationReport& OutReport)
{
    OutReport = {};
    const OsmVectorPackageValidation Validation = ValidateOsmVectorPackage(
        TerrainCore, Package, &CancellationValue);
    if (!Validation.Ok())
    {
        OutReport.Error = Validation.Error;
        OutReport.FailureDetail = ErrorText(Validation.Error);
        return false;
    }
    std::uint64_t FeatureCount = 0;
    std::uint64_t PointCount = 0;
    if (!HasFeatureBudget(Package.Layers, FeatureCount, PointCount))
    {
        OutReport.Error = OsmVectorPackageError::PackageTooLarge;
        OutReport.FailureDetail = ErrorText(OutReport.Error);
        return false;
    }
    bool bCancelled = false;
    const FString Json = SerializeInternal(Package, true, &CancellationValue, bCancelled);
    if (bCancelled || CancellationValue.IsCancelled())
    {
        OutReport.Error = OsmVectorPackageError::Cancelled;
        OutReport.FailureDetail = ErrorText(OutReport.Error);
        return false;
    }
    TArray<uint8> Bytes;
    if (Json.IsEmpty() || !ToUtf8Bytes(Json, Bytes) || Bytes.IsEmpty())
    {
        OutReport.Error = OsmVectorPackageError::PackageTooLarge;
        OutReport.FailureDetail = ErrorText(OutReport.Error);
        return false;
    }
    if (CancellationValue.IsCancelled())
    {
        OutReport.Error = OsmVectorPackageError::Cancelled;
        OutReport.FailureDetail = ErrorText(OutReport.Error);
        return false;
    }
    OutReport.FeatureCount = FeatureCount;
    OutReport.PointCount = PointCount;
    OutReport.Storage.MinimumBytes = static_cast<std::uint64_t>(Bytes.Num());
    OutReport.Storage.MaximumBytes = OutReport.Storage.MinimumBytes;
    OutReport.Storage.Certainty = OsmVectorStorageCertainty::Exact;
    OutReport.Storage.Basis = TEXT("Exact UTF-8 byte count of the verified canonical OSM vector JSON asset; excludes SiteContext and installer overhead.");
    return true;
}
