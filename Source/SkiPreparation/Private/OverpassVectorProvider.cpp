#include "SkiPreparation/OverpassVectorProvider.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "SkiDomain/Coordinates.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Containers/StringConv.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using namespace SkiPreparation;

constexpr TCHAR OverpassEndpoint[] = TEXT("https://overpass-api.de/api/interpreter");
constexpr uint64 MaximumResponseBytes = 32ULL * 1024ULL * 1024ULL;
constexpr int32 MaximumQueryUrlCharacters = 16 * 1024;
constexpr uint64 MaximumRawElements = OsmVectorPackageMaxFeatures;
constexpr uint64 MaximumRawPoints = OsmVectorPackageMaxPoints;
constexpr uint64 MaximumRawRelationMembers = 50'000ULL;
constexpr double QueryHaloM = 100.0;
constexpr double MaximumSiteSideM = 10'000.0;
constexpr double MaximumLocalCoordinateM = 100'000.0;
constexpr double MaximumRawCoordinateM = 200'000.0;
constexpr double OperationDeadlineSeconds = 60.0;
constexpr int32 QueryTimeoutSeconds = 25;
constexpr double CoordinateJoinToleranceM = 1.0e-6;
constexpr uint64 MaximumExactlyRepresentableJsonInteger = 9'007'199'254'740'991ULL;

struct FGeographicQueryBounds
{
    double SouthDeg = 0.0;
    double WestDeg = 0.0;
    double NorthDeg = 0.0;
    double EastDeg = 0.0;
};

bool IsFiniteBounds(const SkiDomain::MetricBounds& Bounds)
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM < Bounds.EastM && Bounds.SouthM < Bounds.NorthM;
}

bool ValidateQuery(const OsmVectorProviderQuery& Query, FString& OutError)
{
    static const std::vector<std::string> ExpectedSelectors{
        "highway", "aerialway", "piste:type"};
    if (!SkiDomain::IsValidGeodetic(Query.LocalOrigin)
        || !IsFiniteBounds(Query.ExtentM)
        || Query.ExtentM.EastM - Query.ExtentM.WestM > MaximumSiteSideM
        || Query.ExtentM.NorthM - Query.ExtentM.SouthM > MaximumSiteSideM
        || std::abs(Query.ExtentM.WestM) > MaximumLocalCoordinateM
        || std::abs(Query.ExtentM.EastM) > MaximumLocalCoordinateM
        || std::abs(Query.ExtentM.SouthM) > MaximumLocalCoordinateM
        || std::abs(Query.ExtentM.NorthM) > MaximumLocalCoordinateM)
    {
        OutError = TEXT("OVERPASS_INVALID_QUERY_BOUNDS");
        return false;
    }
    if (Query.RequiredSelectors != ExpectedSelectors)
    {
        OutError = TEXT("OVERPASS_INVALID_QUERY_SELECTORS");
        return false;
    }
    SkiDomain::LocalFrame Frame;
    if (!SkiDomain::TryMakeLocalFrame(Query.LocalOrigin, Frame))
    {
        OutError = TEXT("OVERPASS_INVALID_LOCAL_FRAME");
        return false;
    }
    return true;
}

double NormalizeLongitude(const double LongitudeDeg)
{
    return std::remainder(LongitudeDeg, 360.0);
}

bool MakeGeographicBounds(const OsmVectorProviderQuery& Query,
    FGeographicQueryBounds& OutBounds)
{
    SkiDomain::LocalFrame Frame;
    if (!SkiDomain::TryMakeLocalFrame(Query.LocalOrigin, Frame)) return false;

    const double WestM = Query.ExtentM.WestM - QueryHaloM;
    const double EastM = Query.ExtentM.EastM + QueryHaloM;
    const double SouthM = Query.ExtentM.SouthM - QueryHaloM;
    const double NorthM = Query.ExtentM.NorthM + QueryHaloM;
    const double EastNorthCorners[4][2] = {
        {WestM, SouthM}, {WestM, NorthM}, {EastM, SouthM}, {EastM, NorthM},
    };

    double MinLatitude = 90.0;
    double MaxLatitude = -90.0;
    double MinUnwrappedLongitude = DBL_MAX;
    double MaxUnwrappedLongitude = -DBL_MAX;
    for (const auto& Corner : EastNorthCorners)
    {
        SkiDomain::GeodeticPoint Geographic;
        if (!SkiDomain::TrySeaLevelGeodeticFromEnu(Frame, Corner[0], Corner[1], Geographic))
            return false;
        MinLatitude = std::min(MinLatitude, Geographic.LatitudeDeg);
        MaxLatitude = std::max(MaxLatitude, Geographic.LatitudeDeg);
        const double Delta = std::remainder(Geographic.LongitudeDeg - Query.LocalOrigin.LongitudeDeg,
            360.0);
        const double Unwrapped = Query.LocalOrigin.LongitudeDeg + Delta;
        MinUnwrappedLongitude = std::min(MinUnwrappedLongitude, Unwrapped);
        MaxUnwrappedLongitude = std::max(MaxUnwrappedLongitude, Unwrapped);
    }
    if (!std::isfinite(MinLatitude) || !std::isfinite(MaxLatitude)
        || MaxLatitude - MinLatitude > 1.0
        || MaxUnwrappedLongitude - MinUnwrappedLongitude > 2.0)
        return false;

    OutBounds.SouthDeg = MinLatitude;
    OutBounds.NorthDeg = MaxLatitude;
    OutBounds.WestDeg = NormalizeLongitude(MinUnwrappedLongitude);
    OutBounds.EastDeg = NormalizeLongitude(MaxUnwrappedLongitude);
    return OutBounds.SouthDeg >= -90.0 && OutBounds.NorthDeg <= 90.0
        && std::isfinite(OutBounds.WestDeg) && std::isfinite(OutBounds.EastDeg);
}

