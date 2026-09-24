#include "SkiPreparation/ElevationCatalog.h"

#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "SkiPreparation/CogPreflight.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "GenericPlatform/GenericPlatformHttp.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr int32 CatalogPageSize = 64;
constexpr int32 CatalogPageLimit = 4;
constexpr int32 MaximumQuantizedBoundsCacheEntries = 16;
constexpr double CatalogBoundsQuantumDegrees = 0.0001;
constexpr uint64 MaximumCatalogResponseBytes = 8ULL * 1024ULL * 1024ULL;
constexpr uint64 MaximumCanonicalSamples = 100020001ULL;

struct FDatasetQuery
{
    SkiDomain::ElevationProduct Product;
    const TCHAR* Dataset;
};

const FDatasetQuery Datasets[] = {
    {SkiDomain::ElevationProduct::S1M, TEXT("Seamless 1-m DEM (S1M)")},
    {SkiDomain::ElevationProduct::Project1m, TEXT("Digital Elevation Model (DEM) 1 meter")},
    {SkiDomain::ElevationProduct::ArcSec13, TEXT("National Elevation Dataset (NED) 1/3 arc-second")},
};

FString ToUtf8String(const std::string& Value)
{
    return FString(UTF8_TO_TCHAR(Value.c_str()));
}

std::string ToUtf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), Converted.Length());
}

bool IsFiniteBounds(const SkiDomain::GeographicBounds& Bounds)
{
    return FMath::IsFinite(Bounds.WestDeg) && FMath::IsFinite(Bounds.SouthDeg)
        && FMath::IsFinite(Bounds.EastDeg) && FMath::IsFinite(Bounds.NorthDeg)
        && Bounds.WestDeg >= -180.0 && Bounds.EastDeg <= 180.0
        && Bounds.SouthDeg >= -90.0 && Bounds.NorthDeg <= 90.0
        && Bounds.WestDeg < Bounds.EastDeg && Bounds.SouthDeg < Bounds.NorthDeg;
}

bool QuantizeBoundsOutward(const SkiDomain::GeographicBounds& Bounds,
    SkiDomain::GeographicBounds& OutBounds, FString& OutCacheKey)
{
    const int64 MinLongitudeTick = -1800000;
    const int64 MaxLongitudeTick = 1800000;
    const int64 MinLatitudeTick = -900000;
    const int64 MaxLatitudeTick = 900000;
    const int64 WestTick = FMath::Clamp<int64>(
        static_cast<int64>(std::floor(Bounds.WestDeg / CatalogBoundsQuantumDegrees)),
        MinLongitudeTick, MaxLongitudeTick);
    const int64 SouthTick = FMath::Clamp<int64>(
        static_cast<int64>(std::floor(Bounds.SouthDeg / CatalogBoundsQuantumDegrees)),
        MinLatitudeTick, MaxLatitudeTick);
    const int64 EastTick = FMath::Clamp<int64>(
        static_cast<int64>(std::ceil(Bounds.EastDeg / CatalogBoundsQuantumDegrees)),
        MinLongitudeTick, MaxLongitudeTick);
    const int64 NorthTick = FMath::Clamp<int64>(
        static_cast<int64>(std::ceil(Bounds.NorthDeg / CatalogBoundsQuantumDegrees)),
        MinLatitudeTick, MaxLatitudeTick);

    double West = static_cast<double>(WestTick) * CatalogBoundsQuantumDegrees;
    double South = static_cast<double>(SouthTick) * CatalogBoundsQuantumDegrees;
    double East = static_cast<double>(EastTick) * CatalogBoundsQuantumDegrees;
    double North = static_cast<double>(NorthTick) * CatalogBoundsQuantumDegrees;
    // Decimal cell edges are not always exactly representable as doubles. Keep the
    // quantized query outward even when multiplication rounds an edge inward by 1 ULP.
    if (West > Bounds.WestDeg) West = std::nextafter(West, -std::numeric_limits<double>::infinity());
    if (South > Bounds.SouthDeg) South = std::nextafter(South, -std::numeric_limits<double>::infinity());
    if (East < Bounds.EastDeg) East = std::nextafter(East, std::numeric_limits<double>::infinity());
    if (North < Bounds.NorthDeg) North = std::nextafter(North, std::numeric_limits<double>::infinity());

    OutBounds = {West, South, East, North};
    OutCacheKey = FString::Printf(TEXT("%lld,%lld,%lld,%lld"),
        static_cast<long long>(WestTick), static_cast<long long>(SouthTick),
        static_cast<long long>(EastTick), static_cast<long long>(NorthTick));
    return IsFiniteBounds(OutBounds);
}

bool FitsEnvelope(const SkiDomain::GeographicBounds& Bounds,
    const double West, const double South, const double East, const double North)
{
    return Bounds.WestDeg >= West && Bounds.SouthDeg >= South
        && Bounds.EastDeg <= East && Bounds.NorthDeg <= North;
}

bool IsSupportedUsGeography(const SkiDomain::GeographicBounds& Bounds, FString& OutRegion)
{
    // These deliberately conservative mainland/island envelopes reject the ocean and
    // non-US territories. TNM results still have to prove actual elevation coverage.
    if (FitsEnvelope(Bounds, -125.0, 24.0, -66.0, 50.0))
    { OutRegion = TEXT("CONUS envelope"); return true; }
    if (FitsEnvelope(Bounds, -180.0, 51.0, -129.0, 72.0))
    { OutRegion = TEXT("Alaska envelope"); return true; }
    if (FitsEnvelope(Bounds, -161.5, 18.5, -154.5, 22.5))
    { OutRegion = TEXT("Hawaii envelope"); return true; }
    OutRegion.Empty();
    return false;
}

