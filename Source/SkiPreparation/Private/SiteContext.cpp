#include "SkiPreparation/SiteContext.h"

#include "Algo/Reverse.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace
{
using namespace SkiPreparation;

bool IsCanonicalSha256(const std::string& Value)
{
    return SkiDomain::IsTerrainCoreSha256(Value);
}

std::string Utf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), Converted.Length());
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

bool ReadMetricBounds(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::MetricBounds& Out)
{
    return ReadFiniteNumber(Object, TEXT("westM"), Out.WestM)
        && ReadFiniteNumber(Object, TEXT("southM"), Out.SouthM)
        && ReadFiniteNumber(Object, TEXT("eastM"), Out.EastM)
        && ReadFiniteNumber(Object, TEXT("northM"), Out.NorthM);
}

double CanonicalDouble(const double Value) noexcept
{
    return Value == 0.0 ? 0.0 : Value;
}

bool SameMetricBounds(const SkiDomain::MetricBounds& A,
    const SkiDomain::MetricBounds& B) noexcept
{
    return CanonicalDouble(A.WestM) == CanonicalDouble(B.WestM)
        && CanonicalDouble(A.SouthM) == CanonicalDouble(B.SouthM)
        && CanonicalDouble(A.EastM) == CanonicalDouble(B.EastM)
        && CanonicalDouble(A.NorthM) == CanonicalDouble(B.NorthM);
}

bool FiniteMetricBounds(const SkiDomain::MetricBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM < Bounds.EastM && Bounds.SouthM < Bounds.NorthM;
}

bool IsCanonicalUtcTimestamp(const std::string& Value) noexcept
{
    // YYYY-MM-DDTHH:MM:SS[.fraction]Z, with an actual calendar date and UTC only.
    if (Value.size() < 20U || Value.size() > 30U
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
    const bool LeapYear = (Year % 4U == 0U && Year % 100U != 0U) || Year % 400U == 0U;
    const unsigned MaximumDay = Month == 2U && LeapYear ? 29U : DaysByMonth[Month - 1U];
    if (Day < 1U || Day > MaximumDay) return false;
    if (Value.size() == 20U) return true;
    if (Value[19] != '.') return false;
    const std::size_t FractionDigits = Value.size() - 21U;
    return FractionDigits >= 1U && FractionDigits <= 9U && Digits(20, FractionDigits);
}

bool HasAnyVectorSourceLineage(const SiteContextVectorSourceLineage& Lineage) noexcept
{
    return !Lineage.SourcePackageContentId.empty() || !Lineage.TerrainCoreId.empty()
        || Lineage.ExtentM.WestM != 0.0 || Lineage.ExtentM.SouthM != 0.0
        || Lineage.ExtentM.EastM != 0.0 || Lineage.ExtentM.NorthM != 0.0
        || !Lineage.Provider.empty() || !Lineage.Endpoint.empty()
        || !Lineage.SourceTimestampUtc.empty() || !Lineage.RetrievedAtUtc.empty()
        || !Lineage.License.empty() || !Lineage.Attribution.empty()
        || !Lineage.AttributionUrl.empty();
}

TSharedRef<FJsonObject> VectorSourceLineageObject(
    const SiteContextVectorSourceLineage& Lineage)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("sourcePackageContentId"),
        UTF8_TO_TCHAR(Lineage.SourcePackageContentId.c_str()));
    Object->SetStringField(TEXT("terrainCoreId"), UTF8_TO_TCHAR(Lineage.TerrainCoreId.c_str()));
    TSharedRef<FJsonObject> Extent = MakeShared<FJsonObject>();
    Extent->SetNumberField(TEXT("westM"), CanonicalDouble(Lineage.ExtentM.WestM));
    Extent->SetNumberField(TEXT("southM"), CanonicalDouble(Lineage.ExtentM.SouthM));
    Extent->SetNumberField(TEXT("eastM"), CanonicalDouble(Lineage.ExtentM.EastM));
    Extent->SetNumberField(TEXT("northM"), CanonicalDouble(Lineage.ExtentM.NorthM));
    Object->SetObjectField(TEXT("extentM"), Extent);
    Object->SetStringField(TEXT("provider"), UTF8_TO_TCHAR(Lineage.Provider.c_str()));
    Object->SetStringField(TEXT("endpoint"), UTF8_TO_TCHAR(Lineage.Endpoint.c_str()));
    Object->SetStringField(TEXT("sourceTimestampUtc"),
        UTF8_TO_TCHAR(Lineage.SourceTimestampUtc.c_str()));
    Object->SetStringField(TEXT("retrievedAtUtc"),
        UTF8_TO_TCHAR(Lineage.RetrievedAtUtc.c_str()));
    Object->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Lineage.License.c_str()));
    Object->SetStringField(TEXT("attribution"), UTF8_TO_TCHAR(Lineage.Attribution.c_str()));
    Object->SetStringField(TEXT("attributionUrl"), UTF8_TO_TCHAR(Lineage.AttributionUrl.c_str()));
    return Object;
}

bool ReadUint8(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, std::uint8_t& Out)
{
    std::uint32_t Value = 0;
    if (!ReadUint32(Object, Name, Value) || Value > MAX_uint8) return false;
    Out = static_cast<std::uint8_t>(Value);
    return true;
}

bool ReadUint64String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
    std::uint64_t& Out)
{
    FString Value;
    return Object && Object->TryGetStringField(Name, Value) && !Value.IsEmpty()
        && LexTryParseString(Out, *Value);
}

bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& Out, FString& Error)
{
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Out) || !Out)
    {
        Error = TEXT("SiteContext metadata is not valid JSON.");
        return false;
    }
    return true;
}

void SerializeJsonObject(const TSharedRef<FJsonObject>& Object, FString& Out)
{
    Out.Reset();
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Out, 0));
}

TSharedRef<FJsonObject> SiteContextManifestObject(const SiteContextManifest& Manifest,
    const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
    if (IncludeContentId)
        Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Manifest.GeneratorVersion.c_str()));
    Root->SetStringField(TEXT("terrainCoreId"), UTF8_TO_TCHAR(Manifest.TerrainCoreId.c_str()));
    Root->SetNumberField(TEXT("imageryTilePixels"), Manifest.ImageryTilePixels);
    Root->SetStringField(TEXT("imageryEncoding"), UTF8_TO_TCHAR(Manifest.ImageryEncoding.c_str()));
    Root->SetStringField(TEXT("imageryFrame"), UTF8_TO_TCHAR(Manifest.ImageryFrame.c_str()));
    Root->SetStringField(TEXT("vectorEncoding"), UTF8_TO_TCHAR(Manifest.VectorEncoding.c_str()));
    Root->SetStringField(TEXT("vectorAssetPath"), UTF8_TO_TCHAR(Manifest.VectorAssetPath.c_str()));
    if (Manifest.SchemaVersion == SiteContextSchema)
        Root->SetObjectField(TEXT("vectorSource"), VectorSourceLineageObject(Manifest.VectorSource));

    TArray<TSharedPtr<FJsonValue>> Tiles;
    Tiles.Reserve(static_cast<int32>(Manifest.ImageryTiles.size()));
    for (const SiteContextImageryTile& Tile : Manifest.ImageryTiles)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("lodIndex"), Tile.LodIndex);
        Object->SetNumberField(TEXT("lodFactor"), Tile.LodFactor);
        Object->SetNumberField(TEXT("tileX"), Tile.TileX);
        Object->SetNumberField(TEXT("tileY"), Tile.TileY);
        Object->SetNumberField(TEXT("pixelWidth"), Tile.PixelWidth);
        Object->SetNumberField(TEXT("pixelHeight"), Tile.PixelHeight);
        Object->SetNumberField(TEXT("eastMetersPerPixel"), Tile.EastMetersPerPixel);
        Object->SetNumberField(TEXT("northMetersPerPixel"), Tile.NorthMetersPerPixel);
        Object->SetStringField(TEXT("assetPath"), UTF8_TO_TCHAR(Tile.AssetPath.c_str()));
        Tiles.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("imageryTiles"), Tiles);

    TArray<TSharedPtr<FJsonValue>> Assets;
    Assets.Reserve(static_cast<int32>(Manifest.Assets.size()));
    for (const SiteContextAsset& Asset : Manifest.Assets)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("path"), UTF8_TO_TCHAR(Asset.Path.c_str()));
        Object->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Asset.Type.c_str()));
        Object->SetStringField(TEXT("sha256"), UTF8_TO_TCHAR(Asset.Sha256.c_str()));
        Object->SetStringField(TEXT("length"), LexToString(Asset.Length));
        Assets.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("assets"), Assets);

    TArray<TSharedPtr<FJsonValue>> Attributions;
    Attributions.Reserve(static_cast<int32>(Manifest.Attributions.size()));
    for (const SiteContextAttribution& Attribution : Manifest.Attributions)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("provider"), UTF8_TO_TCHAR(Attribution.Provider.c_str()));
        Object->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Attribution.License.c_str()));
        Object->SetStringField(TEXT("text"), UTF8_TO_TCHAR(Attribution.Text.c_str()));
        Attributions.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("attributions"), Attributions);
    return Root;
}

TSharedRef<FJsonObject> ProvenanceCountsObject(const SkiDomain::TerrainProvenanceCounts& Counts)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("s1mNativeQualified"), LexToString(Counts.S1MNativeQualified));
    Object->SetStringField(TEXT("s1mNativeOther"), LexToString(Counts.S1MNativeOther));
    Object->SetStringField(TEXT("s1mBlend"), LexToString(Counts.S1MBlend));
    Object->SetStringField(TEXT("s1mBackfill"), LexToString(Counts.S1MBackfill));
    Object->SetStringField(TEXT("s1mInterpolated"), LexToString(Counts.S1MInterpolated));
    Object->SetStringField(TEXT("project1mQualified"), LexToString(Counts.Project1mQualified));
    Object->SetStringField(TEXT("project1mOther"), LexToString(Counts.Project1mOther));
    Object->SetStringField(TEXT("arcSec13"), LexToString(Counts.ArcSec13));
    Object->SetStringField(TEXT("noData"), LexToString(Counts.NoData));
    Object->SetStringField(TEXT("unknownMetadata"), LexToString(Counts.UnknownMetadata));
    return Object;
}