FString Decimal(const double Value)
{
    return FString::Printf(TEXT("%.8f"), Value);
}

FString BuildOverpassQuery(const FGeographicQueryBounds& Bounds)
{
    const FString South = Decimal(Bounds.SouthDeg);
    const FString West = Decimal(Bounds.WestDeg);
    const FString North = Decimal(Bounds.NorthDeg);
    const FString East = Decimal(Bounds.EastDeg);
    // Keep full geometry so the client can distinguish every in-bounds run. Overpass's
    // geom(bbox) output can omit coordinates between runs without a delimiter, which
    // makes joining the returned points capable of inventing a segment.
    return FString::Printf(
        TEXT("[out:json][timeout:%d];(way[\"highway\"](%s,%s,%s,%s);"
             "way[\"aerialway\"](%s,%s,%s,%s);"
             "way[\"piste:type\"](%s,%s,%s,%s);"
             "relation[\"route\"=\"aerialway\"](%s,%s,%s,%s);"
             "relation[\"route\"=\"piste\"](%s,%s,%s,%s););"
             "out body geom;"),
        QueryTimeoutSeconds,
        *South, *West, *North, *East,
        *South, *West, *North, *East,
        *South, *West, *North, *East,
        *South, *West, *North, *East,
        *South, *West, *North, *East);
}

FString EncodeQueryParameter(const FString& Value)
{
    static constexpr TCHAR HexDigits[] = TEXT("0123456789ABCDEF");
    const FTCHARToUTF8 Utf8(*Value);
    FString Encoded;
    Encoded.Reserve(Utf8.Length() * 3);
    for (int32 Index = 0; Index < Utf8.Length(); ++Index)
    {
        const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
        const bool bUnreserved = (Byte >= 'a' && Byte <= 'z')
            || (Byte >= 'A' && Byte <= 'Z')
            || (Byte >= '0' && Byte <= '9')
            || Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';
        if (bUnreserved)
        {
            Encoded.AppendChar(static_cast<TCHAR>(Byte));
        }
        else
        {
            Encoded.AppendChar(TEXT('%'));
            Encoded.AppendChar(HexDigits[Byte >> 4]);
            Encoded.AppendChar(HexDigits[Byte & 0x0f]);
        }
    }
    return Encoded;
}

bool IsUtcTimestamp(const FString& Value)
{
    if (Value.Len() < 20 || Value.Len() > 30
        || Value[4] != TEXT('-') || Value[7] != TEXT('-') || Value[10] != TEXT('T')
        || Value[13] != TEXT(':') || Value[16] != TEXT(':') || Value[Value.Len() - 1] != TEXT('Z'))
        return false;
    auto Digits = [&Value](const int32 Begin, const int32 Count)
    {
        for (int32 Index = Begin; Index < Begin + Count; ++Index)
            if (Value[Index] < TEXT('0') || Value[Index] > TEXT('9')) return false;
        return true;
    };
    if (!Digits(0, 4) || !Digits(5, 2) || !Digits(8, 2)
        || !Digits(11, 2) || !Digits(14, 2) || !Digits(17, 2))
        return false;
    const int32 Year = (Value[0] - TEXT('0')) * 1000 + (Value[1] - TEXT('0')) * 100
        + (Value[2] - TEXT('0')) * 10 + Value[3] - TEXT('0');
    const int32 Month = (Value[5] - TEXT('0')) * 10 + Value[6] - TEXT('0');
    const int32 Day = (Value[8] - TEXT('0')) * 10 + Value[9] - TEXT('0');
    const int32 Hour = (Value[11] - TEXT('0')) * 10 + Value[12] - TEXT('0');
    const int32 Minute = (Value[14] - TEXT('0')) * 10 + Value[15] - TEXT('0');
    const int32 Second = (Value[17] - TEXT('0')) * 10 + Value[18] - TEXT('0');
    if (Month < 1 || Month > 12 || Hour > 23 || Minute > 59 || Second > 60) return false;
    const bool bLeapYear = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    constexpr int32 DaysByMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int32 MaximumDay = Month == 2 && bLeapYear ? 29 : DaysByMonth[Month - 1];
    if (Day < 1 || Day > MaximumDay) return false;
    if (Value.Len() == 20) return true;
    if (Value[19] != TEXT('.')) return false;
    const int32 FractionDigits = Value.Len() - 21;
    return FractionDigits >= 1 && FractionDigits <= 9 && Digits(20, FractionDigits);
}