FString BuildProductsUrl(const SkiDomain::GeographicBounds& Bounds,
    const FDatasetQuery& Dataset, const int32 Offset)
{
    FString EncodedDataset = FGenericPlatformHttp::UrlEncode(Dataset.Dataset);
    if (Dataset.Product == SkiDomain::ElevationProduct::ArcSec13)
    {
        // The fixed TNM dataset label contains "1/3". A slash is legal in a query value,
        // while the gateway deliberately rejects %2F to prevent encoded path separators.
        EncodedDataset.ReplaceInline(TEXT("%2F"), TEXT("/"), ESearchCase::IgnoreCase);
    }
    return FString::Printf(
        TEXT("https://tnmaccess.nationalmap.gov/api/v1/products?bbox=%.8f,%.8f,%.8f,%.8f&datasets=%s&prodFormats=GeoTIFF&max=%d&offset=%d"),
        Bounds.WestDeg, Bounds.SouthDeg, Bounds.EastDeg, Bounds.NorthDeg,
        *EncodedDataset, CatalogPageSize, Offset);
}

FString JsonValueAsString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    if (!Object.IsValid()) return FString();
    FString Value;
    if (Object->TryGetStringField(Field, Value)) return Value;
    double Number = 0.0;
    if (Object->TryGetNumberField(Field, Number)) return LexToString(Number);
    return FString();
}

FString FirstStringField(const TSharedPtr<FJsonObject>& Object,
    std::initializer_list<const TCHAR*> Fields)
{
    for (const TCHAR* Field : Fields)
    {
        const FString Value = JsonValueAsString(Object, Field);
        if (!Value.IsEmpty()) return Value;
    }
    return FString();
}

bool ParseNonNegativeInteger(const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Field, uint64& OutValue)
{
    OutValue = 0;
    if (!Object.IsValid()) return false;
    double Number = 0.0;
    if (!Object->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number)
        || Number < 0.0 || Number > 9.0e15 || FMath::FloorToDouble(Number) != Number)
        return false;
    OutValue = static_cast<uint64>(Number);
    return true;
}

bool TryPositiveInteger(const TSharedPtr<FJsonObject>& Object,
    std::initializer_list<const TCHAR*> Fields, int32& OutValue)
{
    OutValue = 0;
    for (const TCHAR* Field : Fields)
    {
        uint64 Value = 0;
        if (ParseNonNegativeInteger(Object, Field, Value) && Value > 0
            && Value <= static_cast<uint64>(MAX_int32))
        { OutValue = static_cast<int32>(Value); return true; }
        const FString Text = JsonValueAsString(Object, Field);
        int32 Parsed = 0;
        if (LexTryParseString(Parsed, *Text) && Parsed > 0)
        { OutValue = Parsed; return true; }
    }
    return false;
}

bool IsDateOnly(const FString& Value)
{
    if (Value.Len() < 10) return false;
    const FString Date = Value.Left(10);
    if (Date[4] != TEXT('-') || Date[7] != TEXT('-')) return false;
    for (int32 Index = 0; Index < 10; ++Index)
        if (Index != 4 && Index != 7 && !FChar::IsDigit(Date[Index])) return false;
    if (Value.Len() > 10 && Value[10] != TEXT('T') && Value[10] != TEXT(' ')) return false;
    const int32 Year = FCString::Atoi(*Date.Left(4));
    const int32 Month = FCString::Atoi(*Date.Mid(5, 2));
    const int32 Day = FCString::Atoi(*Date.Right(2));
    if (Year < 1 || Month < 1 || Month > 12 || Day < 1) return false;
    constexpr int32 DaysInMonth[] = {0, 31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31};
    const bool bLeapYear = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    return Day <= DaysInMonth[Month] + (Month == 2 && bLeapYear ? 1 : 0);
}

FString DateOnly(const FString& Value)
{
    return IsDateOnly(Value) ? Value.Left(10) : FString();
}

FString NormalizeToken(const FString& Value)
{
    FString Result;
    Result.Reserve(Value.Len());
    for (const TCHAR C : Value)
        if (FChar::IsAlnum(C)) Result.AppendChar(FChar::ToUpper(C));
    return Result;
}

bool IsNavd88(const FString& Value)
{
    const FString Token = NormalizeToken(Value);
    return Token == TEXT("NAVD88") || Token == TEXT("NORTHAMERICANDATUMOF1988NAVD88")
        || Token == TEXT("NORTHAMERICANDATUM1988NAVD88")
        || Token == TEXT("NORTHAMERICANVERTICALDATUMOF1988NAVD88");
}

FString ReadCrs(const TSharedPtr<FJsonObject>& Item)
{
    FString Crs = FirstStringField(Item,
        {TEXT("horizontalCrs"), TEXT("horizontalCRS"), TEXT("crs"), TEXT("projection")});
    if (!Crs.IsEmpty()) return Crs;

    const TSharedPtr<FJsonObject>* SpatialReference = nullptr;
    if (!Item.IsValid() || !Item->TryGetObjectField(TEXT("spatialReference"), SpatialReference)
        || !SpatialReference || !SpatialReference->IsValid()) return FString();
    for (const TCHAR* Field : {TEXT("latestWkid"), TEXT("wkid")})
    {
        uint64 Code = 0;
        if (ParseNonNegativeInteger(*SpatialReference, Field, Code) && Code > 0)
            return FString::Printf(TEXT("EPSG:%llu"), Code);
    }
    return FString();
}

int32 ProjectUtmZone(const FString& Crs)
{
    const FString Token = NormalizeToken(Crs);
    if (!Token.StartsWith(TEXT("EPSG269")) || Token.Len() != 9) return 0;
    int32 Zone = 0;
    return LexTryParseString(Zone, *Token.Right(2)) && Zone >= 1 && Zone <= 23 ? Zone : 0;
}

