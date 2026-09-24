#include "SkiPreparation/SiteContextVectorReader.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

namespace
{
bool Fail(FString& OutError, const TCHAR* Message)
{
    OutError = Message;
    return false;
}

std::string ToUtf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), static_cast<std::size_t>(Converted.Length()));
}

bool IsValidMetadata(const FString& Value)
{
    if (Value.IsEmpty()) return false;
    const std::string Encoded = ToUtf8(Value);
    return Encoded.size() <= 128U
        && std::none_of(Encoded.begin(), Encoded.end(), [](const unsigned char Character)
        {
            return Character < 0x20U || Character == 0x7fU;
        });
}

bool ReadUint32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
    std::uint32_t& OutValue)
{
    double Value = 0.0;
    if (!Object || !Object->TryGetNumberField(Name, Value) || !std::isfinite(Value)
        || Value < 0.0 || Value > static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        || std::floor(Value) != Value)
    {
        return false;
    }
    OutValue = static_cast<std::uint32_t>(Value);
    return true;
}

bool IsNamespacedOsmId(const FString& Value)
{
    FString NumericId;
    if (Value.StartsWith(TEXT("way/"), ESearchCase::CaseSensitive))
        NumericId = Value.Mid(4);
    else if (Value.StartsWith(TEXT("relation/"), ESearchCase::CaseSensitive))
        NumericId = Value.Mid(9);
    else
        return false;

    if (NumericId.IsEmpty() || NumericId.Len() > 20
        || (NumericId.Len() > 1 && NumericId[0] == TEXT('0')))
    {
        return false;
    }
    for (const TCHAR Character : NumericId)
    {
        if (Character < TEXT('0') || Character > TEXT('9')) return false;
    }

    const std::string Digits = ToUtf8(NumericId);
    std::uint64_t ParsedId = 0;
    const auto Parsed = std::from_chars(Digits.data(), Digits.data() + Digits.size(), ParsedId);
    return Parsed.ec == std::errc{} && Parsed.ptr == Digits.data() + Digits.size()
        && ParsedId > 0U;
}

bool ParseKind(const FString& Value, SkiPreparation::ESiteContextVectorKind& OutKind)
{
    if (Value == TEXT("road")) OutKind = SkiPreparation::ESiteContextVectorKind::Road;
    else if (Value == TEXT("lift")) OutKind = SkiPreparation::ESiteContextVectorKind::Lift;
    else if (Value == TEXT("trail")) OutKind = SkiPreparation::ESiteContextVectorKind::Trail;
    else return false;
    return true;
}

bool ReadPoint(const TSharedPtr<FJsonValue>& Value,
    const SkiDomain::MetricBounds& Extent,
    SkiPreparation::FSiteContextVectorPoint& OutPoint)
{
    const TSharedPtr<FJsonObject> Object = Value ? Value->AsObject() : nullptr;
    double EastM = 0.0;
    double NorthM = 0.0;
    if (!Object || !Object->TryGetNumberField(TEXT("eastM"), EastM)
        || !Object->TryGetNumberField(TEXT("northM"), NorthM)
        || !std::isfinite(EastM) || !std::isfinite(NorthM)
        || std::abs(EastM) > SkiPreparation::SiteContextVectorAssetMaxCoordinateMagnitudeM
        || std::abs(NorthM) > SkiPreparation::SiteContextVectorAssetMaxCoordinateMagnitudeM
        || EastM < Extent.WestM || EastM > Extent.EastM
        || NorthM < Extent.SouthM || NorthM > Extent.NorthM)
    {
        return false;
    }
    OutPoint = {EastM, NorthM};
    return true;
}

bool ValidBounds(const SkiDomain::MetricBounds& Bounds)
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM <= Bounds.EastM && Bounds.SouthM <= Bounds.NorthM;
}
}