FString RetrievedAtUtc()
{
    const FDateTime Now = FDateTime::UtcNow();
    return FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02dZ"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(), Now.GetHour(), Now.GetMinute(), Now.GetSecond());
}

bool ReadPositiveJsonNumber(const double Number, uint64& OutId)
{
    if (!FMath::IsFinite(Number) || Number < 1.0
        || Number > static_cast<double>(MaximumExactlyRepresentableJsonInteger)
        || FMath::FloorToDouble(Number) != Number)
        return false;
    OutId = static_cast<uint64>(Number);
    return OutId > 0;
}

bool ReadPositiveJsonId(const TSharedPtr<FJsonValue>& Value, uint64& OutId)
{
    return Value.IsValid() && Value->Type == EJson::Number
        && ReadPositiveJsonNumber(Value->AsNumber(), OutId);
}

bool ReadObjectNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double& OutValue)
{
    return Object.IsValid() && Object->TryGetNumberField(Name, OutValue) && FMath::IsFinite(OutValue);
}

bool SamePoint(const OsmVectorPoint& A, const OsmVectorPoint& B)
{
    return std::abs(A.EastM - B.EastM) <= CoordinateJoinToleranceM
        && std::abs(A.NorthM - B.NorthM) <= CoordinateJoinToleranceM;
}

void AppendDistinct(std::vector<OsmVectorPoint>& Points, const OsmVectorPoint& Point)
{
    if (Points.empty() || !SamePoint(Points.back(), Point)) Points.push_back(Point);
}

bool ClipSegment(const OsmVectorPoint& A, const OsmVectorPoint& B,
    const SkiDomain::MetricBounds& Bounds, OsmVectorPoint& OutA, OsmVectorPoint& OutB)
{
    const double Dx = B.EastM - A.EastM;
    const double Dy = B.NorthM - A.NorthM;
    double T0 = 0.0;
    double T1 = 1.0;
    const double P[4] = {-Dx, Dx, -Dy, Dy};
    const double Q[4] = {A.EastM - Bounds.WestM, Bounds.EastM - A.EastM,
        A.NorthM - Bounds.SouthM, Bounds.NorthM - A.NorthM};
    for (int32 Edge = 0; Edge < 4; ++Edge)
    {
        if (std::abs(P[Edge]) < 1.0e-12)
        {
            if (Q[Edge] < 0.0) return false;
            continue;
        }
        const double Ratio = Q[Edge] / P[Edge];
        if (P[Edge] < 0.0)
        {
            if (Ratio > T1) return false;
            T0 = std::max(T0, Ratio);
        }
        else
        {
            if (Ratio < T0) return false;
            T1 = std::min(T1, Ratio);
        }
    }
    if (T0 > T1) return false;
    auto At = [&](const double T)
    {
        return OsmVectorPoint{
            FMath::Clamp(A.EastM + T * Dx, Bounds.WestM, Bounds.EastM),
            FMath::Clamp(A.NorthM + T * Dy, Bounds.SouthM, Bounds.NorthM),
        };
    };
    OutA = At(T0);
    OutB = At(T1);
    return !SamePoint(OutA, OutB);
}

void FinishRun(std::vector<OsmVectorPoint>& Current, std::vector<std::vector<OsmVectorPoint>>& Runs)
{
    if (Current.size() >= 2) Runs.push_back(std::move(Current));
    Current.clear();
}

bool ClipPolyline(const std::vector<OsmVectorPoint>& Input,
    const SkiDomain::MetricBounds& Bounds, const double Deadline,
    const Cancellation& CancellationValue, std::vector<std::vector<OsmVectorPoint>>& OutRuns,
    FString& OutError)
{
    OutRuns.clear();
    std::vector<std::vector<OsmVectorPoint>> Runs;
    std::vector<OsmVectorPoint> Current;
    for (std::size_t Index = 1; Index < Input.size(); ++Index)
    {
        if ((Index & 1023U) == 1U && CancellationValue.IsCancelled())
        {
            OutError = TEXT("OVERPASS_CANCELLED");
            return false;
        }
        if ((Index & 1023U) == 1U && FPlatformTime::Seconds() >= Deadline)
        {
            OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
            return false;
        }
        if (SamePoint(Input[Index - 1], Input[Index])) continue;
        OsmVectorPoint ClippedA;
        OsmVectorPoint ClippedB;
        if (!ClipSegment(Input[Index - 1], Input[Index], Bounds, ClippedA, ClippedB))
        {
            FinishRun(Current, Runs);
            continue;
        }
        if (!Current.empty() && !SamePoint(Current.back(), ClippedA)) FinishRun(Current, Runs);
        AppendDistinct(Current, ClippedA);
        AppendDistinct(Current, ClippedB);
    }
    FinishRun(Current, Runs);
    for (const std::vector<OsmVectorPoint>& Run : Runs)
    {
        if (Run.size() > OsmVectorFeatureMaxPoints)
        {
            OutError = TEXT("OVERPASS_FEATURE_POINT_LIMIT");
            return false;
        }
    }
    OutRuns = std::move(Runs);
    return true;
}