bool HorizontalCrsSupported(const SkiDomain::ElevationProduct Product, const FString& Crs)
{
    const FString Token = NormalizeToken(Crs);
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M:
        return Token == TEXT("EPSG6350");
    case SkiDomain::ElevationProduct::Project1m:
        return ProjectUtmZone(Crs) != 0;
    case SkiDomain::ElevationProduct::ArcSec13:
        return Token == TEXT("EPSG4269");
    }
    return false;
}

bool Float32EncodingSupported(const SkiPreparation::ElevationSourceProofs& Proofs)
{
    const FString Sample = NormalizeToken(Proofs.SampleFormat);
    const FString Compression = NormalizeToken(Proofs.Compression);
    return (Sample == TEXT("FLOAT32") || Sample == TEXT("FLOAT") || Sample == TEXT("IEEEFLOATINGPOINT"))
        && Proofs.BitsPerSample == 32
        && (Compression == TEXT("LZW") || Compression == TEXT("DEFLATE"))
        && Proofs.Predictor >= 1 && Proofs.Predictor <= 3;
}

bool CatalogEncodingMatchesCog(const SkiPreparation::ElevationCatalogSource& Source,
    const SkiPreparation::FCogPreflightReport& Cog)
{
    if (!Source.ReportedSampleFormat.IsEmpty())
    {
        const FString CatalogSample = NormalizeToken(Source.ReportedSampleFormat);
        if (CatalogSample != TEXT("FLOAT32") && CatalogSample != TEXT("FLOAT")
            && CatalogSample != TEXT("IEEEFLOATINGPOINT")) return false;
        if (NormalizeToken(Cog.SampleFormat) != TEXT("FLOAT32")) return false;
    }
    if (!Source.ReportedCompression.IsEmpty()
        && NormalizeToken(Source.ReportedCompression) != NormalizeToken(Cog.Compression)) return false;
    if (Source.Proofs.BitsPerSample > 0
        && Source.Proofs.BitsPerSample != static_cast<int32>(Cog.BitsPerSample)) return false;
    if (Source.Proofs.Predictor > 0
        && Source.Proofs.Predictor != static_cast<int32>(Cog.Predictor)) return false;
    return true;
}

void ApplyCogProof(SkiPreparation::ElevationCatalogSource& Source,
    const SkiPreparation::FCogPreflightReport& Cog)
{
    Source.HasVerifiedCogHeader = true;
    Source.CogStatusCode = TEXT("COG_PREFLIGHT_PASSED");
    Source.CogProofSourceId = ToUtf8String(Source.Candidate.SourceId);
    Source.CogProofDownloadUrl = Source.DownloadUrl;
    Source.VerifiedCogObjectBytes = Cog.ObjectBytes;
    Source.Proofs.HorizontalCrs = Cog.HorizontalCrs;
    Source.Proofs.HorizontalCrsOrigin = SkiPreparation::ElevationProofOrigin::CogHeader;
    Source.Proofs.VerticalDatum = Cog.VerticalDatum;
    Source.Proofs.VerticalDatumOrigin = Cog.VerticalDatumOrigin == TEXT("GeoTIFF GeoKey")
        ? SkiPreparation::ElevationProofOrigin::CogHeader
        : SkiPreparation::ElevationProofOrigin::TnmMetadata;
    Source.Proofs.SampleFormat = Cog.SampleFormat;
    Source.Proofs.Compression = Cog.Compression;
    Source.Proofs.BitsPerSample = Cog.BitsPerSample;
    Source.Proofs.Predictor = Cog.Predictor;
    Source.Proofs.EncodingOrigin = SkiPreparation::ElevationProofOrigin::CogHeader;
    Source.ExactObjectBytes = Cog.ObjectBytes;
    Source.HasExactObjectBytes = Cog.ObjectBytes > 0;
    Source.Proofs.ObjectSizeOrigin = SkiPreparation::ElevationProofOrigin::CogHeader;
    Source.CogPreflight = Cog;
}

bool VerifyCatalogSourceCog(SkiPreparation::IAcquisitionTransport& Transport,
    SkiPreparation::ElevationCatalogSource& Source,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation)
{
    if (!Source.HasValidatedDownloadUrl)
    {
        Source.CogStatusCode = TEXT("COG_PREFLIGHT_SOURCE_URL_UNAVAILABLE");
        Source.RefreshResolverEligibility();
        return false;
    }

    SkiPreparation::FCogPreflightMetadata Metadata;
    Metadata.CatalogHorizontalCrs = Source.ReportedHorizontalCrs;
    // CogPreflight accepts only a subset of the NAVD88 names accepted by the catalog
    // proof parser. Canonicalize only catalog values already recognized as NAVD88;
    // unrecognized or conflicting metadata still reaches CogPreflight unchanged.
    Metadata.CatalogVerticalDatum = IsNavd88(Source.ReportedVerticalDatum)
        ? TEXT("NAVD88") : Source.ReportedVerticalDatum;
    SkiPreparation::FCogPreflightReport Cog;
    if (!SkiPreparation::PreflightElevationCog(Transport, Source.DownloadUrl,
            Source.Candidate.Product, Metadata, SkiPreparation::FCogPreflightLimits{},
            Cancellation, Cog))
    {
        Source.CogPreflight = MoveTemp(Cog);
        Source.CogStatusCode = Source.CogPreflight.FailureCode.IsEmpty()
            ? TEXT("COG_PREFLIGHT_FAILED") : Source.CogPreflight.FailureCode;
        Source.RefreshResolverEligibility();
        return false;
    }
    Source.CogPreflight = Cog;
    if (Source.HasExactObjectBytes && Source.ExactObjectBytes != Cog.ObjectBytes)
    {
        Source.CogStatusCode = TEXT("CATALOG_COG_OBJECT_SIZE_MISMATCH");
        Source.CogPreflight.bPassed = false;
        Source.CogPreflight.FailureCode = Source.CogStatusCode;
        Source.CogPreflight.FailureDetail = TEXT("TNM sizeInBytes disagrees with the ETag-pinned COG range total.");
        Source.RefreshResolverEligibility();
        return false;
    }
    if (!CatalogEncodingMatchesCog(Source, Cog))
    {
        Source.CogStatusCode = TEXT("CATALOG_COG_ENCODING_MISMATCH");
        Source.CogPreflight.bPassed = false;
        Source.CogPreflight.FailureCode = Source.CogStatusCode;
        Source.CogPreflight.FailureDetail = TEXT("Catalog sample encoding facts disagree with the GeoTIFF header.");
        Source.RefreshResolverEligibility();
        return false;
    }

    ApplyCogProof(Source, Cog);
    Source.RefreshResolverEligibility();
    return Source.HasVerifiedCogHeader;
}

