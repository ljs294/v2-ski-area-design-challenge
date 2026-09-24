#include "SkiPreparation/OsmVectorSiteContextConverter.h"

#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

namespace
{
using namespace SkiPreparation;

struct FOrderedFeature
{
    const OsmVectorFeature* Feature = nullptr;
    std::string Kind;
    std::string NamespacedOsmId;
};

bool AppendAscii(FString& Json, const TCHAR* Value, FString& OutError)
{
    const int32 Length = FCString::Strlen(Value);
    if (Length < 0 || static_cast<std::uint64_t>(Json.Len())
        + static_cast<std::uint64_t>(Length) > SiteContextMaxAssetBytes)
    {
        OutError = TEXT("Converted SiteContext vector JSON exceeds its asset byte limit.");
        return false;
    }
    Json.Append(Value, Length);
    return true;
}

bool AppendAscii(FString& Json, const std::string& Value, FString& OutError)
{
    for (const unsigned char Character : Value)
    {
        if (Character < 0x20U || Character > 0x7eU)
        {
            OutError = TEXT("Converted SiteContext vector identity is not canonical ASCII.");
            return false;
        }
    }
    const FString Converted = UTF8_TO_TCHAR(Value.c_str());
    return AppendAscii(Json, *Converted, OutError);
}

template <typename TNumber>
bool AppendNumber(FString& Json, const TNumber Value, FString& OutError)
{
    char Buffer[64] = {};
    const auto Converted = std::to_chars(Buffer, Buffer + sizeof(Buffer) - 1, Value);
    if (Converted.ec != std::errc{})
    {
        OutError = TEXT("Unable to encode a canonical SiteContext vector number.");
        return false;
    }
    *Converted.ptr = '\0';
    return AppendAscii(Json, ANSI_TO_TCHAR(Buffer), OutError);
}

bool AppendNumber(FString& Json, const double Value, FString& OutError)
{
    char Buffer[64] = {};
    const double Canonical = Value == 0.0 ? 0.0 : Value;
    const auto Converted = std::to_chars(Buffer, Buffer + sizeof(Buffer) - 1,
        Canonical, std::chars_format::general);
    if (Converted.ec != std::errc{})
    {
        OutError = TEXT("Unable to encode a canonical SiteContext vector coordinate.");
        return false;
    }
    *Converted.ptr = '\0';
    return AppendAscii(Json, ANSI_TO_TCHAR(Buffer), OutError);
}

bool AppendFeature(FString& Json, const FOrderedFeature& Ordered, FString& OutError,
    const Cancellation& CancellationValue, std::uint64_t& VisitedPoints)
{
    const OsmVectorFeature& Feature = *Ordered.Feature;
    const TCHAR* Separator = Json.EndsWith(TEXT("[")) ? TEXT("") : TEXT(",");
    if (!AppendAscii(Json, Separator, OutError)
        || !AppendAscii(Json, TEXT("{\"osmId\":\""), OutError)
        || !AppendAscii(Json, Ordered.NamespacedOsmId, OutError)
        || !AppendAscii(Json, TEXT("\",\"kind\":\""), OutError)
        || !AppendAscii(Json, Ordered.Kind, OutError)
        || !AppendAscii(Json, TEXT("\",\"partIndex\":"), OutError)
        || !AppendNumber(Json, Feature.PartIndex, OutError)
        || !AppendAscii(Json, TEXT(",\"points\":["), OutError))
    {
        return false;
    }

    for (std::size_t PointIndex = 0; PointIndex < Feature.Points.size(); ++PointIndex)
    {
        if ((VisitedPoints++ & 255ULL) == 0ULL && CancellationValue.IsCancelled())
        {
            OutError = TEXT("SiteContext vector conversion was cancelled.");
            return false;
        }
        if (!AppendAscii(Json, PointIndex == 0U ? TEXT("") : TEXT(","), OutError)
            || !AppendAscii(Json, TEXT("{\"eastM\":"), OutError)
            || !AppendNumber(Json, Feature.Points[PointIndex].EastM, OutError)
            || !AppendAscii(Json, TEXT(",\"northM\":"), OutError)
            || !AppendNumber(Json, Feature.Points[PointIndex].NorthM, OutError)
            || !AppendAscii(Json, TEXT("}"), OutError))
        {
            return false;
        }
    }
    return AppendAscii(Json, TEXT("]}"), OutError);
}
}