const TCHAR* GradeName(const SkiDomain::TerrainGrade Grade)
{
    switch (Grade)
    {
    case SkiDomain::TerrainGrade::A: return TEXT("A");
    case SkiDomain::TerrainGrade::B: return TEXT("B");
    case SkiDomain::TerrainGrade::C: return TEXT("C");
    case SkiDomain::TerrainGrade::D: return TEXT("D");
    default: return TEXT("invalid");
    }
}

bool ParseGrade(const FString& Value, SkiDomain::TerrainGrade& Out)
{
    if (Value == TEXT("A")) Out = SkiDomain::TerrainGrade::A;
    else if (Value == TEXT("B")) Out = SkiDomain::TerrainGrade::B;
    else if (Value == TEXT("C")) Out = SkiDomain::TerrainGrade::C;
    else if (Value == TEXT("D")) Out = SkiDomain::TerrainGrade::D;
    else return false;
    return true;
}

const TCHAR* ElevationProductName(const SkiDomain::ElevationProduct Product)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return TEXT("S1M");
    case SkiDomain::ElevationProduct::Project1m: return TEXT("Project1m");
    case SkiDomain::ElevationProduct::ArcSec13: return TEXT("ArcSec13");
    default: return TEXT("invalid");
    }
}

bool ParseElevationProduct(const FString& Value, SkiDomain::ElevationProduct& Out)
{
    if (Value == TEXT("S1M")) Out = SkiDomain::ElevationProduct::S1M;
    else if (Value == TEXT("Project1m")) Out = SkiDomain::ElevationProduct::Project1m;
    else if (Value == TEXT("ArcSec13")) Out = SkiDomain::ElevationProduct::ArcSec13;
    else return false;
    return true;
}

const TCHAR* VerticalDatumName(const SkiDomain::TerrainVerticalDatum Datum)
{
    switch (Datum)
    {
    case SkiDomain::TerrainVerticalDatum::Unknown: return TEXT("unknown");
    case SkiDomain::TerrainVerticalDatum::NAVD88: return TEXT("NAVD88");
    case SkiDomain::TerrainVerticalDatum::Other: return TEXT("other");
    default: return TEXT("invalid");
    }
}

bool ParseVerticalDatum(const FString& Value, SkiDomain::TerrainVerticalDatum& Out)
{
    if (Value == TEXT("unknown")) Out = SkiDomain::TerrainVerticalDatum::Unknown;
    else if (Value == TEXT("NAVD88")) Out = SkiDomain::TerrainVerticalDatum::NAVD88;
    else if (Value == TEXT("other")) Out = SkiDomain::TerrainVerticalDatum::Other;
    else return false;
    return true;
}

const TCHAR* DatumProofOriginName(const SkiDomain::TerrainDatumProofOrigin Origin)
{
    using SkiDomain::TerrainDatumProofOrigin;
    switch (Origin)
    {
    case TerrainDatumProofOrigin::Unknown: return TEXT("unknown");
    case TerrainDatumProofOrigin::CogMetadata: return TEXT("cog-metadata");
    case TerrainDatumProofOrigin::GeoPackageSourceInputs: return TEXT("geopackage-source-inputs");
    case TerrainDatumProofOrigin::XmlSidecar: return TEXT("xml-sidecar");
    case TerrainDatumProofOrigin::CatalogMetadata: return TEXT("catalog-metadata");
    case TerrainDatumProofOrigin::VerifiedS1mLineage: return TEXT("verified-s1m-lineage");
    default: return TEXT("invalid");
    }
}

bool ParseDatumProofOrigin(const FString& Value, SkiDomain::TerrainDatumProofOrigin& Out)
{
    using SkiDomain::TerrainDatumProofOrigin;
    if (Value == TEXT("unknown")) Out = TerrainDatumProofOrigin::Unknown;
    else if (Value == TEXT("cog-metadata")) Out = TerrainDatumProofOrigin::CogMetadata;
    else if (Value == TEXT("geopackage-source-inputs"))
        Out = TerrainDatumProofOrigin::GeoPackageSourceInputs;
    else if (Value == TEXT("xml-sidecar")) Out = TerrainDatumProofOrigin::XmlSidecar;
    else if (Value == TEXT("catalog-metadata")) Out = TerrainDatumProofOrigin::CatalogMetadata;
    else if (Value == TEXT("verified-s1m-lineage")) Out = TerrainDatumProofOrigin::VerifiedS1mLineage;
    else return false;
    return true;
}

TSharedRef<FJsonObject> TerrainSourceMixObject(const SkiDomain::TerrainSourceMixFractions& Mix)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("s1m"), Mix.S1M);
    Object->SetNumberField(TEXT("project1m"), Mix.Project1m);
    Object->SetNumberField(TEXT("arcSec13"), Mix.ArcSec13);
    Object->SetBoolField(TEXT("unknown"), Mix.Unknown);
    Object->SetBoolField(TEXT("estimated"), Mix.Estimated);
    return Object;
}

TSharedRef<FJsonObject> TerrainQualitySourceFactsObject(
    const SkiDomain::TerrainQualitySourceFacts& Source)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("sourceId"), UTF8_TO_TCHAR(Source.SourceId.c_str()));
    Object->SetStringField(TEXT("product"), ElevationProductName(Source.Product));
    Object->SetNumberField(TEXT("qualityLevel"), Source.QualityLevel);
    Object->SetBoolField(TEXT("qualityLevelUnknown"), Source.QualityLevelUnknown);
    Object->SetBoolField(TEXT("qualityLevelEstimated"), Source.QualityLevelEstimated);
    Object->SetStringField(TEXT("acquisitionStartDate"),
        UTF8_TO_TCHAR(Source.AcquisitionStartDate.c_str()));
    Object->SetStringField(TEXT("acquisitionEndDate"),
        UTF8_TO_TCHAR(Source.AcquisitionEndDate.c_str()));
    Object->SetBoolField(TEXT("acquisitionDateRangeUnknown"), Source.AcquisitionDateRangeUnknown);
    Object->SetBoolField(TEXT("acquisitionDateRangeEstimated"), Source.AcquisitionDateRangeEstimated);
    Object->SetNumberField(TEXT("verticalRmseMeters"), Source.VerticalRmseMeters);
    Object->SetBoolField(TEXT("verticalRmseUnknown"), Source.VerticalRmseUnknown);
    Object->SetBoolField(TEXT("verticalRmseEstimated"), Source.VerticalRmseEstimated);
    Object->SetStringField(TEXT("verticalDatum"), VerticalDatumName(Source.VerticalDatum));
    Object->SetStringField(TEXT("datumProofOrigin"), DatumProofOriginName(Source.DatumProofOrigin));
    Object->SetBoolField(TEXT("datumUnknown"), Source.DatumUnknown);
    Object->SetBoolField(TEXT("datumEstimated"), Source.DatumEstimated);
    Object->SetBoolField(TEXT("datumProven"), Source.DatumProven);
    Object->SetNumberField(TEXT("sampleFraction"), Source.SampleFraction);
    Object->SetBoolField(TEXT("sampleFractionUnknown"), Source.SampleFractionUnknown);
    Object->SetBoolField(TEXT("sampleFractionEstimated"), Source.SampleFractionEstimated);
    return Object;
}

bool ParseTerrainSourceMix(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::TerrainSourceMixFractions& Out)
{
    return Object
        && Object->TryGetNumberField(TEXT("s1m"), Out.S1M)
        && Object->TryGetNumberField(TEXT("project1m"), Out.Project1m)
        && Object->TryGetNumberField(TEXT("arcSec13"), Out.ArcSec13)
        && Object->TryGetBoolField(TEXT("unknown"), Out.Unknown)
        && Object->TryGetBoolField(TEXT("estimated"), Out.Estimated);
}

bool ParseTerrainQualitySourceFacts(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::TerrainQualitySourceFacts& Out)
{
    FString Product, VerticalDatum, DatumProofOrigin;
    return Object
        && ReadString(Object, TEXT("sourceId"), Out.SourceId)
        && Object->TryGetStringField(TEXT("product"), Product)
        && ParseElevationProduct(Product, Out.Product)
        && ReadUint8(Object, TEXT("qualityLevel"), Out.QualityLevel)
        && Object->TryGetBoolField(TEXT("qualityLevelUnknown"), Out.QualityLevelUnknown)
        && Object->TryGetBoolField(TEXT("qualityLevelEstimated"), Out.QualityLevelEstimated)
        && ReadString(Object, TEXT("acquisitionStartDate"), Out.AcquisitionStartDate)
        && ReadString(Object, TEXT("acquisitionEndDate"), Out.AcquisitionEndDate)
        && Object->TryGetBoolField(TEXT("acquisitionDateRangeUnknown"),
            Out.AcquisitionDateRangeUnknown)
        && Object->TryGetBoolField(TEXT("acquisitionDateRangeEstimated"),
            Out.AcquisitionDateRangeEstimated)
        && Object->TryGetNumberField(TEXT("verticalRmseMeters"), Out.VerticalRmseMeters)
        && Object->TryGetBoolField(TEXT("verticalRmseUnknown"), Out.VerticalRmseUnknown)
        && Object->TryGetBoolField(TEXT("verticalRmseEstimated"), Out.VerticalRmseEstimated)
        && Object->TryGetStringField(TEXT("verticalDatum"), VerticalDatum)
        && ParseVerticalDatum(VerticalDatum, Out.VerticalDatum)
        && Object->TryGetStringField(TEXT("datumProofOrigin"), DatumProofOrigin)
        && ParseDatumProofOrigin(DatumProofOrigin, Out.DatumProofOrigin)
        && Object->TryGetBoolField(TEXT("datumUnknown"), Out.DatumUnknown)
        && Object->TryGetBoolField(TEXT("datumEstimated"), Out.DatumEstimated)
        && Object->TryGetBoolField(TEXT("datumProven"), Out.DatumProven)
        && Object->TryGetNumberField(TEXT("sampleFraction"), Out.SampleFraction)
        && Object->TryGetBoolField(TEXT("sampleFractionUnknown"), Out.SampleFractionUnknown)
        && Object->TryGetBoolField(TEXT("sampleFractionEstimated"), Out.SampleFractionEstimated);
}