FString SourceIdFromItem(const TSharedPtr<FJsonObject>& Item)
{
    return FirstStringField(Item,
        {TEXT("sourceId"), TEXT("productId"), TEXT("sourceOriginId"), TEXT("id")});
}

bool ParseCatalogPage(const TArray<uint8>& Bytes, TArray<TSharedPtr<FJsonObject>>& OutItems,
    uint64& OutTotal, FString& OutError)
{
    OutItems.Reset();
    OutTotal = 0;
    OutError.Empty();
    if (Bytes.IsEmpty()) { OutError = TEXT("CATALOG_EMPTY_RESPONSE"); return false; }

    FString Json;
    FFileHelper::BufferToString(Json, Bytes.GetData(), Bytes.Num());
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    { OutError = TEXT("CATALOG_MALFORMED_JSON"); return false; }

    if (!ParseNonNegativeInteger(Root, TEXT("total"), OutTotal))
    { OutError = TEXT("CATALOG_TOTAL_MISSING_OR_INVALID"); return false; }
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Root->TryGetArrayField(TEXT("items"), Values) || !Values)
    { OutError = TEXT("CATALOG_ITEMS_MISSING"); return false; }
    if (OutTotal > static_cast<uint64>(MAX_int32) || Values->Num() > CatalogPageSize)
    { OutError = TEXT("CATALOG_PAGE_LIMIT_INVALID"); return false; }

    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        const TSharedPtr<FJsonObject>* Item = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Item) || !Item || !Item->IsValid()
            || SourceIdFromItem(*Item).IsEmpty())
        { OutError = TEXT("CATALOG_ITEM_INVALID"); return false; }
        OutItems.Add(*Item);
    }
    if (static_cast<uint64>(OutItems.Num()) > OutTotal)
    { OutError = TEXT("CATALOG_TOTAL_CONTRADICTS_ITEMS"); return false; }
    if (OutTotal > 0 && OutItems.IsEmpty())
    { OutError = TEXT("CATALOG_TOTAL_WITH_EMPTY_PAGE"); return false; }
    return true;
}