bool SkiPreparation::ConvertVerifiedOsmVectorPackageToSiteContextAsset(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation& CancellationValue, OsmVectorSiteContextAsset& OutAsset,
    FString& OutError)
{
    OutAsset = {};
    OutError.Reset();

    OsmVectorPackageVerificationReport Verification;
    if (!VerifyOsmVectorPackage(TerrainCore, Package, CancellationValue, Verification))
    {
        OutError = Verification.FailureDetail.IsEmpty()
            ? TEXT("OSM vector package failed verification.") : Verification.FailureDetail;
        return false;
    }
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("SiteContext vector conversion was cancelled.");
        return false;
    }

    std::vector<FOrderedFeature> OrderedFeatures;
    OrderedFeatures.reserve(static_cast<std::size_t>(Verification.FeatureCount));
    for (const OsmVectorLayer& Layer : Package.Layers)
    {
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("SiteContext vector conversion was cancelled.");
            return false;
        }
        const char* LayerName = OsmVectorLayerName(Layer.Kind);
        if (LayerName == nullptr || std::string(LayerName) == "invalid")
        {
            OutError = TEXT("OSM vector package contains an unsupported layer kind.");
            return false;
        }
        for (const OsmVectorFeature& Feature : Layer.Features)
        {
            if (CancellationValue.IsCancelled())
            {
                OutError = TEXT("SiteContext vector conversion was cancelled.");
                return false;
            }
            const char* ElementPrefix = Feature.ElementKind == OsmElementKind::Way
                ? "way/" : Feature.ElementKind == OsmElementKind::Relation ? "relation/" : nullptr;
            if (ElementPrefix == nullptr)
            {
                OutError = TEXT("OSM vector package contains an unsupported element kind.");
                return false;
            }
            FOrderedFeature& Ordered = OrderedFeatures.emplace_back();
            Ordered.Feature = &Feature;
            Ordered.Kind = LayerName;
            Ordered.NamespacedOsmId = std::string(ElementPrefix)
                + std::to_string(Feature.OsmId);
        }
    }
    if (OrderedFeatures.size() != Verification.FeatureCount)
    {
        OutError = TEXT("Verified OSM vector feature count changed during conversion.");
        return false;
    }

    std::sort(OrderedFeatures.begin(), OrderedFeatures.end(),
        [](const FOrderedFeature& A, const FOrderedFeature& B)
        {
            return std::tie(A.Kind, A.NamespacedOsmId, A.Feature->PartIndex)
                < std::tie(B.Kind, B.NamespacedOsmId, B.Feature->PartIndex);
        });

    FString Json;
    const std::uint64_t ReserveCharacters = std::min<std::uint64_t>(
        SiteContextMaxAssetBytes,
        Verification.Storage.MinimumBytes + Verification.FeatureCount * 24ULL);
    Json.Reserve(static_cast<int32>(ReserveCharacters));
    if (!AppendAscii(Json, TEXT("{\"schemaVersion\":2,\"features\":["), OutError))
        return false;

    std::uint64_t VisitedPoints = 0;
    for (const FOrderedFeature& Feature : OrderedFeatures)
    {
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("SiteContext vector conversion was cancelled.");
            return false;
        }
        if (!AppendFeature(Json, Feature, OutError, CancellationValue, VisitedPoints))
            return false;
    }
    if (!AppendAscii(Json, TEXT("]}"), OutError) || CancellationValue.IsCancelled())
    {
        if (OutError.IsEmpty()) OutError = TEXT("SiteContext vector conversion was cancelled.");
        return false;
    }

    FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0
        || static_cast<std::uint64_t>(Encoded.Length()) > SiteContextMaxAssetBytes)
    {
        OutError = TEXT("Converted SiteContext vector JSON exceeds its UTF-8 byte limit.");
        return false;
    }
    if (CancellationValue.IsCancelled())
    {
        OutError = TEXT("SiteContext vector conversion was cancelled.");
        return false;
    }

    OsmVectorSiteContextAsset Converted;
    Converted.VectorJsonUtf8.Append(
        reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
    Converted.SourcePackageContentId = Package.ContentId;
    Converted.TerrainCoreId = Package.TerrainCoreId;
    Converted.ExtentM = Package.ExtentM;
    Converted.Source = Package.Source;
    OutAsset = MoveTemp(Converted);
    return true;
}