TSharedRef<FJsonObject> QualityObject(const SkiDomain::TerrainProvenanceCounts& Counts,
    const SkiDomain::TerrainQualityReport& Report)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetObjectField(TEXT("provenanceCounts"), ProvenanceCountsObject(Counts));
    Root->SetStringField(TEXT("grade"), GradeName(Report.Grade));
    Root->SetStringField(TEXT("totalSamples"), LexToString(Report.TotalSamples));
    Root->SetNumberField(TEXT("qualifiedLidarFraction"), Report.QualifiedLidarFraction);
    Root->SetNumberField(TEXT("blendInterpolatedFraction"), Report.BlendInterpolatedFraction);
    Root->SetNumberField(TEXT("backfillFraction"), Report.BackfillFraction);
    Root->SetNumberField(TEXT("coarseFraction"), Report.CoarseFraction);
    Root->SetNumberField(TEXT("noDataFraction"), Report.NoDataFraction);
    Root->SetNumberField(TEXT("unknownMetadataFraction"), Report.UnknownMetadataFraction);
    Root->SetBoolField(TEXT("hasNoDataWarning"), Report.HasNoDataWarning);
    // Schema-3 receipts written before source facts existed remain readable with their
    // original hash shape. New reports bind the full normalized source list and states.
    if (!Report.Sources.empty())
    {
        Root->SetObjectField(TEXT("sourceMix"), TerrainSourceMixObject(Report.SourceMix));
        TArray<TSharedPtr<FJsonValue>> Sources;
        Sources.Reserve(static_cast<int32>(Report.Sources.size()));
        for (const SkiDomain::TerrainQualitySourceFacts& Source : Report.Sources)
            Sources.Add(MakeShared<FJsonValueObject>(TerrainQualitySourceFactsObject(Source)));
        Root->SetArrayField(TEXT("sourceFacts"), Sources);
    }
    return Root;
}

bool ParseProvenanceCounts(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::TerrainProvenanceCounts& Out)
{
    return ReadUint64String(Object, TEXT("s1mNativeQualified"), Out.S1MNativeQualified)
        && ReadUint64String(Object, TEXT("s1mNativeOther"), Out.S1MNativeOther)
        && ReadUint64String(Object, TEXT("s1mBlend"), Out.S1MBlend)
        && ReadUint64String(Object, TEXT("s1mBackfill"), Out.S1MBackfill)
        && ReadUint64String(Object, TEXT("s1mInterpolated"), Out.S1MInterpolated)
        && ReadUint64String(Object, TEXT("project1mQualified"), Out.Project1mQualified)
        && ReadUint64String(Object, TEXT("project1mOther"), Out.Project1mOther)
        && ReadUint64String(Object, TEXT("arcSec13"), Out.ArcSec13)
        && ReadUint64String(Object, TEXT("noData"), Out.NoData)
        && ReadUint64String(Object, TEXT("unknownMetadata"), Out.UnknownMetadata);
}

bool ParseQuality(const TSharedPtr<FJsonObject>& Object,
    SkiDomain::TerrainProvenanceCounts& OutCounts,
    SkiDomain::TerrainQualityReport& OutReport)
{
    const TSharedPtr<FJsonObject>* CountsObject = nullptr;
    FString Grade;
    if (!Object || !Object->TryGetObjectField(TEXT("provenanceCounts"), CountsObject)
        || !CountsObject || !ParseProvenanceCounts(*CountsObject, OutCounts)
        || !Object->TryGetStringField(TEXT("grade"), Grade)
        || !ParseGrade(Grade, OutReport.Grade)
        || !ReadUint64String(Object, TEXT("totalSamples"), OutReport.TotalSamples)
        || !Object->TryGetNumberField(TEXT("qualifiedLidarFraction"), OutReport.QualifiedLidarFraction)
        || !Object->TryGetNumberField(TEXT("blendInterpolatedFraction"), OutReport.BlendInterpolatedFraction)
        || !Object->TryGetNumberField(TEXT("backfillFraction"), OutReport.BackfillFraction)
        || !Object->TryGetNumberField(TEXT("coarseFraction"), OutReport.CoarseFraction)
        || !Object->TryGetNumberField(TEXT("noDataFraction"), OutReport.NoDataFraction)
        || !Object->TryGetNumberField(TEXT("unknownMetadataFraction"), OutReport.UnknownMetadataFraction)
        || !Object->TryGetBoolField(TEXT("hasNoDataWarning"), OutReport.HasNoDataWarning))
        return false;
    const bool HasSourceFacts = Object->HasField(TEXT("sourceFacts"));
    const bool HasSourceMix = Object->HasField(TEXT("sourceMix"));
    if (HasSourceFacts != HasSourceMix) return false;
    if (!HasSourceFacts)
    {
        // The previous schema-3 shape carried counts only. Its exact source mix can
        // still be derived from those counts, while per-source lineage stays absent.
        SkiDomain::TerrainQualityReport Aggregate;
        if (!SkiDomain::TrySummarizeTerrainQuality(OutCounts, Aggregate)) return false;
        OutReport.SourceMix = Aggregate.SourceMix;
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
    const TSharedPtr<FJsonObject>* SourceMix = nullptr;
    if (!Object->TryGetArrayField(TEXT("sourceFacts"), Sources) || !Sources
        || Sources->Num() <= 0 || Sources->Num() > 2048
        || !Object->TryGetObjectField(TEXT("sourceMix"), SourceMix) || !SourceMix
        || !ParseTerrainSourceMix(*SourceMix, OutReport.SourceMix))
        return false;
    OutReport.Sources.reserve(static_cast<std::size_t>(Sources->Num()));
    for (const TSharedPtr<FJsonValue>& Item : *Sources)
    {
        const TSharedPtr<FJsonObject> SourceObject = Item ? Item->AsObject() : nullptr;
        SkiDomain::TerrainQualitySourceFacts Source;
        if (!ParseTerrainQualitySourceFacts(SourceObject, Source)) return false;
        OutReport.Sources.push_back(std::move(Source));
    }
    return true;
}

bool NearlyEqual(const double A, const double B) noexcept
{
    return std::isfinite(A) && std::isfinite(B)
        && std::abs(A - B) <= std::max({1.0, std::abs(A), std::abs(B)}) * 1.0e-12;
}

bool ValidMetadata(const std::string& Value, const std::size_t Maximum = 4096) noexcept
{
    return !Value.empty() && Value.size() <= Maximum
        && std::none_of(Value.begin(), Value.end(), [](const unsigned char Character)
        {
            return Character < 0x20U || Character == 0x7fU;
        });
}

std::string LowerAscii(std::string Value)
{
    std::transform(Value.begin(), Value.end(), Value.begin(), [](const unsigned char Character)
    {
        return static_cast<char>(std::tolower(Character));
    });
    return Value;
}

bool ContainsAsciiInsensitive(const std::string& Value, const std::string& Needle)
{
    return LowerAscii(Value).find(LowerAscii(Needle)) != std::string::npos;
}

bool IsNamespacedOsmElementId(const FString& Value)
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
        return false;
    for (int32 Index = 0; Index < NumericId.Len(); ++Index)
    {
        const TCHAR Character = NumericId[Index];
        if (Character < TEXT('0') || Character > TEXT('9')) return false;
    }
    std::uint64_t ParsedId = 0;
    return LexTryParseString(ParsedId, *NumericId) && ParsedId > 0;
}

FString ImageryPath(const SiteContextImageryTile& Tile)
{
    return FString::Printf(TEXT("imagery/lod%u/%u/%u.jpg"),
        static_cast<uint32>(Tile.LodIndex), Tile.TileX, Tile.TileY);
}

FString AssetPathFString(const std::string& Path)
{
    return UTF8_TO_TCHAR(Path.c_str());
}

bool ParseComponentKind(const FString& Value, CompositeInstallComponentKind& Out)
{
    if (Value == TEXT("terrain-core")) Out = CompositeInstallComponentKind::TerrainCore;
    else if (Value == TEXT("cover-ecology")) Out = CompositeInstallComponentKind::CoverEcology;
    else if (Value == TEXT("site-context")) Out = CompositeInstallComponentKind::SiteContext;
    else if (Value == TEXT("quality-report")) Out = CompositeInstallComponentKind::QualityReport;
    else return false;
    return true;
}

const TCHAR* ComponentKindName(const CompositeInstallComponentKind Kind)
{
    switch (Kind)
    {
    case CompositeInstallComponentKind::TerrainCore: return TEXT("terrain-core");
    case CompositeInstallComponentKind::CoverEcology: return TEXT("cover-ecology");
    case CompositeInstallComponentKind::SiteContext: return TEXT("site-context");
    case CompositeInstallComponentKind::QualityReport: return TEXT("quality-report");
    default: return TEXT("invalid");
    }
}

bool ParseComponentStatus(const FString& Value, CompositeInstallComponentStatus& Out)
{
    if (Value == TEXT("missing")) Out = CompositeInstallComponentStatus::Missing;
    else if (Value == TEXT("staged")) Out = CompositeInstallComponentStatus::Staged;
    else if (Value == TEXT("verified")) Out = CompositeInstallComponentStatus::Verified;
    else if (Value == TEXT("failed")) Out = CompositeInstallComponentStatus::Failed;
    else return false;
    return true;
}