bool GetJsonResponse(SkiPreparation::IAcquisitionTransport& Transport, const FString& Url,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
    SkiPreparation::HttpAcquisitionResult& OutResult, FString& OutError)
{
    FString UrlFailure;
    if (!SkiPreparation::SkiNetGateway::ValidateUrl(Url, UrlFailure))
    { OutError = TEXT("CATALOG_URL_REJECTED"); return false; }
    if (Cancellation->IsCancelled()) { OutError = TEXT("PREPARATION_CANCELLED"); return false; }

    SkiPreparation::HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.MaximumResponseBytes = MaximumCatalogResponseBytes;
    Request.ActivityTimeoutSeconds = 30.0F;
    Request.TotalTimeoutSeconds = 45.0F;
    OutResult = Transport.Get(Request, Cancellation);
    if (Cancellation->IsCancelled())
    { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
    if (!OutResult.Ok())
    {
        OutError = OutResult.RequestStatus.IsEmpty()
            ? FString::Printf(TEXT("CATALOG_TRANSPORT_%s"),
                *FString(SkiPreparation::TransportFailureReasonName(OutResult.FailureReason)))
            : TEXT("CATALOG_TRANSPORT_") + OutResult.RequestStatus;
        return false;
    }
    if (OutResult.Bytes.Num() > static_cast<int32>(MaximumCatalogResponseBytes))
    { OutError = TEXT("CATALOG_RESPONSE_TOO_LARGE"); return false; }
    const FString ContentType = OutResult.ContentType.ToLower();
    if (!ContentType.Contains(TEXT("json")))
    { OutError = TEXT("CATALOG_CONTENT_TYPE_INVALID"); return false; }
    if (Cancellation->IsCancelled())
    { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
    return true;
}

bool ParseDatasetRecords(const FDatasetQuery& Dataset,
    const TSharedPtr<FJsonObject>& Item,
    SkiPreparation::ElevationCatalogSource& OutSource)
{
    OutSource = {};
    const FString RawId = SourceIdFromItem(Item);
    const FString SourceId = FString(Dataset.Product == SkiDomain::ElevationProduct::S1M ? TEXT("S1M:")
        : Dataset.Product == SkiDomain::ElevationProduct::Project1m ? TEXT("PROJECT1M:") : TEXT("ARCSEC13:")) + RawId;
    OutSource.Title = FirstStringField(Item, {TEXT("title"), TEXT("name"), TEXT("productName")});
    OutSource.DownloadUrl = FirstStringField(Item, {TEXT("downloadURL"), TEXT("downloadUrl")});
    OutSource.MetadataUrl = FirstStringField(Item, {TEXT("metaUrl"), TEXT("metadataUrl")});
    OutSource.ReportedHorizontalCrs = ReadCrs(Item);
    OutSource.ReportedVerticalDatum = FirstStringField(Item,
        {TEXT("verticalDatum"), TEXT("vertical_datum"), TEXT("vdatum")});
    OutSource.ReportedSampleFormat = FirstStringField(Item,
        {TEXT("sampleFormat"), TEXT("sample_format"), TEXT("dataType"), TEXT("pixelType")});
    OutSource.ReportedCompression = FirstStringField(Item,
        {TEXT("compression"), TEXT("compressionType"), TEXT("compression_type")});

    OutSource.Candidate.Product = Dataset.Product;
    OutSource.Candidate.SourceId = ToUtf8(SourceId);
    OutSource.Candidate.QualityLevel = 0;
    int32 QualityLevel = 0;
    if (TryPositiveInteger(Item, {TEXT("qualityLevel"), TEXT("quality_level"), TEXT("ql")}, QualityLevel)
        && QualityLevel <= 4)
        OutSource.Candidate.QualityLevel = static_cast<uint8>(QualityLevel);
    OutSource.Candidate.CollectionEndDate = ToUtf8(DateOnly(FirstStringField(Item,
        {TEXT("collectionEndDate"), TEXT("acquisitionEndDate"), TEXT("endDate")})));
    OutSource.Candidate.PublicationDate = ToUtf8(DateOnly(FirstStringField(Item,
        {TEXT("publicationDate"), TEXT("publishedDate")})));

    OutSource.Proofs.HorizontalCrs = OutSource.ReportedHorizontalCrs;
    OutSource.Proofs.VerticalDatum = OutSource.ReportedVerticalDatum;
    OutSource.Proofs.SampleFormat = OutSource.ReportedSampleFormat;
    OutSource.Proofs.Compression = OutSource.ReportedCompression;
    OutSource.Proofs.HorizontalCrsOrigin = OutSource.ReportedHorizontalCrs.IsEmpty()
        ? SkiPreparation::ElevationProofOrigin::None : SkiPreparation::ElevationProofOrigin::TnmMetadata;
    OutSource.Proofs.VerticalDatumOrigin = OutSource.ReportedVerticalDatum.IsEmpty()
        ? SkiPreparation::ElevationProofOrigin::None : SkiPreparation::ElevationProofOrigin::TnmMetadata;
    OutSource.Proofs.EncodingOrigin = OutSource.ReportedSampleFormat.IsEmpty()
        || OutSource.ReportedCompression.IsEmpty()
        ? SkiPreparation::ElevationProofOrigin::None : SkiPreparation::ElevationProofOrigin::TnmMetadata;
    TryPositiveInteger(Item, {TEXT("bitsPerSample"), TEXT("bits_per_sample"), TEXT("bits")},
        OutSource.Proofs.BitsPerSample);
    TryPositiveInteger(Item, {TEXT("predictor"), TEXT("tiffPredictor")}, OutSource.Proofs.Predictor);

    uint64 Size = 0;
    if (ParseNonNegativeInteger(Item, TEXT("sizeInBytes"), Size) && Size > 0)
    {
        OutSource.ExactObjectBytes = Size;
        OutSource.HasExactObjectBytes = true;
        OutSource.Proofs.ObjectSizeOrigin = SkiPreparation::ElevationProofOrigin::TnmMetadata;
    }

    if (!OutSource.DownloadUrl.IsEmpty())
    {
        FString UrlFailure;
        OutSource.HasValidatedDownloadUrl = SkiPreparation::SkiNetGateway::ValidateUrl(
            OutSource.DownloadUrl, UrlFailure);
        if (!OutSource.HasValidatedDownloadUrl)
            OutSource.DownloadUrl.Empty();
    }
    OutSource.RefreshResolverEligibility();
    return !RawId.IsEmpty();
}

bool QueryDataset(SkiPreparation::IAcquisitionTransport& Transport,
    const SkiDomain::GeographicBounds& Bounds, const FDatasetQuery& Dataset,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
    TArray<SkiPreparation::ElevationCatalogSource>& OutSources, FString& OutError)
{
    OutSources.Reset();
    uint64 ExpectedTotal = TNumericLimits<uint64>::Max();
    TSet<FString> SeenIds;
    for (int32 Page = 0; Page < CatalogPageLimit; ++Page)
    {
        if (Cancellation->IsCancelled())
        { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
        const int32 Offset = Page * CatalogPageSize;
        const FString Url = BuildProductsUrl(Bounds, Dataset, Offset);
        SkiPreparation::HttpAcquisitionResult Response;
        if (!GetJsonResponse(Transport, Url, Cancellation, Response, OutError)) return false;

        TArray<TSharedPtr<FJsonObject>> Items;
        uint64 Total = 0;
        if (!ParseCatalogPage(Response.Bytes, Items, Total, OutError)) return false;
        if (Cancellation->IsCancelled())
        { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
        if (ExpectedTotal == TNumericLimits<uint64>::Max()) ExpectedTotal = Total;
        else if (ExpectedTotal != Total)
        { OutError = TEXT("CATALOG_TOTAL_CHANGED_DURING_QUERY"); return false; }

        if (static_cast<uint64>(Offset) >= ExpectedTotal)
        {
            if (!Items.IsEmpty()) { OutError = TEXT("CATALOG_UNEXPECTED_EXTRA_PAGE"); return false; }
            if (Cancellation->IsCancelled())
            { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
            return true;
        }
        const uint64 Remaining = ExpectedTotal - static_cast<uint64>(Offset);
        const int32 ExpectedPageCount = static_cast<int32>(FMath::Min<uint64>(Remaining, CatalogPageSize));
        if (Items.Num() != ExpectedPageCount)
        { OutError = TEXT("CATALOG_PAGE_INCOMPLETE"); return false; }

        for (const TSharedPtr<FJsonObject>& Item : Items)
        {
            SkiPreparation::ElevationCatalogSource Source;
            if (!ParseDatasetRecords(Dataset, Item, Source))
            { OutError = TEXT("CATALOG_ITEM_NORMALIZATION_FAILED"); return false; }
            Source.CatalogQueryBounds = Bounds;
            Source.CoverageStatusCode = TEXT("TNM_BBOX_QUERY_RETURNED_CANDIDATE");
            const FString Id = ToUtf8String(Source.Candidate.SourceId);
            if (SeenIds.Contains(Id))
            { OutError = TEXT("CATALOG_DUPLICATE_SOURCE_ID"); return false; }
            SeenIds.Add(Id);
            OutSources.Add(MoveTemp(Source));
        }
        if (static_cast<uint64>(Offset + Items.Num()) == ExpectedTotal)
        {
            if (Cancellation->IsCancelled())
            { OutError = TEXT("PREPARATION_CANCELLED"); return false; }
            return true;
        }
    }
    OutError = TEXT("CATALOG_RESULT_LIMIT_EXCEEDED");
    return false;
}

bool MakeScratchEstimate(const SkiDomain::GeographicBounds& Bounds,
    SkiPreparation::ElevationStorageEstimate& OutEstimate)
{
    const double CenterLat = (Bounds.SouthDeg + Bounds.NorthDeg) * 0.5;
    const double WidthMeters = (Bounds.EastDeg - Bounds.WestDeg) * 111320.0
        * FMath::Cos(FMath::DegreesToRadians(CenterLat));
    const double HeightMeters = (Bounds.NorthDeg - Bounds.SouthDeg) * 110574.0;
    if (!FMath::IsFinite(WidthMeters) || !FMath::IsFinite(HeightMeters)
        || WidthMeters <= 0.0 || HeightMeters <= 0.0) return false;
    const double WidthSamples = FMath::CeilToDouble(WidthMeters) + 1.0;
    const double HeightSamples = FMath::CeilToDouble(HeightMeters) + 1.0;
    if (WidthSamples > static_cast<double>(MaximumCanonicalSamples)
        || HeightSamples > static_cast<double>(MaximumCanonicalSamples)) return false;
    const double Samples = WidthSamples * HeightSamples;
    if (!FMath::IsFinite(Samples) || Samples <= 0.0
        || Samples > static_cast<double>(MaximumCanonicalSamples)) return false;
    OutEstimate.Bytes = static_cast<uint64>(Samples) * 5ULL;
    OutEstimate.Certainty = SkiPreparation::StorageSizeCertainty::Estimated;
    OutEstimate.Basis = TEXT("Approximate 1 m LOD0 grid: float32 height plus one validity byte per sample; projection and tile overhead excluded.");
    return true;
}

bool SumExactResolvedObjectBytes(const TArray<SkiPreparation::ElevationCatalogSource>& Sources,
    SkiPreparation::ElevationStorageEstimate& OutEstimate)
{
    if (Sources.IsEmpty()) return false;
    uint64 Total = 0;
    for (const SkiPreparation::ElevationCatalogSource& Source : Sources)
    {
        if (!Source.HasExactObjectBytes || Source.ExactObjectBytes > TNumericLimits<uint64>::Max() - Total)
            return false;
        Total += Source.ExactObjectBytes;
    }
    OutEstimate.Bytes = Total;
    OutEstimate.Certainty = SkiPreparation::StorageSizeCertainty::Exact;
    OutEstimate.Basis = TEXT("Sum of exact object sizes cross-checked against ETag-pinned COG range totals for resolver-eligible fallback candidates. This is not a planned download total.");
    return true;
}

void SetFailure(SkiPreparation::TerrainAvailabilityReport& Report,
    const FString& Code, const FString& Detail)
{
    Report.Status = SkiPreparation::ElevationCatalogStatus::CatalogFailure;
    Report.FailureCode = Code;
    Report.FailureDetail = Detail;
    Report.DownloadEnabled = false;
}
}

void SkiPreparation::ElevationCatalogSource::RefreshResolverEligibility()
{
    const bool bCogProofBoundToSource = HasVerifiedCogHeader
        && CogPreflight.bPassed
        && CogProofSourceId == ToUtf8String(Candidate.SourceId)
        && CogProofDownloadUrl == DownloadUrl
        && VerifiedCogObjectBytes > 0
        && VerifiedCogObjectBytes == CogPreflight.ObjectBytes
        && HasExactObjectBytes && ExactObjectBytes == VerifiedCogObjectBytes;
    Candidate.SupportedHorizontalCrs = bCogProofBoundToSource
        && Proofs.HorizontalCrsOrigin != ElevationProofOrigin::None
        && HorizontalCrsSupported(Candidate.Product, Proofs.HorizontalCrs);
    Candidate.Navd88Proven = bCogProofBoundToSource
        && Proofs.VerticalDatumOrigin != ElevationProofOrigin::None
        && IsNavd88(Proofs.VerticalDatum);
    Candidate.SupportedEncoding = bCogProofBoundToSource
        && Proofs.EncodingOrigin != ElevationProofOrigin::None
        && Float32EncodingSupported(Proofs);

    EligibilityReasonCode.Empty();
    if (!HasValidatedDownloadUrl) EligibilityReasonCode = TEXT("SOURCE_DOWNLOAD_URL_UNAVAILABLE");
    else if (!bCogProofBoundToSource)
    {
        if (CogStatusCode == TEXT("COG_HEADERS_NOT_READ"))
            EligibilityReasonCode = TEXT("COG_PREFLIGHT_REQUIRED");
        else EligibilityReasonCode = CogStatusCode;
    }
    else if (!Candidate.SupportedHorizontalCrs) EligibilityReasonCode = TEXT("SOURCE_CRS_UNPROVEN");
    else if (!Candidate.Navd88Proven) EligibilityReasonCode = TEXT("VERTICAL_DATUM_UNPROVEN");
    else if (!Candidate.SupportedEncoding) EligibilityReasonCode = TEXT("SOURCE_ENCODING_UNPROVEN");
}

SkiPreparation::ElevationCatalog::ElevationCatalog(IAcquisitionTransport& InTransport)
    : Transport(InTransport)
{
}

bool SkiPreparation::ElevationCatalog::Preflight(
    const SkiDomain::GeographicBounds& Bounds,
    const TSharedRef<Cancellation>& Cancellation,
    TerrainAvailabilityReport& OutReport) const
{
    OutReport = {};
    if (Cancellation->IsCancelled())
    {
        SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
        return false;
    }
    if (!IsFiniteBounds(Bounds))
    {
        SetFailure(OutReport, TEXT("INVALID_BOUNDS"), TEXT("Bounds must be finite WGS84 west/south/east/north coordinates."));
        return false;
    }
    SkiDomain::GeographicBounds QuantizedBounds;
    FString CacheKey;
    if (!QuantizeBoundsOutward(Bounds, QuantizedBounds, CacheKey))
    {
        SetFailure(OutReport, TEXT("INVALID_BOUNDS"), TEXT("Bounds could not be quantized to the catalog cache grid."));
        return false;
    }

    bool bHasCachedReport = false;
    {
        FScopeLock Lock(&CacheMutex);
        if (const TerrainAvailabilityReport* Cached = QuantizedBoundsCache.Find(CacheKey))
        {
            OutReport = *Cached;
            bHasCachedReport = true;
        }
    }
    if (bHasCachedReport)
    {
        if (Cancellation->IsCancelled())
        {
            OutReport = {};
            SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
            return false;
        }
        return true;
    }

    const auto CacheCompletedReport = [&]()
    {
        if (Cancellation->IsCancelled()) return false;
        FScopeLock Lock(&CacheMutex);
        if (Cancellation->IsCancelled()) return false;
        if (!QuantizedBoundsCache.Contains(CacheKey))
        {
            if (QuantizedBoundsCacheOrder.Num() >= MaximumQuantizedBoundsCacheEntries)
            {
                QuantizedBoundsCache.Remove(QuantizedBoundsCacheOrder[0]);
                QuantizedBoundsCacheOrder.RemoveAt(0);
            }
            QuantizedBoundsCacheOrder.Add(CacheKey);
        }
        QuantizedBoundsCache.Add(CacheKey, OutReport);
        return true;
    };

    OutReport = {};
    if (Cancellation->IsCancelled())
    {
        SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
        return false;
    }
    OutReport.IsSupportedGeography = IsSupportedUsGeography(QuantizedBounds, OutReport.GeographyRegion);
    if (!OutReport.IsSupportedGeography)
    {
        OutReport.Status = ElevationCatalogStatus::UnsupportedGeography;
        OutReport.FailureCode = TEXT("GEOGRAPHY_OUTSIDE_SUPPORTED_US_ENVELOPES");
        OutReport.FailureDetail = TEXT("This site is outside the supported conservative USGS 3DEP geography envelopes.");
        if (!CacheCompletedReport())
        {
            OutReport = {};
            SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
            return false;
        }
        return true;
    }
    if (!MakeScratchEstimate(QuantizedBounds, OutReport.CanonicalScratchBytes))
    {
        SetFailure(OutReport, TEXT("SITE_SAMPLE_LIMIT_EXCEEDED"),
            TEXT("The approximate 1 m canonical grid exceeds the TerrainCore sample limit."));
        if (!CacheCompletedReport())
        {
            OutReport = {};
            SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
            return false;
        }
        return true;
    }

    for (const FDatasetQuery& Dataset : Datasets)
    {
        TArray<ElevationCatalogSource> TierSources;
        FString Error;
        if (!QueryDataset(Transport, QuantizedBounds, Dataset, Cancellation, TierSources, Error))
        {
            SetFailure(OutReport, Error, TEXT("TNM Access returned an unusable or incomplete catalog response."));
            return false;
        }
        if (Cancellation->IsCancelled())
        {
            SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
            return false;
        }
        OutReport.Sources.Append(MoveTemp(TierSources));
    }
    if (Cancellation->IsCancelled())
    {
        SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
        return false;
    }
    OutReport.CatalogComplete = true;
    OutReport.HasElevationCoverage = !OutReport.Sources.IsEmpty();
    OutReport.CoverageStatusCode = OutReport.HasElevationCoverage
        ? TEXT("TNM_BBOX_QUERY_RETURNED_PRODUCTS_RASTER_EXTENT_UNCHECKED")
        : TEXT("NO_USGS_ELEVATION_COVERAGE");

    bool bAnyCogCandidate = false;
    bool bAnyCogFailure = false;
    FString FirstCogFailure;
    for (ElevationCatalogSource& Source : OutReport.Sources)
    {
        if (Cancellation->IsCancelled())
        {
            SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation COG preflight was cancelled."));
            return false;
        }
        if (!Source.HasValidatedDownloadUrl)
        {
            Source.CogStatusCode = TEXT("COG_PREFLIGHT_SOURCE_URL_UNAVAILABLE");
            Source.RefreshResolverEligibility();
            bAnyCogFailure = true;
            if (FirstCogFailure.IsEmpty()) FirstCogFailure = Source.CogStatusCode;
            continue;
        }
        bAnyCogCandidate = true;
        if (!VerifyCatalogSourceCog(Transport, Source, Cancellation))
        {
            if (Cancellation->IsCancelled())
            {
                SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation COG preflight was cancelled."));
                return false;
            }
            bAnyCogFailure = true;
            if (FirstCogFailure.IsEmpty()) FirstCogFailure = Source.CogStatusCode;
        }
    }
    if (Cancellation->IsCancelled())
    {
        SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation COG preflight was cancelled."));
        return false;
    }
    OutReport.CogStatusCode = !bAnyCogCandidate ? TEXT("COG_PREFLIGHT_NO_SOURCE_URL")
        : bAnyCogFailure ? TEXT("COG_PREFLIGHT_COMPLETED_WITH_REJECTIONS")
        : TEXT("COG_PREFLIGHT_PASSED");

    std::vector<SkiDomain::ElevationSourceCandidate> Candidates;
    Candidates.reserve(static_cast<std::size_t>(OutReport.Sources.Num()));
    TMap<FString, int32> SourceIndices;
    for (int32 Index = 0; Index < OutReport.Sources.Num(); ++Index)
    {
        const ElevationCatalogSource& Source = OutReport.Sources[Index];
        if (!Source.HasVerifiedCogHeader) continue;
        Candidates.push_back(Source.Candidate);
        SourceIndices.Add(ToUtf8String(Source.Candidate.SourceId), Index);
    }
    std::vector<SkiDomain::ElevationSourceCandidate> Resolved;
    const bool bResolved = SkiDomain::ResolveElevationSources(Candidates, Resolved);
    if (bResolved)
    {
        for (const SkiDomain::ElevationSourceCandidate& Candidate : Resolved)
        {
            const int32* Index = SourceIndices.Find(ToUtf8String(Candidate.SourceId));
            if (Index) OutReport.ResolvedSources.Add(OutReport.Sources[*Index]);
        }
    }
    SumExactResolvedObjectBytes(OutReport.ResolvedSources, OutReport.ResolvedCandidateObjectBytes);
    OutReport.HasVerifiedCogHeaders = !OutReport.ResolvedSources.IsEmpty();
    for (const ElevationCatalogSource& Source : OutReport.ResolvedSources)
        OutReport.HasVerifiedCogHeaders = OutReport.HasVerifiedCogHeaders && Source.HasVerifiedCogHeader;
    OutReport.DownloadEnabled = OutReport.HasElevationCoverage && !OutReport.ResolvedSources.IsEmpty()
        && OutReport.HasVerifiedCogHeaders && OutReport.HasVerifiedSiteCoverage
        && OutReport.HasCompleteSourceLineage && OutReport.HasQualityReport;
    if (!OutReport.HasElevationCoverage)
    {
        OutReport.Status = ElevationCatalogStatus::NoCoverage;
        OutReport.FailureCode = TEXT("NO_USGS_ELEVATION_COVERAGE");
        OutReport.FailureDetail = TEXT("TNM Access returned no 3DEP elevation products for this site.");
    }
    else if (OutReport.ResolvedSources.IsEmpty())
    {
        OutReport.Status = ElevationCatalogStatus::NoDatumProvenSource;
        OutReport.FailureCode = bAnyCogCandidate ? TEXT("NO_SOURCE_PASSED_COG_AND_REQUIRED_METADATA_PROOFS")
            : TEXT("NO_SOURCE_WITH_REQUIRED_METADATA_PROOFS");
        OutReport.FailureDetail = bAnyCogCandidate
            ? FString::Printf(TEXT("Coverage candidates exist, but none passed identity-bound COG, size, CRS, NAVD88, and float32 checks. First COG reason: %s."), *FirstCogFailure)
            : TEXT("Coverage exists, but no source has an allowed download URL and required datum-proven metadata.");
    }
    else if (!OutReport.DownloadEnabled)
    {
        OutReport.Status = ElevationCatalogStatus::CatalogReady;
        OutReport.FailureCode = OutReport.HasVerifiedCogHeaders
            ? TEXT("ELEVATION_SITE_COVERAGE_LINEAGE_AND_QUALITY_PROOF_REQUIRED")
            : TEXT("ELEVATION_COG_AND_LINEAGE_PROOF_REQUIRED");
        OutReport.FailureDetail = OutReport.HasVerifiedCogHeaders
            ? TEXT("COG headers passed for the resolved fallback chain, but TNM bbox results do not prove full raster coverage; raw GeoPackage/XML lineage and sampler quality are also still required.")
            : TEXT("Catalog candidates remain preview-only until the resolved COG chain, source lineage, site coverage, and sampler quality are proven.");
    }
    else
    {
        OutReport.Status = ElevationCatalogStatus::Ready;
        OutReport.FailureCode.Empty();
        OutReport.FailureDetail.Empty();
    }
    if (!CacheCompletedReport())
    {
        OutReport = {};
        SetFailure(OutReport, TEXT("PREPARATION_CANCELLED"), TEXT("Elevation catalog preflight was cancelled."));
        return false;
    }
    return true;
}

bool SkiPreparation::ElevationCatalog::TryBuildQualityReport(
    const SkiDomain::TerrainProvenanceCounts& Counts,
    SkiDomain::TerrainQualityReport& OutReport) noexcept
{
    return SkiDomain::TrySummarizeTerrainQuality(Counts, OutReport);
}