bool ParseGeometry(const TArray<TSharedPtr<FJsonValue>>& Geometry,
    const SkiDomain::LocalFrame& LocalFrame, const double Deadline,
    const Cancellation& CancellationValue, uint64& RawGeometryPointCount,
    const TCHAR* MissingGeometryError, const TCHAR* MalformedGeometryError,
    std::vector<OsmVectorPoint>& OutPoints, FString& OutError)
{
    OutPoints.clear();
    if (Geometry.Num() < 2)
    {
        OutError = MissingGeometryError;
        return false;
    }
    if (static_cast<uint64>(Geometry.Num()) > OsmVectorFeatureMaxPoints
        || static_cast<uint64>(Geometry.Num()) > MaximumRawPoints - RawGeometryPointCount)
    {
        OutError = TEXT("OVERPASS_POINT_LIMIT");
        return false;
    }
    RawGeometryPointCount += static_cast<uint64>(Geometry.Num());
    OutPoints.reserve(static_cast<std::size_t>(Geometry.Num()));
    for (int32 GeometryIndex = 0; GeometryIndex < Geometry.Num(); ++GeometryIndex)
    {
        if ((GeometryIndex & 1023) == 0)
        {
            if (CancellationValue.IsCancelled())
            {
                OutError = TEXT("OVERPASS_CANCELLED");
                return false;
            }
            if (FPlatformTime::Seconds() >= Deadline)
            {
                OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
                return false;
            }
        }
        const TSharedPtr<FJsonValue>& GeometryValue = Geometry[GeometryIndex];
        const TSharedPtr<FJsonObject> Coordinate = GeometryValue
            ? GeometryValue->AsObject() : nullptr;
        double Latitude = 0.0;
        double Longitude = 0.0;
        if (!ReadObjectNumber(Coordinate, TEXT("lat"), Latitude)
            || !ReadObjectNumber(Coordinate, TEXT("lon"), Longitude))
        {
            OutError = MalformedGeometryError;
            return false;
        }
        const SkiDomain::GeodeticPoint Geographic{Latitude, Longitude, 0.0};
        if (!SkiDomain::IsValidGeodetic(Geographic))
        {
            OutError = TEXT("OVERPASS_GEOGRAPHIC_COORDINATE_OUT_OF_RANGE");
            return false;
        }
        const SkiDomain::EnuPoint Enu = SkiDomain::ToEnu(LocalFrame, Geographic);
        if (!std::isfinite(Enu.EastM) || !std::isfinite(Enu.NorthM)
            || std::abs(Enu.EastM) > MaximumRawCoordinateM
            || std::abs(Enu.NorthM) > MaximumRawCoordinateM)
        {
            OutError = TEXT("OVERPASS_LOCAL_COORDINATE_OUT_OF_RANGE");
            return false;
        }
        OutPoints.push_back({Enu.EastM, Enu.NorthM});
    }
    return true;
}

bool IsSupportedRelationNodeRole(const OsmVectorLayerKind Kind, const FString& Role)
{
    if (Kind == OsmVectorLayerKind::Trail) return Role == TEXT("start");
    return Role == TEXT("stop") || Role == TEXT("platform")
        || Role == TEXT("stop_entry_only") || Role == TEXT("stop_exit_only");
}

bool IsSupportedRelationWayRole(const FString& Role)
{
    return Role.IsEmpty() || Role == TEXT("forward") || Role == TEXT("backward");
}

bool AppendClippedParts(std::vector<std::vector<OsmVectorPoint>>& Runs,
    const OsmElementKind ElementKind, const uint64 OsmId, uint32& NextPartIndex,
    const OsmVectorLayerKind LayerKind, uint64& OutputFeatureCount, uint64& OutputPointCount,
    OsmVectorProviderResponse& OutResponse, FString& OutError)
{
    const std::size_t LayerIndex = static_cast<std::size_t>(LayerKind);
    if (LayerIndex >= OutResponse.Layers.size())
    {
        OutError = TEXT("OVERPASS_INVALID_LAYER");
        return false;
    }
    for (std::vector<OsmVectorPoint>& Run : Runs)
    {
        if (OutputFeatureCount >= MaximumRawElements)
        {
            OutError = TEXT("OVERPASS_FEATURE_LIMIT");
            return false;
        }
        if (Run.size() < 2U || Run.size() > OsmVectorFeatureMaxPoints)
        {
            OutError = TEXT("OVERPASS_FEATURE_POINT_LIMIT");
            return false;
        }
        if (Run.size() > MaximumRawPoints - OutputPointCount)
        {
            OutError = TEXT("OVERPASS_POINT_LIMIT");
            return false;
        }
        OsmVectorFeature Feature;
        Feature.ElementKind = ElementKind;
        Feature.OsmId = OsmId;
        Feature.PartIndex = NextPartIndex++;
        const uint64 RunPointCount = static_cast<uint64>(Run.size());
        Feature.Points = std::move(Run);
        OutResponse.Layers[LayerIndex].Features.push_back(std::move(Feature));
        ++OutputFeatureCount;
        OutputPointCount += RunPointCount;
    }
    return true;
}