const TCHAR* ComponentStatusName(const CompositeInstallComponentStatus Status)
{
    switch (Status)
    {
    case CompositeInstallComponentStatus::Missing: return TEXT("missing");
    case CompositeInstallComponentStatus::Staged: return TEXT("staged");
    case CompositeInstallComponentStatus::Verified: return TEXT("verified");
    case CompositeInstallComponentStatus::Failed: return TEXT("failed");
    default: return TEXT("invalid");
    }
}
}

namespace
{
bool IsWithinDirectory(const FString& Directory, const FString& Candidate)
{
    FString Base = FPaths::ConvertRelativePathToFull(Directory);
    FString Value = FPaths::ConvertRelativePathToFull(Candidate);
    FPaths::NormalizeDirectoryName(Base);
    FPaths::NormalizeFilename(Value);
    return Value.Equals(Base, ESearchCase::IgnoreCase)
        || Value.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase);
}

bool NoReparsePath(const FString& Path, const bool RequireLeaf, FString& Error)
{
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full);
    TArray<FString> Parts;
    for (FString Cursor = Full; !Cursor.IsEmpty();)
    {
        Parts.Add(Cursor);
        const FString Parent = FPaths::GetPath(Cursor);
        if (Parent.IsEmpty() || Parent.Equals(Cursor, ESearchCase::IgnoreCase)) break;
        Cursor = Parent;
    }
    Algo::Reverse(Parts);
    IFileManager& Files = IFileManager::Get();
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        const FString& Item = Parts[Index];
        const bool Exists = Files.FileExists(*Item) || Files.DirectoryExists(*Item);
        if (!Exists) continue;
        if (Files.IsSymlink(*Item))
        {
            Error = TEXT("SiteContext storage path contains a reparse point.");
            return false;
        }
        if (Index + 1 < Parts.Num() && !Files.DirectoryExists(*Item))
        {
            Error = TEXT("SiteContext storage ancestor is not a directory.");
            return false;
        }
    }
    if (RequireLeaf && !Files.FileExists(*Full) && !Files.DirectoryExists(*Full))
    {
        Error = TEXT("SiteContext storage path is missing.");
        return false;
    }
    return true;
}

bool EnsureDirectory(const FString& Root, const FString& Directory, FString& Error)
{
    if (!IsWithinDirectory(Root, Directory) || !NoReparsePath(Root, false, Error)
        || !NoReparsePath(Directory, false, Error)
        || !IFileManager::Get().MakeDirectory(*Directory, true)
        || !NoReparsePath(Directory, true, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to create secure SiteContext directory.");
        return false;
    }
    return true;
}

bool TreeContainsExactly(const FString& Directory, const TSet<FString>& Expected,
    FString& Error)
{
    if (!NoReparsePath(Directory, true, Error)) return false;
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    TSet<FString> Actual;
    bool Valid = true;
    TFunction<bool(const FString&)> Visit = [&](const FString& Current)
    {
        return Platform.IterateDirectory(*Current,
            [&](const TCHAR* Name, const bool IsDirectory)
            {
                const FString Entry(Name);
                if (IFileManager::Get().IsSymlink(*Entry))
                {
                    Valid = false;
                    return false;
                }
                if (IsDirectory) return Visit(Entry);
                FString Relative = Entry;
                const FString Prefix = Directory.EndsWith(TEXT("/"))
                    ? Directory : Directory + TEXT("/");
                if (!FPaths::MakePathRelativeTo(Relative, *Prefix))
                {
                    Valid = false;
                    return false;
                }
                FPaths::NormalizeFilename(Relative);
                Actual.Add(Relative.ToLower());
                return true;
            }) && Valid;
    };
    if (!Visit(Directory) || Actual.Num() != Expected.Num())
    {
        Error = TEXT("SiteContext package has missing or undeclared files.");
        return false;
    }
    for (const FString& Item : Expected)
    {
        if (!Actual.Contains(Item.ToLower()))
        {
            Error = TEXT("SiteContext package has missing or undeclared files.");
            return false;
        }
    }
    return true;
}

bool LoadBoundedFile(const FString& Root, const FString& Path, const std::uint64_t Maximum,
    TArray<uint8>& Out, FString& Error)
{
    Out.Reset();
    if (!IsWithinDirectory(Root, Path) || !NoReparsePath(Path, true, Error)) return false;
    const int64 Size = IFileManager::Get().FileSize(*Path);
    if (Size <= 0 || static_cast<std::uint64_t>(Size) > Maximum || Size > MAX_int32)
    {
        Error = TEXT("SiteContext asset is missing or oversized.");
        return false;
    }
    if (!FFileHelper::LoadFileToArray(Out, *Path) || Out.Num() != Size
        || !NoReparsePath(Path, true, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to read SiteContext asset completely.");
        Out.Reset();
        return false;
    }
    return true;
}

bool ValidateOsmPolylineJson(const TArray<uint8>& Bytes,
    const std::string& VectorEncoding,
    const SkiDomain::TerrainCoreManifest& TerrainCore, FString& Error)
{
    const SkiDomain::MetricBounds& Extent = TerrainCore.OuterBounds;
    if (!std::isfinite(Extent.WestM) || !std::isfinite(Extent.SouthM)
        || !std::isfinite(Extent.EastM) || !std::isfinite(Extent.NorthM)
        || Extent.WestM > Extent.EastM || Extent.SouthM > Extent.NorthM)
    {
        Error = TEXT("SiteContext TerrainCore outer bounds are invalid.");
        return false;
    }

    bool HasNulByte = false;
    if (!Bytes.IsEmpty() && Bytes.Num() <= static_cast<int32>(SiteContextMaxAssetBytes))
    {
        for (int32 Index = 0; Index < Bytes.Num(); ++Index)
        {
            if (Bytes[Index] == 0)
            {
                HasNulByte = true;
                break;
            }
        }
    }

    if (Bytes.IsEmpty() || Bytes.Num() > static_cast<int32>(SiteContextMaxAssetBytes)
        || HasNulByte)
    {
        Error = TEXT("SiteContext OSM vector JSON is empty, oversized, or contains NUL bytes.");
        return false;
    }
    const std::string Utf8Json(reinterpret_cast<const char*>(Bytes.GetData()),
        static_cast<std::size_t>(Bytes.Num()));
    const FString Json = UTF8_TO_TCHAR(Utf8Json.c_str());
    TSharedPtr<FJsonObject> RootObject;
    if (!ParseJsonObject(Json, RootObject, Error)) return false;
    std::uint32_t Schema = 0;
    const TArray<TSharedPtr<FJsonValue>>* Features = nullptr;
    const bool EncodingV1 = VectorEncoding == SiteContextVectorEncodingV1;
    const bool EncodingV2 = VectorEncoding == SiteContextVectorEncodingV2;
    if (!ReadUint32(RootObject, TEXT("schemaVersion"), Schema)
        || (Schema != 1 && Schema != 2)
        || (Schema == 1 && !EncodingV1) || (Schema == 2 && !EncodingV2)
        || !RootObject->TryGetArrayField(TEXT("features"), Features) || !Features
        || Features->Num() > 500000)
    {
        Error = TEXT("SiteContext OSM vector JSON has an unsupported or oversized feature list.");
        return false;
    }

    std::uint64_t TotalPoints = 0;
    std::unordered_set<std::string> SinglePartFeatureIdentities;
    std::string PreviousKind;
    std::string PreviousOsmId;
    std::uint32_t PreviousPartIndex = 0;
    bool HasPreviousSchema2Feature = false;
    for (int32 FeatureIndex = 0; FeatureIndex < Features->Num(); ++FeatureIndex)
    {
        const TSharedPtr<FJsonObject> Feature = (*Features)[FeatureIndex]
            ? (*Features)[FeatureIndex]->AsObject() : nullptr;
        FString OsmId, Kind;
        const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
        std::uint32_t PartIndex = 0;
        if (!Feature || !Feature->TryGetStringField(TEXT("osmId"), OsmId)
            || !Feature->TryGetStringField(TEXT("kind"), Kind)
            || !Feature->TryGetArrayField(TEXT("points"), Points) || !Points
            || Points->Num() < 2 || Points->Num() > 100000
            || !ValidMetadata(Utf8(OsmId), 128)
            || (Kind != TEXT("road") && Kind != TEXT("lift") && Kind != TEXT("trail")))
        {
            Error = TEXT("SiteContext OSM vector feature metadata or polyline is invalid.");
            return false;
        }
        const std::string KindKey = Utf8(Kind);
        const std::string OsmIdKey = Utf8(OsmId);
        if (Schema == 1)
        {
            if (Feature->HasField(TEXT("partIndex")))
            {
                Error = TEXT("Schema-1 OSM vectors cannot carry a part index.");
                return false;
            }
            std::string Identity = KindKey;
            Identity.push_back('\0');
            Identity += OsmIdKey;
            if (!SinglePartFeatureIdentities.insert(std::move(Identity)).second)
            {
                Error = TEXT("Schema-1 OSM vectors contain an ambiguous repeated element part.");
                return false;
            }
        }
        else
        {
            if (!IsNamespacedOsmElementId(OsmId)
                || !ReadUint32(Feature, TEXT("partIndex"), PartIndex))
            {
                Error = TEXT("Schema-2 OSM vectors require a namespaced positive OSM ID and part index.");
                return false;
            }
            if (HasPreviousSchema2Feature)
            {
                if (KindKey < PreviousKind
                    || (KindKey == PreviousKind && OsmIdKey < PreviousOsmId))
                {
                    Error = TEXT("Schema-2 OSM vector parts are not sorted by kind and element ID.");
                    return false;
                }
                if (KindKey == PreviousKind && OsmIdKey == PreviousOsmId)
                {
                    if (PreviousPartIndex == MAX_uint32
                        || PartIndex != PreviousPartIndex + 1U)
                    {
                        Error = TEXT("Schema-2 OSM part indices must be unique and contiguous from zero.");
                        return false;
                    }
                }
                else if (PartIndex != 0)
                {
                    Error = TEXT("Schema-2 OSM parts must start at index zero for each element.");
                    return false;
                }
            }
            else if (PartIndex != 0)
            {
                Error = TEXT("Schema-2 OSM parts must start at index zero for each element.");
                return false;
            }
            PreviousKind = KindKey;
            PreviousOsmId = OsmIdKey;
            PreviousPartIndex = PartIndex;
            HasPreviousSchema2Feature = true;
        }
        if (TotalPoints > 2000000ULL - static_cast<std::uint64_t>(Points->Num()))
        {
            Error = TEXT("SiteContext OSM vector point count exceeds its limit.");
            return false;
        }
        TotalPoints += static_cast<std::uint64_t>(Points->Num());
        for (int32 PointIndex = 0; PointIndex < Points->Num(); ++PointIndex)
        {
            const TSharedPtr<FJsonObject> Point = (*Points)[PointIndex]
                ? (*Points)[PointIndex]->AsObject() : nullptr;
            double EastM = 0.0;
            double NorthM = 0.0;
            if (!Point || !Point->TryGetNumberField(TEXT("eastM"), EastM)
                || !Point->TryGetNumberField(TEXT("northM"), NorthM)
                || !FMath::IsFinite(EastM) || !FMath::IsFinite(NorthM)
                || std::abs(EastM) > 100000.0 || std::abs(NorthM) > 100000.0
                || EastM < Extent.WestM || EastM > Extent.EastM
                || NorthM < Extent.SouthM || NorthM > Extent.NorthM)
            {
                Error = TEXT("SiteContext OSM vector point is outside the TerrainCore ENU extent.");
                return false;
            }
        }
    }
    return true;
}

bool VerifySiteContextIndex(const FString& Root, const SiteContextPackageIndex& Index,
    FString& Error)
{
    if (!IsWithinDirectory(Root, Index.PackageDirectory)
        || !NoReparsePath(Index.PackageDirectory, true, Error)) return false;
    const SiteContextValidation Validation = ValidateSiteContextManifest(Index.Manifest,
        Index.TerrainCore);
    if (!Validation.Ok())
    {
        Error = TEXT("SiteContext index no longer matches its TerrainCore manifest.");
        return false;
    }
    TSet<FString> ExpectedFiles{TEXT("manifest.json")};
    for (const SiteContextAsset& Asset : Index.Manifest.Assets)
    {
        ExpectedFiles.Add(AssetPathFString(Asset.Path).ToLower());
        TArray<uint8> Bytes;
        if (!LoadBoundedFile(Root, FPaths::Combine(Index.PackageDirectory,
                AssetPathFString(Asset.Path)), SiteContextMaxAssetBytes, Bytes, Error)) return false;
        if (static_cast<std::uint64_t>(Bytes.Num()) != Asset.Length
            || !Sha256(Bytes).Equals(UTF8_TO_TCHAR(Asset.Sha256.c_str()), ESearchCase::CaseSensitive))
        {
            Error = TEXT("SiteContext asset length or SHA-256 does not match its manifest.");
            return false;
        }
        if (Asset.Type == Index.Manifest.ImageryEncoding)
        {
            IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
                TEXT("ImageWrapper"));
            const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::JPEG);
            TArray<uint8> Decoded;
            if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
                || Wrapper->GetWidth() != static_cast<int32>(SiteContextImageryTilePixels)
                || Wrapper->GetHeight() != static_cast<int32>(SiteContextImageryTilePixels)
                || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Decoded)
                || Decoded.Num() != static_cast<int32>(SiteContextImageryTilePixels
                    * SiteContextImageryTilePixels * sizeof(FColor)))
            {
                Error = TEXT("SiteContext imagery is not a decodable 256 by 256 JPEG tile.");
                return false;
            }
        }
        else if (Asset.Type == Index.Manifest.VectorEncoding
            && !ValidateOsmPolylineJson(Bytes, Index.Manifest.VectorEncoding,
                Index.TerrainCore, Error))
        {
            return false;
        }
    }
    if (!TreeContainsExactly(Index.PackageDirectory, ExpectedFiles, Error)) return false;
    return true;
}