bool SkiPreparation::ParseSiteContextVectorAsset(const TArray<uint8>& Utf8JsonBytes,
    const SiteContextManifest& Manifest, const SkiDomain::TerrainCoreManifest& TerrainCore,
    FSiteContextVectorAsset& OutAsset, FString& OutError)
{
    OutAsset = {};
    OutError.Reset();

    if (!ValidBounds(TerrainCore.OuterBounds))
        return Fail(OutError, TEXT("SiteContext vector TerrainCore bounds are invalid."));
    if (Utf8JsonBytes.IsEmpty()
        || static_cast<std::uint64_t>(Utf8JsonBytes.Num()) > SiteContextMaxAssetBytes
        || Utf8JsonBytes.Contains(0))
    {
        return Fail(OutError, TEXT("SiteContext OSM vector JSON is empty, oversized, or contains NUL bytes."));
    }
    if (Manifest.VectorEncoding != SiteContextVectorEncodingV1
        && Manifest.VectorEncoding != SiteContextVectorEncodingV2)
    {
        return Fail(OutError, TEXT("SiteContext vector encoding is unsupported."));
    }

    const std::string JsonUtf8(reinterpret_cast<const char*>(Utf8JsonBytes.GetData()),
        static_cast<std::size_t>(Utf8JsonBytes.Num()));
    const FString Json = UTF8_TO_TCHAR(JsonUtf8.c_str());
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        return Fail(OutError, TEXT("SiteContext OSM vector JSON is malformed."));

    std::uint32_t SchemaVersion = 0;
    const TArray<TSharedPtr<FJsonValue>>* Features = nullptr;
    const bool bEncodingV1 = Manifest.VectorEncoding == SiteContextVectorEncodingV1;
    const bool bEncodingV2 = Manifest.VectorEncoding == SiteContextVectorEncodingV2;
    if (!ReadUint32(Root, TEXT("schemaVersion"), SchemaVersion)
        || (SchemaVersion != 1U && SchemaVersion != 2U)
        || (SchemaVersion == 1U && !bEncodingV1)
        || (SchemaVersion == 2U && !bEncodingV2)
        || !Root->TryGetArrayField(TEXT("features"), Features) || !Features
        || static_cast<std::uint32_t>(Features->Num()) > SiteContextVectorAssetMaxFeatures)
    {
        return Fail(OutError, TEXT("SiteContext OSM vector schema or feature count is invalid."));
    }

    FSiteContextVectorAsset Candidate;
    Candidate.SchemaVersion = SchemaVersion;
    Candidate.Polylines.Reserve(Features->Num());
    std::uint64_t TotalPoints = 0;
    std::unordered_set<std::string> LegacyFeatureIdentities;
    std::string PreviousKind;
    std::string PreviousOsmId;
    std::uint32_t PreviousPartIndex = 0;
    bool bHasPreviousV2Feature = false;

    for (int32 FeatureIndex = 0; FeatureIndex < Features->Num(); ++FeatureIndex)
    {
        const TSharedPtr<FJsonObject> Feature = (*Features)[FeatureIndex]
            ? (*Features)[FeatureIndex]->AsObject() : nullptr;
        FString OsmId;
        FString KindName;
        const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
        std::uint32_t PartIndex = 0;
        if (!Feature || !Feature->TryGetStringField(TEXT("osmId"), OsmId)
            || !Feature->TryGetStringField(TEXT("kind"), KindName)
            || !Feature->TryGetArrayField(TEXT("points"), Points) || !Points
            || Points->Num() < 2
            || static_cast<std::uint32_t>(Points->Num())
                > SiteContextVectorAssetMaxPointsPerPolyline
            || !IsValidMetadata(OsmId))
        {
            return Fail(OutError, TEXT("SiteContext OSM vector feature metadata or polyline is invalid."));
        }

        FSiteContextVectorPolyline Polyline;
        Polyline.OsmId = OsmId;
        if (!ParseKind(KindName, Polyline.Kind))
            return Fail(OutError, TEXT("SiteContext OSM vector feature kind is unsupported."));

        const std::string KindKey = ToUtf8(KindName);
        const std::string OsmIdKey = ToUtf8(OsmId);
        if (SchemaVersion == 1U)
        {
            if (Feature->HasField(TEXT("partIndex")))
                return Fail(OutError, TEXT("Schema-1 OSM vectors cannot carry a part index."));
            std::string Identity = KindKey;
            Identity.push_back('\0');
            Identity += OsmIdKey;
            if (!LegacyFeatureIdentities.insert(std::move(Identity)).second)
                return Fail(OutError, TEXT("Schema-1 OSM vectors contain a duplicate element part."));
        }
        else
        {
            if (!IsNamespacedOsmId(OsmId)
                || !ReadUint32(Feature, TEXT("partIndex"), PartIndex))
            {
                return Fail(OutError, TEXT("Schema-2 OSM vectors require a namespaced positive OSM ID and part index."));
            }
            if (bHasPreviousV2Feature)
            {
                if (KindKey < PreviousKind
                    || (KindKey == PreviousKind && OsmIdKey < PreviousOsmId))
                {
                    return Fail(OutError, TEXT("Schema-2 OSM vector parts are not sorted by kind and element ID."));
                }
                if (KindKey == PreviousKind && OsmIdKey == PreviousOsmId)
                {
                    if (PreviousPartIndex == std::numeric_limits<std::uint32_t>::max()
                        || PartIndex != PreviousPartIndex + 1U)
                    {
                        return Fail(OutError, TEXT("Schema-2 OSM part indices are duplicate or non-contiguous."));
                    }
                }
                else if (PartIndex != 0U)
                {
                    return Fail(OutError, TEXT("Schema-2 OSM parts must start at index zero for each element."));
                }
            }
            else if (PartIndex != 0U)
            {
                return Fail(OutError, TEXT("Schema-2 OSM parts must start at index zero for each element."));
            }
            PreviousKind = KindKey;
            PreviousOsmId = OsmIdKey;
            PreviousPartIndex = PartIndex;
            bHasPreviousV2Feature = true;
        }

        const std::uint64_t FeaturePointCount = static_cast<std::uint64_t>(Points->Num());
        if (TotalPoints > SiteContextVectorAssetMaxPoints - FeaturePointCount)
            return Fail(OutError, TEXT("SiteContext OSM vector point count exceeds its limit."));
        TotalPoints += FeaturePointCount;
        Polyline.PartIndex = PartIndex;
        Polyline.Points.Reserve(Points->Num());
        for (const TSharedPtr<FJsonValue>& PointValue : *Points)
        {
            FSiteContextVectorPoint Point;
            if (!ReadPoint(PointValue, TerrainCore.OuterBounds, Point))
                return Fail(OutError, TEXT("SiteContext OSM vector point is invalid or outside TerrainCore bounds."));
            Polyline.Points.Add(Point);
        }
        Candidate.Polylines.Add(MoveTemp(Polyline));
    }

    Candidate.PointCount = TotalPoints;
    OutAsset = MoveTemp(Candidate);
    return true;
}