bool ParseElements(const TArray<TSharedPtr<FJsonValue>>& Elements,
    const OsmVectorProviderQuery& Query, const SkiDomain::LocalFrame& LocalFrame,
    const double Deadline,
    const Cancellation& CancellationValue, OsmVectorProviderResponse& OutResponse,
    FString& OutError)
{
    if (static_cast<uint64>(Elements.Num()) > MaximumRawElements)
    {
        OutError = TEXT("OVERPASS_FEATURE_LIMIT");
        return false;
    }

    OutResponse.Layers = {
        {OsmVectorLayerKind::Road, {}},
        {OsmVectorLayerKind::Lift, {}},
        {OsmVectorLayerKind::Trail, {}},
    };
    std::unordered_set<std::string> SeenElementIds;
    SeenElementIds.reserve(static_cast<std::size_t>(Elements.Num()));
    uint64 RawNodeCount = 0;
    uint64 RawRelationMemberCount = 0;
    uint64 RawGeometryPointCount = 0;
    uint64 OutputFeatureCount = 0;
    uint64 OutputPointCount = 0;
    for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
    {
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("OVERPASS_CANCELLED");
            return false;
        }
        if (FPlatformTime::Seconds() >= Deadline)
        {
            OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
            return false;
        }
        const TSharedPtr<FJsonObject> Element = Elements[ElementIndex]
            ? Elements[ElementIndex]->AsObject() : nullptr;
        FString Type;
        if (!Element.IsValid() || !Element->TryGetStringField(TEXT("type"), Type)
            || (Type != TEXT("way") && Type != TEXT("relation")))
        {
            OutError = TEXT("OVERPASS_MALFORMED_ELEMENT_TYPE");
            return false;
        }
        uint64 OsmId = 0;
        double NumericId = 0.0;
        if (!Element->TryGetNumberField(TEXT("id"), NumericId)
            || !ReadPositiveJsonNumber(NumericId, OsmId))
        {
            OutError = Type == TEXT("way")
                ? TEXT("OVERPASS_MALFORMED_WAY_ID") : TEXT("OVERPASS_MALFORMED_RELATION_ID");
            return false;
        }
        const std::string ElementKey = std::string(Type == TEXT("way") ? "way/" : "relation/")
            + std::to_string(OsmId);
        if (!SeenElementIds.insert(ElementKey).second)
        {
            OutError = Type == TEXT("way")
                ? TEXT("OVERPASS_DUPLICATE_WAY_ID") : TEXT("OVERPASS_DUPLICATE_RELATION_ID");
            return false;
        }

        const TSharedPtr<FJsonObject>* TagsValue = nullptr;
        if (!Element->TryGetObjectField(TEXT("tags"), TagsValue) || !TagsValue || !TagsValue->IsValid())
        {
            OutError = TEXT("OVERPASS_MISSING_TAGS");
            return false;
        }

        OsmVectorLayerKind Kind = OsmVectorLayerKind::Road;
        if (Type == TEXT("way"))
        {
            FString Aerialway;
            FString PisteType;
            FString Highway;
            const bool bHasAerialway = (*TagsValue)->TryGetStringField(TEXT("aerialway"), Aerialway)
                && !Aerialway.IsEmpty();
            const bool bHasPiste = (*TagsValue)->TryGetStringField(TEXT("piste:type"), PisteType)
                && !PisteType.IsEmpty();
            const bool bHasHighway = (*TagsValue)->TryGetStringField(TEXT("highway"), Highway)
                && !Highway.IsEmpty();
            // Prefer the specific lift/piste classification when a way also carries highway.
            if (bHasAerialway) Kind = OsmVectorLayerKind::Lift;
            else if (bHasPiste) Kind = OsmVectorLayerKind::Trail;
            else if (!bHasHighway)
            {
                OutError = TEXT("OVERPASS_WAY_MISSING_REQUIRED_SELECTOR");
                return false;
            }
        }
        else
        {
            FString RelationType;
            FString Route;
            if (!(*TagsValue)->TryGetStringField(TEXT("type"), RelationType)
                || RelationType != TEXT("route")
                || !(*TagsValue)->TryGetStringField(TEXT("route"), Route))
            {
                OutError = TEXT("OVERPASS_UNSUPPORTED_RELATION_TYPE");
                return false;
            }
            if (Route == TEXT("piste"))
            {
                FString PisteType;
                if (!(*TagsValue)->TryGetStringField(TEXT("piste:type"), PisteType)
                    || PisteType.IsEmpty())
                {
                    OutError = TEXT("OVERPASS_RELATION_MISSING_PISTE_TYPE");
                    return false;
                }
                Kind = OsmVectorLayerKind::Trail;
            }
            else if (Route == TEXT("aerialway"))
            {
                Kind = OsmVectorLayerKind::Lift;
            }
            else
            {
                OutError = TEXT("OVERPASS_UNSUPPORTED_RELATION_TYPE");
                return false;
            }
        }

        uint32 NextPartIndex = 0;
        if (Type == TEXT("way"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (!Element->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes || Nodes->Num() < 2)
            {
                OutError = TEXT("OVERPASS_MISSING_WAY_NODES");
                return false;
            }
            if (static_cast<uint64>(Nodes->Num()) > MaximumRawPoints - RawNodeCount)
            {
                OutError = TEXT("OVERPASS_POINT_LIMIT");
                return false;
            }
            RawNodeCount += static_cast<uint64>(Nodes->Num());
            for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
            {
                if ((NodeIndex & 1023) == 0)
                {
                    if (CancellationValue.IsCancelled())
                    {
                        OutError = TEXT("OVERPASS_CANCELLED");
                        return false;
                    }
                    if (FPlatformTime::Seconds() >= Deadline)
                    {
                        OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
                        return false;
                    }
                }
                uint64 NodeId = 0;
                if (!ReadPositiveJsonId((*Nodes)[NodeIndex], NodeId))
                {
                    OutError = TEXT("OVERPASS_MALFORMED_NODE_ID");
                    return false;
                }
            }

            const TArray<TSharedPtr<FJsonValue>>* Geometry = nullptr;
            if (!Element->TryGetArrayField(TEXT("geometry"), Geometry) || !Geometry)
            {
                OutError = TEXT("OVERPASS_MISSING_WAY_GEOMETRY");
                return false;
            }
            if (Geometry->Num() != Nodes->Num())
            {
                OutError = TEXT("OVERPASS_INCOMPLETE_WAY_GEOMETRY");
                return false;
            }
            std::vector<OsmVectorPoint> RawPoints;
            if (!ParseGeometry(*Geometry, LocalFrame, Deadline, CancellationValue,
                RawGeometryPointCount, TEXT("OVERPASS_MISSING_WAY_GEOMETRY"),
                TEXT("OVERPASS_MALFORMED_WAY_GEOMETRY"), RawPoints, OutError))
                return false;
            std::vector<std::vector<OsmVectorPoint>> ClippedRuns;
            if (!ClipPolyline(RawPoints, Query.ExtentM, Deadline, CancellationValue,
                ClippedRuns, OutError))
                return false;
            if (!AppendClippedParts(ClippedRuns, OsmElementKind::Way, OsmId, NextPartIndex,
                Kind, OutputFeatureCount, OutputPointCount, OutResponse, OutError))
                return false;
        }
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
            if (!Element->TryGetArrayField(TEXT("members"), Members) || !Members || Members->Num() == 0)
            {
                OutError = TEXT("OVERPASS_RELATION_MISSING_MEMBERS");
                return false;
            }
            if (static_cast<uint64>(Members->Num()) > MaximumRawRelationMembers - RawRelationMemberCount)
            {
                OutError = TEXT("OVERPASS_RELATION_MEMBER_LIMIT");
                return false;
            }
            RawRelationMemberCount += static_cast<uint64>(Members->Num());
            uint64 UsableWayMemberCount = 0;
            for (int32 MemberIndex = 0; MemberIndex < Members->Num(); ++MemberIndex)
            {
                if (CancellationValue.IsCancelled())
                {
                    OutError = TEXT("OVERPASS_CANCELLED");
                    return false;
                }
                if (FPlatformTime::Seconds() >= Deadline)
                {
                    OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
                    return false;
                }
                const TSharedPtr<FJsonObject> Member = (*Members)[MemberIndex]
                    ? (*Members)[MemberIndex]->AsObject() : nullptr;
                FString MemberType;
                FString Role;
                double NumericMemberId = 0.0;
                uint64 MemberId = 0;
                if (!Member.IsValid() || !Member->TryGetStringField(TEXT("type"), MemberType)
                    || !Member->TryGetStringField(TEXT("role"), Role)
                    || !Member->TryGetNumberField(TEXT("ref"), NumericMemberId)
                    || !ReadPositiveJsonNumber(NumericMemberId, MemberId))
                {
                    OutError = TEXT("OVERPASS_MALFORMED_RELATION_MEMBER");
                    return false;
                }
                (void)MemberId;
                if (MemberType == TEXT("node"))
                {
                    // Known station/start markers do not carry line geometry in this model.
                    // Unknown node roles are rejected rather than silently discarding topology.
                    if (!IsSupportedRelationNodeRole(Kind, Role))
                    {
                        OutError = TEXT("OVERPASS_UNSUPPORTED_RELATION_MEMBER_ROLE");
                        return false;
                    }
                    continue;
                }
                if (MemberType != TEXT("way"))
                {
                    OutError = TEXT("OVERPASS_UNSUPPORTED_RELATION_MEMBER_TYPE");
                    return false;
                }
                ++UsableWayMemberCount;
                if (!IsSupportedRelationWayRole(Role))
                {
                    OutError = TEXT("OVERPASS_UNSUPPORTED_RELATION_MEMBER_ROLE");
                    return false;
                }
                const TArray<TSharedPtr<FJsonValue>>* Geometry = nullptr;
                if (!Member->TryGetArrayField(TEXT("geometry"), Geometry) || !Geometry)
                {
                    OutError = TEXT("OVERPASS_MISSING_RELATION_MEMBER_GEOMETRY");
                    return false;
                }
                std::vector<OsmVectorPoint> RawPoints;
                if (!ParseGeometry(*Geometry, LocalFrame, Deadline, CancellationValue,
                    RawGeometryPointCount, TEXT("OVERPASS_MISSING_RELATION_MEMBER_GEOMETRY"),
                    TEXT("OVERPASS_MALFORMED_RELATION_MEMBER_GEOMETRY"), RawPoints, OutError))
                    return false;
                if (Role == TEXT("backward")) std::reverse(RawPoints.begin(), RawPoints.end());
                std::vector<std::vector<OsmVectorPoint>> ClippedRuns;
                if (!ClipPolyline(RawPoints, Query.ExtentM, Deadline, CancellationValue,
                    ClippedRuns, OutError))
                    return false;
                if (!AppendClippedParts(ClippedRuns, OsmElementKind::Relation, OsmId,
                    NextPartIndex, Kind, OutputFeatureCount, OutputPointCount,
                    OutResponse, OutError))
                    return false;
            }
            if (UsableWayMemberCount == 0)
            {
                OutError = TEXT("OVERPASS_RELATION_HAS_NO_WAY_MEMBERS");
                return false;
            }
        }
    }

    for (OsmVectorLayer& Layer : OutResponse.Layers)
    {
        std::sort(Layer.Features.begin(), Layer.Features.end(), [](const OsmVectorFeature& A,
            const OsmVectorFeature& B)
        {
            if (A.ElementKind != B.ElementKind)
                return static_cast<uint8>(A.ElementKind) < static_cast<uint8>(B.ElementKind);
            if (A.OsmId != B.OsmId) return A.OsmId < B.OsmId;
            return A.PartIndex < B.PartIndex;
        });
    }
    return true;
}