bool OpenSiteContextAt(const FString& Root, const FString& Directory,
    const FString& ExpectedId, const SkiDomain::TerrainCoreManifest& Core,
    SiteContextPackageIndex& OutIndex, FString& Error)
{
    OutIndex = {};
    if (!IsCanonicalSha256(Utf8(ExpectedId)) || !IsWithinDirectory(Root, Directory)
        || !NoReparsePath(Directory, true, Error)) return false;
    TArray<uint8> ManifestBytes;
    const FString ManifestPath = FPaths::Combine(Directory, TEXT("manifest.json"));
    if (!LoadBoundedFile(Root, ManifestPath, SiteContextMaxManifestBytes,
            ManifestBytes, Error)) return false;
    ManifestBytes.Add(0);
    const FString Json = UTF8_TO_TCHAR(reinterpret_cast<const char*>(ManifestBytes.GetData()));
    SiteContextManifest Manifest;
    const std::string ExpectedIdentity = Utf8(ExpectedId);
    if (!ParseSiteContextManifest(Json, Core, Manifest, Error)) return false;
    if (Manifest.ContentId != ExpectedIdentity)
    {
        Error = TEXT("SiteContext package identity does not match its directory.");
        return false;
    }
    const FString Canonical = SerializeSiteContextManifest(Manifest, true);
    const FTCHARToUTF8 Encoded(*Canonical);
    if (ManifestBytes.Num() != Encoded.Length() + 1
        || FMemory::Memcmp(ManifestBytes.GetData(), Encoded.Get(), Encoded.Length()) != 0)
    {
        Error = TEXT("SiteContext manifest is not canonical.");
        return false;
    }
    SiteContextPackageIndex Candidate;
    Candidate.Manifest = std::move(Manifest);
    Candidate.TerrainCore = Core;
    Candidate.PackageDirectory = Directory;
    TSet<FString> ExpectedFiles{TEXT("manifest.json")};
    for (const SiteContextAsset& Asset : Candidate.Manifest.Assets)
        ExpectedFiles.Add(AssetPathFString(Asset.Path).ToLower());
    if (!TreeContainsExactly(Directory, ExpectedFiles, Error)) return false;
    OutIndex = std::move(Candidate);
    return true;
}

void SafeCleanup(const FString& Parent, const FString& Directory)
{
    FString Error;
    if (IsWithinDirectory(Parent, Directory) && !FPaths::IsSamePath(Parent, Directory)
        && IFileManager::Get().DirectoryExists(*Directory)
        && NoReparsePath(Directory, true, Error))
    {
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
    }
}
}

SkiPreparation::SiteContextStore::SiteContextStore(FString InDataRoot)
    : Root(FPaths::ConvertRelativePathToFull(std::move(InDataRoot)))
{
}

bool SkiPreparation::SiteContextStore::WriteAndActivate(SiteContextManifest Manifest,
    const SkiDomain::TerrainCoreManifest& VerifiedTerrainCore,
    const SiteContextAssetReader& ReadAsset, FString& OutPackageDirectory,
    SiteContextManifest& OutManifest, FString& OutError) const
{
    OutPackageDirectory.Reset();
    OutManifest = {};
    OutError.Reset();
    if (!ReadAsset || Manifest.Assets.empty()
        || Manifest.Assets.size() > SiteContextMaxTiles + 1U)
    {
        OutError = TEXT("SiteContext asset reader is missing or the manifest asset list is invalid.");
        return false;
    }
    Manifest.ContentId = std::string(64, '0');
    std::sort(Manifest.Assets.begin(), Manifest.Assets.end(),
        [](const SiteContextAsset& A, const SiteContextAsset& B)
        {
            return LowerAscii(A.Path) < LowerAscii(B.Path);
        });
    if (!ValidateSiteContextManifest(Manifest, VerifiedTerrainCore).Ok())
    {
        OutError = TEXT("SiteContext manifest failed validation before storage.");
        return false;
    }

    const FString UnsignedJson = SerializeSiteContextManifest(Manifest, false);
    const FTCHARToUTF8 UnsignedBytes(*UnsignedJson);
    const TArrayView<const uint8> UnsignedView(
        reinterpret_cast<const uint8*>(UnsignedBytes.Get()), UnsignedBytes.Length());
    Manifest.ContentId = Utf8(Sha256(UnsignedView));
    const FString Json = SerializeSiteContextManifest(Manifest, true);
    const FTCHARToUTF8 JsonBytes(*Json);
    if (!ValidateSiteContextManifest(Manifest, VerifiedTerrainCore,
            static_cast<std::uint64_t>(JsonBytes.Length())).Ok())
    {
        OutError = TEXT("Generated SiteContext manifest failed validation.");
        return false;
    }

    const FString StagingParent = FPaths::Combine(Root, TEXT(".sitecontext-staging"));
    const FString Stage = FPaths::Combine(StagingParent,
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".work"));
    const FString Packages = FPaths::Combine(Root, TEXT("SiteContext"));
    const FString Target = FPaths::Combine(Packages, UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    if (!EnsureDirectory(Root, StagingParent, OutError)
        || !EnsureDirectory(Root, Stage, OutError))
    {
        SafeCleanup(StagingParent, Stage);
        return false;
    }
    const auto Cleanup = [&]() { SafeCleanup(StagingParent, Stage); };
    for (const SiteContextAsset& Asset : Manifest.Assets)
    {
        const FString RelativePath = AssetPathFString(Asset.Path);
        TArray<uint8> Bytes;
        if (!ReadAsset(RelativePath, Bytes, OutError))
        {
            if (OutError.IsEmpty()) OutError = TEXT("SiteContext asset reader failed.");
            Cleanup();
            return false;
        }
        if (Bytes.IsEmpty() || static_cast<std::uint64_t>(Bytes.Num()) != Asset.Length
            || !Sha256(Bytes).Equals(UTF8_TO_TCHAR(Asset.Sha256.c_str()), ESearchCase::CaseSensitive))
        {
            OutError = TEXT("SiteContext asset reader returned bytes with an identity or length mismatch.");
            Cleanup();
            return false;
        }
        const FString Destination = FPaths::Combine(Stage, RelativePath);
        if (!EnsureDirectory(Stage, FPaths::GetPath(Destination), OutError)
            || !FFileHelper::SaveArrayToFile(Bytes, *Destination))
        {
            if (OutError.IsEmpty()) OutError = TEXT("Unable to write staged SiteContext asset.");
            Cleanup();
            return false;
        }
    }
    if (!FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Stage, TEXT("manifest.json")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = TEXT("Unable to write staged SiteContext manifest.");
        Cleanup();
        return false;
    }
    SiteContextPackageIndex Staged;
    if (!OpenSiteContextAt(Root, Stage, UTF8_TO_TCHAR(Manifest.ContentId.c_str()),
            VerifiedTerrainCore, Staged, OutError)
        || !VerifySiteContextIndex(Root, Staged, OutError))
    {
        Cleanup();
        return false;
    }
    if (!EnsureDirectory(Root, Packages, OutError))
    {
        Cleanup();
        return false;
    }
    if (IFileManager::Get().DirectoryExists(*Target))
    {
        SiteContextPackageIndex Existing;
        if (!OpenSiteContextAt(Root, Target, UTF8_TO_TCHAR(Manifest.ContentId.c_str()),
                VerifiedTerrainCore, Existing, OutError)
            || !VerifySiteContextIndex(Root, Existing, OutError))
        {
            Cleanup();
            return false;
        }
        Cleanup();
    }
    else
    {
        if (!NoReparsePath(Packages, true, OutError)
            || !IFileManager::Get().Move(*Target, *Stage, false, false, true, true))
        {
            if (OutError.IsEmpty()) OutError = TEXT("Unable to activate immutable SiteContext package.");
            Cleanup();
            return false;
        }
    }
    SiteContextPackageIndex Final;
    const FString Id = UTF8_TO_TCHAR(Manifest.ContentId.c_str());
    if (!Open(Id, VerifiedTerrainCore, Final, OutError) || !Verify(Final, OutError)) return false;
    OutPackageDirectory = Target;
    OutManifest = std::move(Final.Manifest);
    return true;
}