bool ParseResponse(const HttpAcquisitionResult& Response,
    const OsmVectorProviderQuery& Query, const double Deadline,
    const Cancellation& CancellationValue, OsmVectorProviderResponse& OutResponse,
    FString& OutError)
{
    if (Response.Bytes.Num() <= 0 || static_cast<uint64>(Response.Bytes.Num()) > MaximumResponseBytes
        || Response.BytesReceived > MaximumResponseBytes)
    {
        OutError = TEXT("OVERPASS_RESPONSE_TOO_LARGE_OR_EMPTY");
        return false;
    }
    if (Response.ContentType.IsEmpty() || !Response.ContentType.Contains(TEXT("json"), ESearchCase::IgnoreCase))
    {
        OutError = TEXT("OVERPASS_UNEXPECTED_CONTENT_TYPE");
        return false;
    }
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("OVERPASS_CANCELLED");
        return false;
    }
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response.Bytes.GetData()),
        Response.Bytes.Num());
    const FString JsonText(Converted.Length(), Converted.Get());
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid())
    {
        OutError = TEXT("OVERPASS_INVALID_JSON");
        return false;
    }
    FString Remark;
    if (Root->TryGetStringField(TEXT("remark"), Remark) && !Remark.IsEmpty())
    {
        OutError = TEXT("OVERPASS_SERVER_REMARK:") + Remark.Left(160);
        return false;
    }
    const TSharedPtr<FJsonObject>* Osm3s = nullptr;
    if (!Root->TryGetObjectField(TEXT("osm3s"), Osm3s) || !Osm3s || !Osm3s->IsValid())
    {
        OutError = TEXT("OVERPASS_MISSING_OSM3S_METADATA");
        return false;
    }
    FString SourceTimestamp;
    if (!(*Osm3s)->TryGetStringField(TEXT("timestamp_osm_base"), SourceTimestamp)
        || !IsUtcTimestamp(SourceTimestamp))
    {
        OutError = TEXT("OVERPASS_INVALID_SOURCE_TIMESTAMP");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    if (!Root->TryGetArrayField(TEXT("elements"), Elements) || !Elements)
    {
        OutError = TEXT("OVERPASS_MISSING_ELEMENTS");
        return false;
    }
    if (FPlatformTime::Seconds() >= Deadline)
    {
        OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
        return false;
    }

    SkiDomain::LocalFrame LocalFrame;
    if (!SkiDomain::TryMakeLocalFrame(Query.LocalOrigin, LocalFrame))
    {
        OutError = TEXT("OVERPASS_INVALID_LOCAL_FRAME");
        return false;
    }
    OutResponse.Source = {};
    OutResponse.Source.SourceTimestampUtc = TCHAR_TO_UTF8(*SourceTimestamp);
    const FString Retrieved = RetrievedAtUtc();
    OutResponse.Source.RetrievedAtUtc = TCHAR_TO_UTF8(*Retrieved);
    if (!ParseElements(*Elements, Query, LocalFrame, Deadline,
        CancellationValue, OutResponse, OutError))
        return false;
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("OVERPASS_CANCELLED");
        return false;
    }
    if (FPlatformTime::Seconds() >= Deadline)
    {
        OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
        return false;
    }
    return true;
}
}

SkiPreparation::OverpassVectorProvider::OverpassVectorProvider(
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport)
    : Transport(MoveTemp(InTransport))
{
    if (!Transport.IsValid()) Transport = MakeShared<SkiNetGateway, ESPMode::ThreadSafe>();
}

bool SkiPreparation::OverpassVectorProvider::Acquire(const OsmVectorProviderQuery& Query,
    const Cancellation& CancellationValue, OsmVectorProviderResponse& OutResponse,
    FString& OutError)
{
    OutResponse = {};
    OutError.Reset();
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("OVERPASS_CANCELLED");
        return false;
    }
    if (!Transport.IsValid())
    {
        OutError = TEXT("OVERPASS_TRANSPORT_UNAVAILABLE");
        return false;
    }
    if (!ValidateQuery(Query, OutError)) return false;

    FGeographicQueryBounds Bounds;
    if (!MakeGeographicBounds(Query, Bounds))
    {
        OutError = TEXT("OVERPASS_INVALID_GEOGRAPHIC_BOUNDS");
        return false;
    }
    const FString QueryText = BuildOverpassQuery(Bounds);
    const FString Url = FString(OverpassEndpoint) + TEXT("?data=") + EncodeQueryParameter(QueryText);
    if (Url.Len() > MaximumQueryUrlCharacters)
    {
        OutError = TEXT("OVERPASS_QUERY_TOO_LARGE");
        return false;
    }
    FString UrlReason;
    if (!SkiNetGateway::ValidateUrl(Url, UrlReason))
    {
        OutError = TEXT("OVERPASS_GATEWAY_URL_REJECTED:") + UrlReason;
        return false;
    }

    const double Began = FPlatformTime::Seconds();
    const double Deadline = Began + OperationDeadlineSeconds;
    HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.Product = ProviderProduct::VectorContext;
    Request.Attempt = 1;
    Request.ActivityTimeoutSeconds = 15.0F;
    Request.TotalTimeoutSeconds = 45.0F;
    Request.AbsoluteOperationDeadlineSeconds = Deadline;
    Request.MaximumResponseBytes = MaximumResponseBytes;

    // IOsmVectorProvider takes a cancellation reference while the transport contract takes
    // a shared token. Run the transport on a worker and relay cancellation/deadline promptly.
    const TSharedRef<Cancellation> TransportCancellation = MakeShared<Cancellation>();
    const TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> TransportCopy = Transport;
    TFuture<HttpAcquisitionResult> Pending = Async(EAsyncExecution::ThreadPool,
        [TransportCopy, Request, TransportCancellation]() mutable
        {
            return TransportCopy->Get(Request, TransportCancellation);
        });
    bool bDeadlineExpired = false;
    while (!Pending.IsReady())
    {
        if (CancellationValue.IsCancelled()) TransportCancellation->Cancel();
        if (FPlatformTime::Seconds() >= Deadline)
        {
            bDeadlineExpired = true;
            TransportCancellation->Cancel();
        }
        FPlatformProcess::SleepNoStats(0.005F);
    }
    const HttpAcquisitionResult Response = Pending.Get();
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("OVERPASS_CANCELLED");
        return false;
    }
    if (bDeadlineExpired || FPlatformTime::Seconds() >= Deadline)
    {
        OutError = TEXT("OVERPASS_OPERATION_DEADLINE");
        return false;
    }
    if (Response.FailureReason == TransportFailureReason::ResponseTooLarge
        || Response.Bytes.Num() > static_cast<int32>(MaximumResponseBytes)
        || Response.BytesReceived > MaximumResponseBytes)
    {
        OutError = TEXT("OVERPASS_RESPONSE_TOO_LARGE");
        return false;
    }
    if (!Response.Ok())
    {
        OutError = FString::Printf(TEXT("OVERPASS_REQUEST_FAILED:%s:%d"),
            *Response.RequestStatus.Left(96), Response.HttpStatus);
        return false;
    }
    if (!ParseResponse(Response, Query, Deadline, CancellationValue, OutResponse, OutError))
    {
        OutResponse = {};
        return false;
    }
    return true;
}