bool SkiPreparation::SiteContextStore::Open(const FString& ContentId,
    const SkiDomain::TerrainCoreManifest& VerifiedTerrainCore,
    SiteContextPackageIndex& OutIndex, FString& OutError) const
{
    OutIndex = {};
    OutError.Reset();
    return OpenSiteContextAt(Root,
        FPaths::Combine(Root, TEXT("SiteContext"), ContentId), ContentId,
        VerifiedTerrainCore, OutIndex, OutError);
}

bool SkiPreparation::SiteContextStore::Verify(const SiteContextPackageIndex& Index,
    FString& OutError) const
{
    OutError.Reset();
    SiteContextPackageIndex Reopened;
    const FString Id = UTF8_TO_TCHAR(Index.Manifest.ContentId.c_str());
    if (!OpenSiteContextAt(Root, Index.PackageDirectory, Id,
            Index.TerrainCore, Reopened, OutError)) return false;
    return VerifySiteContextIndex(Root, Reopened, OutError);
}

bool SkiPreparation::SiteContextStore::ReadAsset(const SiteContextPackageIndex& Index,
    const FString& RelativePath, TArray<uint8>& OutBytes, FString& OutError) const
{
    OutBytes.Reset();
    OutError.Reset();
    const std::string Path = Utf8(RelativePath);
    const auto Asset = std::find_if(Index.Manifest.Assets.begin(), Index.Manifest.Assets.end(),
        [&](const SiteContextAsset& Candidate) { return Candidate.Path == Path; });
    if (Asset == Index.Manifest.Assets.end() || !SkiDomain::IsSafeTerrainCorePath(Path))
    {
        OutError = TEXT("Requested SiteContext asset is not declared.");
        return false;
    }
    if (!LoadBoundedFile(Root, FPaths::Combine(Index.PackageDirectory, RelativePath),
            SiteContextMaxAssetBytes, OutBytes, OutError)) return false;
    if (static_cast<std::uint64_t>(OutBytes.Num()) != Asset->Length
        || !Sha256(OutBytes).Equals(UTF8_TO_TCHAR(Asset->Sha256.c_str()), ESearchCase::CaseSensitive))
    {
        OutBytes.Reset();
        OutError = TEXT("Requested SiteContext asset failed its integrity check.");
        return false;
    }
    return true;
}

namespace
{
TSharedRef<FJsonObject> CompositeReceiptObject(const CompositeInstallReceipt& Receipt,
    const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Receipt.SchemaVersion);
    if (IncludeContentId)
        Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Receipt.ContentId.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Receipt.GeneratorVersion.c_str()));
    TArray<TSharedPtr<FJsonValue>> Components;
    for (const CompositeInstallComponent& Component : Receipt.Components)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("kind"), ComponentKindName(Component.Kind));
        Object->SetStringField(TEXT("status"), ComponentStatusName(Component.Status));
        Object->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Component.ContentId.c_str()));
        Object->SetStringField(TEXT("manifestSha256"), UTF8_TO_TCHAR(Component.ManifestSha256.c_str()));
        Components.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("components"), Components);
    Root->SetObjectField(TEXT("qualityReport"), QualityObject(Receipt.ProvenanceCounts,
        Receipt.Quality));
    return Root;
}
}

FString SkiPreparation::SerializeCompositeInstallReceipt(const CompositeInstallReceipt& Receipt,
    const bool IncludeContentId)
{
    FString Output;
    SerializeJsonObject(CompositeReceiptObject(Receipt, IncludeContentId), Output);
    return Output;
}

FString SkiPreparation::ComputeTerrainQualityReportId(
    const SkiDomain::TerrainProvenanceCounts& Counts,
    const SkiDomain::TerrainQualityReport& Report)
{
    FString Json;
    SerializeJsonObject(QualityObject(Counts, Report), Json);
    const FTCHARToUTF8 Encoded(*Json);
    return Sha256(TArrayView<const uint8>(
        reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
}

FString SkiPreparation::ComputeCompositeInstallReceiptId(const CompositeInstallReceipt& Receipt)
{
    const FString Json = SerializeCompositeInstallReceipt(Receipt, false);
    const FTCHARToUTF8 Encoded(*Json);
    return Sha256(TArrayView<const uint8>(
        reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
}

SkiPreparation::CompositeReceiptValidation SkiPreparation::ValidateCompositeInstallReceipt(
    const CompositeInstallReceipt& Receipt, const std::uint64_t SerializedReceiptBytes) noexcept
{
    if (Receipt.SchemaVersion != CompositeInstallReceiptSchema)
        return {CompositeReceiptError::UnsupportedSchema, 0};
    if (SerializedReceiptBytes > CompositeInstallReceiptMaxBytes)
        return {CompositeReceiptError::ReceiptTooLarge, 0};
    if (!IsCanonicalSha256(Receipt.ContentId))
        return {CompositeReceiptError::InvalidIdentity, 0};
    if (!ValidMetadata(Receipt.GeneratorVersion))
        return {CompositeReceiptError::InvalidMetadata, 0};
    if (Receipt.Components.size() != 4)
        return {CompositeReceiptError::MissingComponent, Receipt.Components.size()};

    SkiDomain::TerrainQualityReport ExpectedReport;
    const bool HasSourceFacts = !Receipt.Quality.Sources.empty();
    const bool QualitySummarized = HasSourceFacts
        ? SkiDomain::TrySummarizeTerrainQuality(Receipt.ProvenanceCounts,
            Receipt.Quality.Sources, ExpectedReport)
        : SkiDomain::TrySummarizeTerrainQuality(Receipt.ProvenanceCounts, ExpectedReport);
    if (!QualitySummarized
        || ExpectedReport.Grade != Receipt.Quality.Grade
        || ExpectedReport.TotalSamples != Receipt.Quality.TotalSamples
        || ExpectedReport.HasNoDataWarning != Receipt.Quality.HasNoDataWarning
        || !NearlyEqual(ExpectedReport.QualifiedLidarFraction, Receipt.Quality.QualifiedLidarFraction)
        || !NearlyEqual(ExpectedReport.BlendInterpolatedFraction, Receipt.Quality.BlendInterpolatedFraction)
        || !NearlyEqual(ExpectedReport.BackfillFraction, Receipt.Quality.BackfillFraction)
        || !NearlyEqual(ExpectedReport.CoarseFraction, Receipt.Quality.CoarseFraction)
        || !NearlyEqual(ExpectedReport.NoDataFraction, Receipt.Quality.NoDataFraction)
        || !NearlyEqual(ExpectedReport.UnknownMetadataFraction, Receipt.Quality.UnknownMetadataFraction)
        || (HasSourceFacts
            && (!NearlyEqual(ExpectedReport.SourceMix.S1M, Receipt.Quality.SourceMix.S1M)
                || !NearlyEqual(ExpectedReport.SourceMix.Project1m, Receipt.Quality.SourceMix.Project1m)
                || !NearlyEqual(ExpectedReport.SourceMix.ArcSec13, Receipt.Quality.SourceMix.ArcSec13)
                || ExpectedReport.SourceMix.Unknown != Receipt.Quality.SourceMix.Unknown
                || ExpectedReport.SourceMix.Estimated != Receipt.Quality.SourceMix.Estimated)))
        return {CompositeReceiptError::InvalidQuality, 0};

    bool Seen[4]{};
    const FString ExpectedQualityId = ComputeTerrainQualityReportId(
        Receipt.ProvenanceCounts, Receipt.Quality);
    for (std::size_t Index = 0; Index < Receipt.Components.size(); ++Index)
    {
        const CompositeInstallComponent& Component = Receipt.Components[Index];
        const std::size_t KindIndex = static_cast<std::size_t>(Component.Kind);
        if (KindIndex >= 4) return {CompositeReceiptError::InvalidComponent, Index};
        if (Seen[KindIndex]) return {CompositeReceiptError::DuplicateComponent, Index};
        Seen[KindIndex] = true;
        if (!IsCanonicalSha256(Component.ContentId)
            || !IsCanonicalSha256(Component.ManifestSha256))
            return {CompositeReceiptError::InvalidComponent, Index};
        if (Component.Status != CompositeInstallComponentStatus::Verified)
            return {CompositeReceiptError::ComponentNotVerified, Index};
        if (Component.Kind == CompositeInstallComponentKind::QualityReport
            && (Component.ContentId != Utf8(ExpectedQualityId)
                || Component.ManifestSha256 != Utf8(ExpectedQualityId)))
            return {CompositeReceiptError::InvalidComponent, Index};
    }
    for (const bool Present : Seen)
        if (!Present) return {CompositeReceiptError::MissingComponent, 0};
    if (Receipt.ContentId != Utf8(ComputeCompositeInstallReceiptId(Receipt)))
        return {CompositeReceiptError::InvalidIdentity, 0};
    return {};
}

bool SkiPreparation::ParseCompositeInstallReceipt(const FString& Json,
    CompositeInstallReceipt& OutReceipt, FString& OutError)
{
    OutReceipt = {};
    OutError.Reset();
    const FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0
        || static_cast<std::uint64_t>(Encoded.Length()) > CompositeInstallReceiptMaxBytes)
    {
        OutError = TEXT("Composite install receipt is empty or oversized.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!ParseJsonObject(Json, Root, OutError)) return false;
    CompositeInstallReceipt Value;
    if (!ReadUint32(Root, TEXT("schemaVersion"), Value.SchemaVersion)
        || !ReadString(Root, TEXT("contentId"), Value.ContentId)
        || !ReadString(Root, TEXT("generatorVersion"), Value.GeneratorVersion))
    {
        OutError = TEXT("Composite install receipt fields are missing or invalid.");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    const TSharedPtr<FJsonObject>* Quality = nullptr;
    if (!Root->TryGetArrayField(TEXT("components"), Components) || !Components
        || Components->Num() > 4
        || !Root->TryGetObjectField(TEXT("qualityReport"), Quality) || !Quality
        || !ParseQuality(*Quality, Value.ProvenanceCounts, Value.Quality))
    {
        OutError = TEXT("Composite components or quality report are missing or invalid.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Item : *Components)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        CompositeInstallComponent Component;
        FString Kind, Status;
        if (!Object || !Object->TryGetStringField(TEXT("kind"), Kind)
            || !Object->TryGetStringField(TEXT("status"), Status)
            || !ParseComponentKind(Kind, Component.Kind)
            || !ParseComponentStatus(Status, Component.Status)
            || !ReadString(Object, TEXT("contentId"), Component.ContentId)
            || !ReadString(Object, TEXT("manifestSha256"), Component.ManifestSha256))
        {
            OutError = TEXT("Composite component receipt is invalid.");
            return false;
        }
        Value.Components.push_back(std::move(Component));
    }
    const CompositeReceiptValidation Validation = ValidateCompositeInstallReceipt(Value,
        static_cast<std::uint64_t>(Encoded.Length()));
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("Composite install receipt validation failed (%d, item %llu)."),
            static_cast<int32>(Validation.Error), static_cast<std::uint64_t>(Validation.Index));
        return false;
    }
    const FString Canonical = SerializeCompositeInstallReceipt(Value, true);
    const FTCHARToUTF8 CanonicalBytes(*Canonical);
    if (CanonicalBytes.Length() != Encoded.Length()
        || FMemory::Memcmp(CanonicalBytes.Get(), Encoded.Get(), Encoded.Length()) != 0)
    {
        OutError = TEXT("Composite install receipt is not canonical.");
        return false;
    }
    OutReceipt = std::move(Value);
    return true;
}

bool SkiPreparation::VerifyCompositeInstallReceiptForActivation(
    const CompositeInstallReceipt& Receipt, const CompositeComponentVerifier& VerifyComponent,
    FString& OutError)
{
    OutError.Reset();
    const CompositeReceiptValidation Validation = ValidateCompositeInstallReceipt(Receipt);
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("Composite activation rejected (%d, item %llu)."),
            static_cast<int32>(Validation.Error), static_cast<std::uint64_t>(Validation.Index));
        return false;
    }
    if (!VerifyComponent)
    {
        OutError = TEXT("Composite activation requires a component verification adapter.");
        return false;
    }
    for (const CompositeInstallComponent& Component : Receipt.Components)
    {
        if (Component.Kind == CompositeInstallComponentKind::QualityReport) continue;
        FString ComponentError;
        if (!VerifyComponent(Receipt, Component, ComponentError))
        {
            OutError = ComponentError.IsEmpty()
                ? TEXT("A required composite component failed adapter verification.")
                : ComponentError;
            return false;
        }
    }
    return true;
}

FString SkiPreparation::SerializeSiteContextManifest(const SiteContextManifest& Manifest,
    const bool IncludeContentId)
{
    FString Output;
    SerializeJsonObject(SiteContextManifestObject(Manifest, IncludeContentId), Output);
    return Output;
}

SkiPreparation::SiteContextValidation SkiPreparation::ValidateSiteContextManifest(
    const SiteContextManifest& Manifest, const SkiDomain::TerrainCoreManifest& TerrainCore,
    const std::uint64_t SerializedManifestBytes) noexcept
{
    if (Manifest.SchemaVersion != SiteContextLegacySchema
        && Manifest.SchemaVersion != SiteContextSchema)
        return {SiteContextError::UnsupportedSchema, 0};
    if (SerializedManifestBytes > SiteContextMaxManifestBytes)
        return {SiteContextError::ManifestTooLarge, 0};
    if (!IsCanonicalSha256(Manifest.ContentId)
        || !IsCanonicalSha256(Manifest.TerrainCoreId)
        || Manifest.TerrainCoreId != TerrainCore.ContentId)
        return {SiteContextError::InvalidIdentity, 0};
    if (TerrainCore.SchemaVersion != SkiDomain::TerrainCoreSchema
        || !IsCanonicalSha256(TerrainCore.ContentId)
        || TerrainCore.Width < 2 || TerrainCore.Height < 2
        || !std::isfinite(TerrainCore.DeliveredEastSpacingM)
        || !std::isfinite(TerrainCore.DeliveredNorthSpacingM)
        || TerrainCore.DeliveredEastSpacingM <= 0.0
        || TerrainCore.DeliveredNorthSpacingM <= 0.0)
        return {SiteContextError::InvalidTerrainCore, 0};
    if (!ValidMetadata(Manifest.GeneratorVersion)
        || Manifest.ImageryTilePixels != SiteContextImageryTilePixels
        || Manifest.ImageryEncoding != "jpeg-rgb8-v1"
        || Manifest.ImageryFrame != "terraincore-local-enu-v1"
        || (Manifest.VectorEncoding != SiteContextVectorEncodingV1
            && Manifest.VectorEncoding != SiteContextVectorEncodingV2)
        || Manifest.VectorAssetPath != "vectors/osm-enu-polylines.json")
        return {SiteContextError::InvalidMetadata, 0};

    if (Manifest.SchemaVersion == SiteContextLegacySchema)
    {
        if (HasAnyVectorSourceLineage(Manifest.VectorSource))
            return {SiteContextError::InvalidVectorSourceLineage, 0};
    }
    else
    {
        const SiteContextVectorSourceLineage& Source = Manifest.VectorSource;
        if (!IsCanonicalSha256(Source.SourcePackageContentId)
            || !IsCanonicalSha256(Source.TerrainCoreId)
            || Source.TerrainCoreId != Manifest.TerrainCoreId
            || Source.TerrainCoreId != TerrainCore.ContentId
            || !FiniteMetricBounds(Source.ExtentM)
            || !FiniteMetricBounds(TerrainCore.OuterBounds)
            || !SameMetricBounds(Source.ExtentM, TerrainCore.OuterBounds)
            || !ValidMetadata(Source.Provider, 128)
            || !ValidMetadata(Source.Endpoint, 256)
            || Source.Provider != "openstreetmap-overpass"
            || Source.Endpoint != "https://overpass-api.de/api/interpreter"
            || !IsCanonicalUtcTimestamp(Source.SourceTimestampUtc)
            || !IsCanonicalUtcTimestamp(Source.RetrievedAtUtc)
            || Source.License != "ODbL-1.0"
            || Source.Attribution != "© OpenStreetMap contributors"
            || Source.AttributionUrl != "https://www.openstreetmap.org/copyright")
            return {SiteContextError::InvalidVectorSourceLineage, 0};
    }

    SkiDomain::TerrainCoreTilePlan Plan;
    if (!SkiDomain::PlanTerrainCoreTiles(TerrainCore.Width, TerrainCore.Height, Plan)
        || Plan.Tiles.empty() || Plan.Tiles.size() > SiteContextMaxTiles
        || Manifest.ImageryTiles.size() != Plan.Tiles.size())
        return {SiteContextError::InvalidPyramid, 0};
    if (Manifest.Assets.size() != Manifest.ImageryTiles.size() + 1U
        || Manifest.Assets.size() > SiteContextMaxTiles + 1U)
        return {SiteContextError::MissingVectors, 0};

    std::unordered_set<std::string> Paths;
    std::uint64_t Total = SerializedManifestBytes;
    bool HasVectors = false;
    for (std::size_t Index = 0; Index < Manifest.Assets.size(); ++Index)
    {
        const SiteContextAsset& Asset = Manifest.Assets[Index];
        if (!SkiDomain::IsSafeTerrainCorePath(Asset.Path))
            return {SiteContextError::InvalidAssetPath, Index};
        if (!Paths.insert(LowerAscii(Asset.Path)).second)
            return {SiteContextError::DuplicateAssetPath, Index};
        if (!IsCanonicalSha256(Asset.Sha256))
            return {SiteContextError::InvalidAssetHash, Index};
        if (Asset.Length == 0)
            return {SiteContextError::InvalidAssetLength, Index};
        if (Asset.Length > SiteContextMaxAssetBytes)
            return {SiteContextError::AssetTooLarge, Index};
        if (Total > SiteContextMaxPackageBytes - Asset.Length)
            return {SiteContextError::PackageTooLarge, Index};
        Total += Asset.Length;
        if (Asset.Path == Manifest.VectorAssetPath)
        {
            if (Asset.Type != Manifest.VectorEncoding) return {SiteContextError::InvalidMetadata, Index};
            HasVectors = true;
        }
        else if (Asset.Type != Manifest.ImageryEncoding)
        {
            return {SiteContextError::InvalidMetadata, Index};
        }
    }
    if (!HasVectors) return {SiteContextError::MissingVectors, 0};

    for (std::size_t Index = 0; Index < Plan.Tiles.size(); ++Index)
    {
        const SkiDomain::TerrainCoreTileDescriptor& Expected = Plan.Tiles[Index];
        const SiteContextImageryTile& Tile = Manifest.ImageryTiles[Index];
        const double ExpectedEast = TerrainCore.DeliveredEastSpacingM * Expected.LodFactor;
        const double ExpectedNorth = TerrainCore.DeliveredNorthSpacingM * Expected.LodFactor;
        if (Tile.LodIndex != Expected.LodIndex || Tile.LodFactor != Expected.LodFactor
            || Tile.TileX != Expected.TileX || Tile.TileY != Expected.TileY
            || Tile.PixelWidth != SiteContextImageryTilePixels
            || Tile.PixelHeight != SiteContextImageryTilePixels
            || !NearlyEqual(Tile.EastMetersPerPixel, ExpectedEast)
            || !NearlyEqual(Tile.NorthMetersPerPixel, ExpectedNorth)
            || Tile.AssetPath != Utf8(ImageryPath(Tile)))
            return {SiteContextError::ImageryCoverage, Index};
        const auto Asset = std::find_if(Manifest.Assets.begin(), Manifest.Assets.end(),
            [&](const SiteContextAsset& Candidate) { return Candidate.Path == Tile.AssetPath; });
        if (Asset == Manifest.Assets.end() || Asset->Type != Manifest.ImageryEncoding)
            return {SiteContextError::ImageryCoverage, Index};
    }

    if (Manifest.Attributions.size() < 2 || Manifest.Attributions.size() > 16)
        return {SiteContextError::MissingAttribution, 0};
    bool HasUsGspublicDomain = false;
    bool HasOsmOdbL = false;
    for (std::size_t Index = 0; Index < Manifest.Attributions.size(); ++Index)
    {
        const SiteContextAttribution& Attribution = Manifest.Attributions[Index];
        if (!ValidMetadata(Attribution.Provider) || !ValidMetadata(Attribution.License)
            || !ValidMetadata(Attribution.Text))
            return {SiteContextError::MissingAttribution, Index};
        HasUsGspublicDomain |= Attribution.Provider == "USGS"
            && ContainsAsciiInsensitive(Attribution.License, "public domain");
        HasOsmOdbL |= Attribution.Provider == "OpenStreetMap contributors"
            && ContainsAsciiInsensitive(Attribution.License, "ODbL")
            && ContainsAsciiInsensitive(Attribution.Text, "OpenStreetMap contributors");
    }
    if (!HasUsGspublicDomain || !HasOsmOdbL)
        return {SiteContextError::MissingAttribution, 0};
    return {};
}

bool SkiPreparation::ParseSiteContextManifest(const FString& Json,
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    SiteContextManifest& OutManifest, FString& OutError)
{
    OutManifest = {};
    OutError.Reset();
    const FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0 || static_cast<std::uint64_t>(Encoded.Length()) > SiteContextMaxManifestBytes)
    {
        OutError = TEXT("SiteContext manifest is empty or oversized.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!ParseJsonObject(Json, Root, OutError)) return false;
    SiteContextManifest Value;
    if (!ReadUint32(Root, TEXT("schemaVersion"), Value.SchemaVersion)
        || !ReadString(Root, TEXT("contentId"), Value.ContentId)
        || !ReadString(Root, TEXT("generatorVersion"), Value.GeneratorVersion)
        || !ReadString(Root, TEXT("terrainCoreId"), Value.TerrainCoreId)
        || !ReadUint32(Root, TEXT("imageryTilePixels"), Value.ImageryTilePixels)
        || !ReadString(Root, TEXT("imageryEncoding"), Value.ImageryEncoding)
        || !ReadString(Root, TEXT("imageryFrame"), Value.ImageryFrame)
        || !ReadString(Root, TEXT("vectorEncoding"), Value.VectorEncoding)
        || !ReadString(Root, TEXT("vectorAssetPath"), Value.VectorAssetPath))
    {
        OutError = TEXT("SiteContext manifest fields are missing or invalid.");
        return false;
    }
    if (Value.SchemaVersion == SiteContextSchema)
    {
        const TSharedPtr<FJsonObject>* Source = nullptr;
        const TSharedPtr<FJsonObject>* Extent = nullptr;
        if (!Root->TryGetObjectField(TEXT("vectorSource"), Source) || !Source || !*Source
            || !(*Source)->TryGetObjectField(TEXT("extentM"), Extent) || !Extent || !*Extent
            || !ReadString(*Source, TEXT("sourcePackageContentId"),
                Value.VectorSource.SourcePackageContentId)
            || !ReadString(*Source, TEXT("terrainCoreId"), Value.VectorSource.TerrainCoreId)
            || !ReadMetricBounds(*Extent, Value.VectorSource.ExtentM)
            || !ReadString(*Source, TEXT("provider"), Value.VectorSource.Provider)
            || !ReadString(*Source, TEXT("endpoint"), Value.VectorSource.Endpoint)
            || !ReadString(*Source, TEXT("sourceTimestampUtc"),
                Value.VectorSource.SourceTimestampUtc)
            || !ReadString(*Source, TEXT("retrievedAtUtc"),
                Value.VectorSource.RetrievedAtUtc)
            || !ReadString(*Source, TEXT("license"), Value.VectorSource.License)
            || !ReadString(*Source, TEXT("attribution"), Value.VectorSource.Attribution)
            || !ReadString(*Source, TEXT("attributionUrl"), Value.VectorSource.AttributionUrl))
        {
            OutError = TEXT("SiteContext schema-2 vector-source lineage is missing or malformed.");
            return false;
        }
    }
    else if (Value.SchemaVersion == SiteContextLegacySchema
        && Root->HasField(TEXT("vectorSource")))
    {
        OutError = TEXT("Legacy SiteContext schema 1 cannot claim structured vector-source lineage.");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Attributions = nullptr;
    if (!Root->TryGetArrayField(TEXT("imageryTiles"), Tiles) || !Tiles
        || Tiles->Num() > static_cast<int32>(SiteContextMaxTiles)
        || !Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets
        || Assets->Num() > static_cast<int32>(SiteContextMaxTiles + 1U)
        || !Root->TryGetArrayField(TEXT("attributions"), Attributions) || !Attributions
        || Attributions->Num() > 16)
    {
        OutError = TEXT("SiteContext tile, asset, or attribution arrays are missing or oversized.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Item : *Tiles)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        SiteContextImageryTile Tile;
        if (!Object || !ReadUint8(Object, TEXT("lodIndex"), Tile.LodIndex)
            || !ReadUint32(Object, TEXT("lodFactor"), Tile.LodFactor)
            || !ReadUint32(Object, TEXT("tileX"), Tile.TileX)
            || !ReadUint32(Object, TEXT("tileY"), Tile.TileY)
            || !ReadUint32(Object, TEXT("pixelWidth"), Tile.PixelWidth)
            || !ReadUint32(Object, TEXT("pixelHeight"), Tile.PixelHeight)
            || !Object->TryGetNumberField(TEXT("eastMetersPerPixel"), Tile.EastMetersPerPixel)
            || !Object->TryGetNumberField(TEXT("northMetersPerPixel"), Tile.NorthMetersPerPixel)
            || !ReadString(Object, TEXT("assetPath"), Tile.AssetPath))
        {
            OutError = TEXT("SiteContext imagery tile descriptor is invalid.");
            return false;
        }
        Value.ImageryTiles.push_back(std::move(Tile));
    }
    for (const TSharedPtr<FJsonValue>& Item : *Assets)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        SiteContextAsset Asset;
        if (!Object || !ReadString(Object, TEXT("path"), Asset.Path)
            || !ReadString(Object, TEXT("type"), Asset.Type)
            || !ReadString(Object, TEXT("sha256"), Asset.Sha256)
            || !ReadUint64String(Object, TEXT("length"), Asset.Length))
        {
            OutError = TEXT("SiteContext asset descriptor is invalid.");
            return false;
        }
        Value.Assets.push_back(std::move(Asset));
    }
    for (const TSharedPtr<FJsonValue>& Item : *Attributions)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        SiteContextAttribution Attribution;
        if (!Object || !ReadString(Object, TEXT("provider"), Attribution.Provider)
            || !ReadString(Object, TEXT("license"), Attribution.License)
            || !ReadString(Object, TEXT("text"), Attribution.Text))
        {
            OutError = TEXT("SiteContext attribution is invalid.");
            return false;
        }
        Value.Attributions.push_back(std::move(Attribution));
    }
    const SiteContextValidation Validation = ValidateSiteContextManifest(Value, TerrainCore,
        static_cast<std::uint64_t>(Encoded.Length()));
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("SiteContext validation failed (%d, item %llu)."),
            static_cast<int32>(Validation.Error), static_cast<std::uint64_t>(Validation.Index));
        return false;
    }
    const FString Unsigned = SerializeSiteContextManifest(Value, false);
    const FTCHARToUTF8 UnsignedBytes(*Unsigned);
    const TArrayView<const uint8> UnsignedView(
        reinterpret_cast<const uint8*>(UnsignedBytes.Get()), UnsignedBytes.Length());
    if (!Sha256(UnsignedView).Equals(UTF8_TO_TCHAR(Value.ContentId.c_str()), ESearchCase::CaseSensitive))
    {
        OutError = TEXT("SiteContext content identity does not match its canonical manifest.");
        return false;
    }
    OutManifest = std::move(Value);
    return true;
}
