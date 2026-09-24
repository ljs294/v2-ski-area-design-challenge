#include "SkiPreparation/CogPreflight.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/WorldCoverCogDecoder.h"
#include "tiffio.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint16 GeoKeyModelType = 1024;
constexpr uint16 GeoKeyRasterType = 1025;
constexpr uint16 GeoKeyGeographicType = 2048;
constexpr uint16 GeoKeyProjectedType = 3072;
constexpr uint16 GeoKeyVerticalCSType = 4096;
constexpr uint16 GeoKeyVerticalDatum = 4098;
constexpr uint16 RasterPixelIsArea = 1;
constexpr uint16 ModelProjected = 1;
constexpr uint16 ModelGeographic = 2;
constexpr uint16 Navd88VerticalCrs = 5703;
constexpr uint16 Navd88VerticalDatum = 5103;
constexpr uint64 InitialProbeBytes = 16;

void Fail(SkiPreparation::FCogPreflightReport& Report, const TCHAR* Code, const TCHAR* Detail)
{
    Report.bPassed = false;
    Report.FailureCode = Code;
    Report.FailureDetail = Detail;
}

bool IsSafeStrongETag(const FString& Tag)
{
    if (Tag.Len() < 2 || Tag.Len() > 128 || Tag[0] != TEXT('"') || Tag[Tag.Len() - 1] != TEXT('"'))
        return false;
    for (int32 Index = 1; Index < Tag.Len() - 1; ++Index)
    {
        const TCHAR C = Tag[Index];
        if (C < 0x21 || C > 0x7e || C == TEXT('"') || C == TEXT(':') || C == TEXT('\\')) return false;
    }
    return true;
}

FString NormalizeToken(const FString& Text)
{
    FString Result;
    Result.Reserve(Text.Len());
    for (const TCHAR Character : Text)
        if (FChar::IsAlnum(Character)) Result.AppendChar(FChar::ToUpper(Character));
    return Result;
}

bool IsNavd88(const FString& Text)
{
    const FString Token = NormalizeToken(Text);
    return Token == TEXT("NAVD88") || Token == TEXT("NORTHAMERICANDATUMOF1988NAVD88")
        || Token == TEXT("NORTHAMERICANDATUM1988NAVD88") || Token == TEXT("EPSG5703");
}

bool TryGetGeoKey(TIFF* Image, const uint16 KeyId, bool& bOutPresent, uint16& OutValue)
{
    bOutPresent = false;
    OutValue = 0;
    uint32 Count = 0;
    uint16* Values = nullptr;
    if (!TIFFGetField(Image, GeoKeyDirectoryTag, &Count, &Values) || !Values || Count < 4
        || Values[3] > (Count - 4) / 4)
        return false;

    bool bFound = false;
    for (uint32 Index = 0; Index < Values[3]; ++Index)
    {
        const uint16* Entry = Values + 4 + Index * 4;
        if (Entry[0] != KeyId) continue;
        if (bFound || Entry[1] != 0 || Entry[2] != 1) return false;
        bFound = true;
        OutValue = Entry[3];
    }
    bOutPresent = bFound;
    return true;
}

bool ParseCatalogEpsg(const FString& Text, uint32& OutCode)
{
    OutCode = 0;
    const FString Upper = Text.ToUpper();
    const int32 EpsgAt = Upper.Find(TEXT("EPSG"));
    if (EpsgAt == INDEX_NONE) return false;
    int32 Position = EpsgAt + 4;
    while (Position < Upper.Len() && (Upper[Position] == TEXT(':') || FChar::IsWhitespace(Upper[Position])))
        ++Position;
    const int32 FirstDigit = Position;
    while (Position < Upper.Len() && FChar::IsDigit(Upper[Position])) ++Position;
    if (Position == FirstDigit) return false;
    const FString Digits = Upper.Mid(FirstDigit, Position - FirstDigit);
    if (Position < Upper.Len() && FChar::IsAlnum(Upper[Position])) return false;
    return LexTryParseString(OutCode, *Digits);
}

bool ParseNoData(const char* Text, const uint32 Count, double& OutValue, FString& OutText)
{
    OutValue = 0.0;
    OutText.Reset();
    if (!Text || Count == 0 || Count > 129) return false;
    uint32 Length = 0;
    while (Length < Count && Text[Length] != '\0') ++Length;
    if (Length == 0 || Length == Count) return false;
    const std::string Encoded(Text, Text + Length);
    errno = 0;
    char* End = nullptr;
    const double Parsed = std::strtod(Encoded.c_str(), &End);
    if (errno != 0 || End == Encoded.c_str() || !std::isfinite(Parsed)) return false;
    while (*End && std::isspace(static_cast<unsigned char>(*End))) ++End;
    if (*End != '\0') return false;
    OutValue = Parsed;
    OutText = UTF8_TO_TCHAR(Encoded.c_str());
    return true;
}

class FBoundedCogRangeSource final : public SkiPreparation::ICogByteSource
{
public:
    FBoundedCogRangeSource(SkiPreparation::IAcquisitionTransport& InTransport, const FString& InUrl,
        const SkiPreparation::FCogPreflightLimits& InLimits,
        const TSharedRef<SkiPreparation::Cancellation>& InCancellation)
        : Transport(InTransport), Url(InUrl), Limits(InLimits), Cancellation(InCancellation) {}

    bool Initialize(FString& OutError)
    {
        ObjectBytes = 0;
        PinnedETag.Reset();
        Cache.Reset();
        CacheOrder.Reset();
        CachedBytes = 0;
        TransferredBytes = 0;
        RequestCount = 0;
        if (Limits.MaxObjectBytes < InitialProbeBytes || Limits.MaxObjectBytes > MAX_uint32
            || Limits.MaxTransferredBytes < InitialProbeBytes || Limits.MaxRequests < 2
            || Limits.MaxRangeBytes < Limits.RangeBlockBytes || Limits.MaxRangeBytes < InitialProbeBytes
            || Limits.RangeBlockBytes < InitialProbeBytes || Limits.RangeBlockBytes > MAX_int32
            || Limits.MaxRangeBytes > MAX_int32 || Limits.MaxCachedBytes < Limits.RangeBlockBytes
            || Limits.MaxCachedBytes > MAX_int32 || Limits.MaxDimension == 0
            || Limits.MaxDirectories == 0 || Limits.MaxDirectories > MAX_int32
            || Limits.MaxTotalTiles == 0 || Limits.MaxTilesPerDirectory == 0)
        {
            OutError = TEXT("COG_PREFLIGHT_LIMITS_INVALID");
            return false;
        }
        FString UrlReason;
        if (!SkiPreparation::SkiNetGateway::ValidateUrl(Url, UrlReason))
        {
            OutError = TEXT("COG_PREFLIGHT_URL_REJECTED");
            return false;
        }

        TArray<uint8> InitialBytes;
        uint64 TotalBytes = 0;
        if (!Fetch(0, InitialProbeBytes, true, InitialBytes, TotalBytes, OutError)) return false;
        if (TotalBytes < InitialProbeBytes || TotalBytes > Limits.MaxObjectBytes)
        {
            OutError = TEXT("COG_PREFLIGHT_OBJECT_SIZE_INVALID");
            return false;
        }
        ObjectBytes = TotalBytes;

        // Prime one bounded block so TIFFClientOpen can inspect the header without issuing
        // tiny network requests for each individual tag field.
        const uint64 FirstBlockBytes = FMath::Min<uint64>(Limits.RangeBlockBytes, ObjectBytes);
        TArray<uint8> FirstBlock;
        if (!Fetch(0, FirstBlockBytes, false, FirstBlock, TotalBytes, OutError)) return false;
        if (TotalBytes != ObjectBytes)
        {
            OutError = TEXT("COG_PREFLIGHT_OBJECT_SIZE_CHANGED");
            Invalidate();
            return false;
        }
        Cache.Add(0, MoveTemp(FirstBlock));
        CacheOrder.Add(0);
        CachedBytes = FirstBlockBytes;
        return true;
    }

    uint64 Size() const noexcept override { return ObjectBytes; }

    bool Read(const uint64 Offset, const uint64 Length, TArray<uint8>& OutBytes, FString& OutError) override
    {
        OutBytes.Reset();
        if (ObjectBytes == 0 || Length == 0 || Length > Limits.MaxRangeBytes
            || Length > Limits.MaxTransferredBytes || Length > MAX_int32 || Offset > ObjectBytes
            || Length > ObjectBytes - Offset)
        {
            OutError = TEXT("COG_PREFLIGHT_READ_OUT_OF_BOUNDS");
            return false;
        }
        OutBytes.Reserve(static_cast<int32>(Length));
        uint64 Position = Offset;
        uint64 Remaining = Length;
        while (Remaining > 0)
        {
            const uint64 BlockOffset = (Position / Limits.RangeBlockBytes) * Limits.RangeBlockBytes;
            const uint64 BlockLength = FMath::Min<uint64>(Limits.RangeBlockBytes, ObjectBytes - BlockOffset);
            TArray<uint8>* Block = Cache.Find(BlockOffset);
            if (!Block)
            {
                TArray<uint8> Fetched;
                uint64 TotalBytes = 0;
                if (!Fetch(BlockOffset, BlockLength, false, Fetched, TotalBytes, OutError)) return false;
                if (TotalBytes != ObjectBytes)
                {
                    OutError = TEXT("COG_PREFLIGHT_OBJECT_SIZE_CHANGED");
                    Invalidate();
                    return false;
                }
                InsertCache(BlockOffset, MoveTemp(Fetched));
                Block = Cache.Find(BlockOffset);
            }
            else
            {
                Touch(BlockOffset);
            }
            if (!Block || Block->Num() != static_cast<int32>(BlockLength))
            {
                OutError = TEXT("COG_PREFLIGHT_CACHE_INVALID");
                return false;
            }
            const uint64 Within = Position - BlockOffset;
            const uint64 Count = FMath::Min<uint64>(Remaining, BlockLength - Within);
            OutBytes.Append(Block->GetData() + Within, static_cast<int32>(Count));
            Position += Count;
            Remaining -= Count;
        }
        return static_cast<uint64>(OutBytes.Num()) == Length;
    }

    uint64 GetRequestCount() const { return RequestCount; }
    uint64 GetTransferredBytes() const { return TransferredBytes; }
    uint64 GetObjectBytes() const { return ObjectBytes; }
    bool HasPinnedETag() const { return !PinnedETag.IsEmpty(); }

private:
    bool Fetch(const uint64 Offset, const uint64 Length, const bool bInitial, TArray<uint8>& OutBytes,
        uint64& OutTotalBytes, FString& OutError)
    {
        OutBytes.Reset();
        OutTotalBytes = 0;
        if (Cancellation->IsCancelled())
        {
            OutError = TEXT("COG_PREFLIGHT_CANCELLED");
            return false;
        }
        if (Length == 0 || Length > Limits.MaxRangeBytes || Length > MAX_int32
            || Offset > MAX_uint64 - (Length - 1) || RequestCount >= Limits.MaxRequests
            || TransferredBytes > Limits.MaxTransferredBytes
            || Length > Limits.MaxTransferredBytes - TransferredBytes)
        {
            OutError = RequestCount >= Limits.MaxRequests || Length > Limits.MaxTransferredBytes
                ? TEXT("COG_PREFLIGHT_BUDGET_EXCEEDED") : TEXT("COG_PREFLIGHT_RANGE_INVALID");
            return false;
        }

        SkiPreparation::HttpAcquisitionRequest Request;
        Request.Url = Url;
        Request.Product = SkiPreparation::ProviderProduct::CoreElevation;
        Request.MaximumResponseBytes = Length;
        Request.TotalTimeoutSeconds = 30.0F;
        Request.ActivityTimeoutSeconds = 15.0F;
        Request.ByteRange = SkiPreparation::HttpByteRange{Offset, Length};
        if (!bInitial) Request.IfMatchETag = PinnedETag;
        ++RequestCount;
        const SkiPreparation::HttpAcquisitionResult Result = Transport.Get(Request, Cancellation);

        if (!bInitial && Result.HttpStatus == 412)
        {
            OutError = TEXT("COG_PREFLIGHT_ETAG_PRECONDITION_FAILED");
            Invalidate();
            return false;
        }
        if (!bInitial && Result.HttpStatus == 206 && Result.ETag != PinnedETag)
        {
            OutError = Result.ETag.IsEmpty() ? TEXT("COG_PREFLIGHT_ETAG_MISSING") : TEXT("COG_PREFLIGHT_ETAG_CHANGED");
            Invalidate();
            return false;
        }
        if (Result.FailureReason != SkiPreparation::TransportFailureReason::None
            || Result.HttpStatus != 206 || Result.Bytes.IsEmpty())
        {
            OutError = TEXT("COG_PREFLIGHT_RANGE_REQUEST_FAILED");
            return false;
        }
        if (Result.Bytes.Num() < 0 || static_cast<uint64>(Result.Bytes.Num()) != Length
            || (Result.BytesReceived != 0 && Result.BytesReceived != static_cast<uint64>(Result.Bytes.Num())))
        {
            OutError = TEXT("COG_PREFLIGHT_RANGE_LENGTH_INVALID");
            return false;
        }
        if (!SkiPreparation::ValidateContentRange(Result.ContentRange,
                Request.ByteRange.GetValue(), static_cast<uint64>(Result.Bytes.Num()), OutTotalBytes))
        {
            OutError = TEXT("COG_PREFLIGHT_CONTENT_RANGE_INVALID");
            return false;
        }
        if (bInitial)
        {
            if (!IsSafeStrongETag(Result.ETag))
            {
                OutError = TEXT("COG_PREFLIGHT_STRONG_ETAG_REQUIRED");
                return false;
            }
            PinnedETag = Result.ETag;
        }
        else if (Result.ETag != PinnedETag)
        {
            OutError = TEXT("COG_PREFLIGHT_ETAG_CHANGED");
            Invalidate();
            return false;
        }
        if (!bInitial && ObjectBytes != 0 && OutTotalBytes != ObjectBytes)
        {
            OutError = TEXT("COG_PREFLIGHT_OBJECT_SIZE_CHANGED");
            Invalidate();
            return false;
        }
        TransferredBytes += Result.Bytes.Num();
        OutBytes = Result.Bytes;
        return true;
    }

    void Invalidate()
    {
        ObjectBytes = 0;
        PinnedETag.Reset();
        Cache.Reset();
        CacheOrder.Reset();
        CachedBytes = 0;
    }

    void Touch(const uint64 Offset)
    {
        CacheOrder.Remove(Offset);
        CacheOrder.Add(Offset);
    }

    void InsertCache(const uint64 Offset, TArray<uint8>&& Bytes)
    {
        if (Bytes.Num() > static_cast<int64>(Limits.MaxCachedBytes)) return;
        if (TArray<uint8>* Existing = Cache.Find(Offset)) CachedBytes -= Existing->Num();
        Cache.Add(Offset, MoveTemp(Bytes));
        CachedBytes += Cache.FindChecked(Offset).Num();
        Touch(Offset);
        while (CachedBytes > Limits.MaxCachedBytes && CacheOrder.Num() > 1)
        {
            const uint64 Oldest = CacheOrder[0];
            CacheOrder.RemoveAt(0);
            if (TArray<uint8>* Evicted = Cache.Find(Oldest)) CachedBytes -= Evicted->Num();
            Cache.Remove(Oldest);
        }
    }

    SkiPreparation::IAcquisitionTransport& Transport;
    FString Url;
    const SkiPreparation::FCogPreflightLimits& Limits;
    TSharedRef<SkiPreparation::Cancellation> Cancellation;
    FString PinnedETag;
    uint64 ObjectBytes = 0;
    uint64 RequestCount = 0;
    uint64 TransferredBytes = 0;
    uint64 CachedBytes = 0;
    TMap<uint64, TArray<uint8>> Cache;
    TArray<uint64> CacheOrder;
};

struct FCogTiffReader
{
    SkiPreparation::ICogByteSource* Source = nullptr;
    uint64 Position = 0;
    FString Error;
};

tmsize_t ReadCog(thandle_t Handle, void* Destination, const tmsize_t Requested)
{
    FCogTiffReader& Reader = *static_cast<FCogTiffReader*>(Handle);
    if (!Reader.Source || !Destination || Requested <= 0) return 0;
    const uint64 ObjectBytes = Reader.Source->Size();
    if (Reader.Position >= ObjectBytes)
    {
        Reader.Error = TEXT("COG_PREFLIGHT_READ_OUT_OF_BOUNDS");
        return 0;
    }
    const uint64 Count = FMath::Min<uint64>(static_cast<uint64>(Requested), ObjectBytes - Reader.Position);
    if (Count == 0 || Count > MAX_int32)
    {
        Reader.Error = TEXT("COG_PREFLIGHT_READ_LENGTH_INVALID");
        return 0;
    }
    TArray<uint8> Bytes;
    if (!Reader.Source->Read(Reader.Position, Count, Bytes, Reader.Error)
        || static_cast<uint64>(Bytes.Num()) != Count)
        return 0;
    FMemory::Memcpy(Destination, Bytes.GetData(), static_cast<SIZE_T>(Count));
    Reader.Position += Count;
    return static_cast<tmsize_t>(Count);
}

tmsize_t RejectWrite(thandle_t, void*, const tmsize_t) { return 0; }

toff_t SeekCog(thandle_t Handle, const toff_t Offset, const int Origin)
{
    FCogTiffReader& Reader = *static_cast<FCogTiffReader*>(Handle);
    if (!Reader.Source) return static_cast<toff_t>(-1);
    const uint64 ObjectBytes = Reader.Source->Size();
    uint64 Base = 0;
    if (Origin == SEEK_SET) Base = 0;
    else if (Origin == SEEK_CUR) Base = Reader.Position;
    else if (Origin == SEEK_END) Base = ObjectBytes;
    else return static_cast<toff_t>(-1);
    if (static_cast<uint64>(Offset) > MAX_uint64 - Base || Base + static_cast<uint64>(Offset) > ObjectBytes)
    {
        Reader.Error = TEXT("COG_PREFLIGHT_SEEK_OUT_OF_BOUNDS");
        return static_cast<toff_t>(-1);
    }
    Reader.Position = Base + static_cast<uint64>(Offset);
    return static_cast<toff_t>(Reader.Position);
}

int CloseCog(thandle_t) { return 0; }
toff_t SizeCog(thandle_t Handle)
{
    const FCogTiffReader& Reader = *static_cast<FCogTiffReader*>(Handle);
    return Reader.Source ? static_cast<toff_t>(Reader.Source->Size()) : 0;
}
int RejectMap(thandle_t, void**, toff_t*) { return 0; }
void RejectUnmap(thandle_t, void*, toff_t) {}

bool HasClassicTiffSignature(SkiPreparation::ICogByteSource& Source, FString& OutError)
{
    TArray<uint8> Signature;
    if (!Source.Read(0, 4, Signature, OutError) || Signature.Num() != 4) return false;
    return (Signature[0] == 'I' && Signature[1] == 'I' && Signature[2] == 42 && Signature[3] == 0)
        || (Signature[0] == 'M' && Signature[1] == 'M' && Signature[2] == 0 && Signature[3] == 42);
}

bool IsSupportedHorizontalCode(const SkiDomain::ElevationProduct Product, const uint16 ModelType,
    const uint16 HorizontalCode)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M:
        return ModelType == ModelProjected && HorizontalCode == 6350;
    case SkiDomain::ElevationProduct::Project1m:
        return ModelType == ModelProjected && HorizontalCode >= 26901 && HorizontalCode <= 26923;
    case SkiDomain::ElevationProduct::ArcSec13:
        return ModelType == ModelGeographic && HorizontalCode == 4269;
    }
    return false;
}

bool ValidateHorizontalAndVerticalCrs(TIFF* Image, const SkiDomain::ElevationProduct Product,
    const SkiPreparation::FCogPreflightMetadata& Metadata, SkiPreparation::FCogPreflightReport& Report)
{
    bool bHasModel = false, bHasRasterType = false, bHasGeographic = false, bHasProjected = false;
    bool bHasVerticalCrs = false, bHasVerticalDatum = false;
    uint16 Model = 0, RasterType = 0, Geographic = 0, Projected = 0, VerticalCrs = 0, VerticalDatum = 0;
    if (!TryGetGeoKey(Image, GeoKeyModelType, bHasModel, Model)
        || !TryGetGeoKey(Image, GeoKeyRasterType, bHasRasterType, RasterType)
        || !TryGetGeoKey(Image, GeoKeyGeographicType, bHasGeographic, Geographic)
        || !TryGetGeoKey(Image, GeoKeyProjectedType, bHasProjected, Projected)
        || !TryGetGeoKey(Image, GeoKeyVerticalCSType, bHasVerticalCrs, VerticalCrs)
        || !TryGetGeoKey(Image, GeoKeyVerticalDatum, bHasVerticalDatum, VerticalDatum)
        || !bHasModel || !bHasRasterType || RasterType != RasterPixelIsArea)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_GEO_KEYS_INVALID"), TEXT("GeoTIFF CRS or pixel registration keys are missing or malformed."));
        return false;
    }
    const uint16 HorizontalCode = Model == ModelProjected && bHasProjected ? Projected
        : Model == ModelGeographic && bHasGeographic ? Geographic : 0;
    if (HorizontalCode == 0 || !IsSupportedHorizontalCode(Product, Model, HorizontalCode))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_CRS_UNSUPPORTED"), TEXT("GeoTIFF horizontal CRS is outside the supported product profile."));
        return false;
    }
    Report.HorizontalCrs = FString::Printf(TEXT("EPSG:%u"), static_cast<uint32>(HorizontalCode));
    Report.bPixelIsArea = true;

    uint32 CatalogCode = 0;
    if (!Metadata.CatalogHorizontalCrs.IsEmpty()
        && (!ParseCatalogEpsg(Metadata.CatalogHorizontalCrs, CatalogCode) || CatalogCode != HorizontalCode))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_CATALOG_CRS_MISMATCH"), TEXT("GeoTIFF CRS disagrees with the catalog CRS."));
        return false;
    }

    const bool bHeaderDatumKeyPresent = bHasVerticalCrs || bHasVerticalDatum;
    bool bHeaderNavd88 = false;
    if (bHasVerticalCrs && VerticalCrs != Navd88VerticalCrs)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_VERTICAL_CRS_UNSUPPORTED"), TEXT("GeoTIFF declares a vertical CRS other than NAVD88 height."));
        return false;
    }
    if (bHasVerticalDatum && VerticalDatum != Navd88VerticalDatum)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_VERTICAL_DATUM_CONFLICT"), TEXT("GeoTIFF declares a vertical datum other than NAVD88."));
        return false;
    }
    bHeaderNavd88 = (bHasVerticalCrs && VerticalCrs == Navd88VerticalCrs)
        || (bHasVerticalDatum && VerticalDatum == Navd88VerticalDatum);
    const bool bCatalogNavd88 = IsNavd88(Metadata.CatalogVerticalDatum);
    if (!Metadata.CatalogVerticalDatum.IsEmpty() && !bCatalogNavd88)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_CATALOG_DATUM_UNSUPPORTED"), TEXT("Catalog vertical datum is not NAVD88."));
        return false;
    }
    if (Product == SkiDomain::ElevationProduct::S1M && !bHeaderNavd88)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_S1M_DATUM_UNPROVEN"), TEXT("S1M requires a NAVD88 vertical key in the GeoTIFF header."));
        return false;
    }
    if (!bHeaderNavd88 && !bCatalogNavd88)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_DATUM_UNPROVEN"), TEXT("NAVD88 is not confirmed by a vertical key or catalog metadata."));
        return false;
    }
    if (bHeaderDatumKeyPresent && !bHeaderNavd88)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_VERTICAL_DATUM_CONFLICT"), TEXT("GeoTIFF vertical keys do not prove NAVD88."));
        return false;
    }
    Report.VerticalDatum = TEXT("NAVD88");
    Report.VerticalDatumOrigin = bHeaderNavd88 ? TEXT("GeoTIFF GeoKey") : TEXT("catalog metadata");
    return true;
}

bool ValidateGeoreferencing(TIFF* Image, SkiPreparation::FCogPreflightReport& Report)
{
    uint32 ScaleCount = 0, TieCount = 0;
    double* Scale = nullptr;
    double* Tie = nullptr;
    if (!TIFFGetField(Image, ModelPixelScaleTag, &ScaleCount, &Scale)
        || !TIFFGetField(Image, ModelTiepointTag, &TieCount, &Tie)
        || !Scale || ScaleCount < 2 || !Tie || TieCount < 6)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_GEOREFERENCE_MISSING"), TEXT("GeoTIFF pixel scale or tiepoint metadata is missing."));
        return false;
    }
    for (uint32 Index = 0; Index < ScaleCount; ++Index)
    {
        if (!std::isfinite(Scale[Index]))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_GEOREFERENCE_INVALID"), TEXT("GeoTIFF pixel scale contains a non-finite value."));
            return false;
        }
    }
    for (uint32 Index = 0; Index < TieCount; ++Index)
    {
        if (!std::isfinite(Tie[Index]))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_GEOREFERENCE_INVALID"), TEXT("GeoTIFF tiepoint contains a non-finite value."));
            return false;
        }
    }
    if (Scale[0] <= 0.0 || Scale[1] <= 0.0)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_GEOREFERENCE_INVALID"), TEXT("GeoTIFF pixel scale must be positive and north-up."));
        return false;
    }
    return true;
}

bool ValidateBaseNoData(TIFF* Image, const SkiDomain::ElevationProduct Product,
    SkiPreparation::FCogPreflightReport& Report)
{
    uint32 Count = 0;
    char* Text = nullptr;
    double Value = 0.0;
    if (!TIFFGetField(Image, TIFFTAG_GDAL_NODATA, &Count, &Text)
        || !ParseNoData(Text, Count, Value, Report.NoDataText))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_NODATA_INVALID"), TEXT("GeoTIFF nodata metadata is missing or malformed."));
        return false;
    }
    if (Product == SkiDomain::ElevationProduct::S1M && Value != -999999.0)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_S1M_NODATA_MISMATCH"), TEXT("S1M nodata must be exactly -999999."));
        return false;
    }
    return true;
}

bool IsSupportedCompression(const uint16 Compression)
{
    return Compression == COMPRESSION_LZW || Compression == COMPRESSION_ADOBE_DEFLATE
        || Compression == COMPRESSION_DEFLATE;
}

bool ValidateDirectory(TIFF* Image, const bool bOverview, const SkiPreparation::FCogPreflightLimits& Limits,
    uint64& InOutTotalTiles, std::vector<std::pair<uint64, uint64>>& InOutRanges,
    SkiPreparation::FCogDirectoryPreflight& OutDirectory, SkiPreparation::FCogPreflightReport& Report)
{
    uint32 Width = 0, Height = 0, TileWidth = 0, TileHeight = 0;
    uint16 Bits = 0, Samples = 0, SampleFormat = 0, Planar = 0, Orientation = 0, Compression = 0, Predictor = 0;
    if (!TIFFGetField(Image, TIFFTAG_IMAGEWIDTH, &Width)
        || !TIFFGetField(Image, TIFFTAG_IMAGELENGTH, &Height)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_BITSPERSAMPLE, &Bits)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_SAMPLESPERPIXEL, &Samples)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_SAMPLEFORMAT, &SampleFormat)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_PLANARCONFIG, &Planar)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_ORIENTATION, &Orientation)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_COMPRESSION, &Compression)
        || !TIFFGetFieldDefaulted(Image, TIFFTAG_PREDICTOR, &Predictor)
        || !TIFFIsTiled(Image)
        || !TIFFGetField(Image, TIFFTAG_TILEWIDTH, &TileWidth)
        || !TIFFGetField(Image, TIFFTAG_TILELENGTH, &TileHeight))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_DIRECTORY_TAGS_MISSING"), TEXT("A COG directory omits required tiled image tags."));
        return false;
    }
    if (Width < 2 || Height < 2 || Width > Limits.MaxDimension || Height > Limits.MaxDimension
        || TileWidth == 0 || TileHeight == 0 || TileWidth > 4096 || TileHeight > 4096
        || Bits != 32 || Samples != 1 || SampleFormat != SAMPLEFORMAT_IEEEFP
        || Planar != PLANARCONFIG_CONTIG || Orientation != ORIENTATION_TOPLEFT
        || !IsSupportedCompression(Compression) || Predictor < 1 || Predictor > 3)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_ENCODING_UNSUPPORTED"), TEXT("A COG directory has unsafe dimensions or unsupported float32 tile encoding."));
        return false;
    }
    if ((Limits.ExpectedTileWidth != 0 && TileWidth != Limits.ExpectedTileWidth)
        || (Limits.ExpectedTileHeight != 0 && TileHeight != Limits.ExpectedTileHeight))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_TILE_LAYOUT_UNSUPPORTED"), TEXT("COG tile dimensions do not match the USGS acquisition profile."));
        return false;
    }

    const uint64 TileColumns = (static_cast<uint64>(Width) + TileWidth - 1) / TileWidth;
    const uint64 TileRows = (static_cast<uint64>(Height) + TileHeight - 1) / TileHeight;
    if (TileColumns == 0 || TileRows == 0 || TileColumns > MAX_uint64 / TileRows)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_TILE_COUNT_INVALID"), TEXT("COG tile grid count overflows."));
        return false;
    }
    const uint64 ExpectedTileCount = TileColumns * TileRows;
    const ttile_t LibTiffTileCount = TIFFNumberOfTiles(Image);
    if (ExpectedTileCount > MAX_uint32 || ExpectedTileCount > Limits.MaxTilesPerDirectory || ExpectedTileCount > Limits.MaxTotalTiles
        || InOutTotalTiles > Limits.MaxTotalTiles - ExpectedTileCount
        || ExpectedTileCount != static_cast<uint64>(LibTiffTileCount))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_TILE_COUNT_INVALID"), TEXT("COG tile offset table count does not match its dimensions."));
        return false;
    }

    toff_t* Offsets = nullptr;
    uint64* ByteCounts = nullptr;
    if (!TIFFGetField(Image, TIFFTAG_TILEOFFSETS, &Offsets)
        || !TIFFGetField(Image, TIFFTAG_TILEBYTECOUNTS, &ByteCounts) || !Offsets || !ByteCounts)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_TILE_INDEX_MISSING"), TEXT("COG tile offset or bytecount table is missing."));
        return false;
    }
    uint64 PreviousEnd = 0;
    for (uint64 Tile = 0; Tile < ExpectedTileCount; ++Tile)
    {
        const uint64 Offset = static_cast<uint64>(Offsets[Tile]);
        const uint64 ByteCount = ByteCounts[Tile];
        if (Offset < 8 || ByteCount == 0 || Offset > Limits.MaxObjectBytes
            || Offset > MAX_uint64 - ByteCount || Offset + ByteCount > Report.ObjectBytes
            || (Tile > 0 && Offset < PreviousEnd))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_TILE_RANGE_INVALID"), TEXT("COG tile offset/bytecount is out of bounds, empty, or overlapping."));
            return false;
        }
        PreviousEnd = Offset + ByteCount;
        InOutRanges.emplace_back(Offset, PreviousEnd);
    }
    InOutTotalTiles += ExpectedTileCount;

    OutDirectory.Width = Width;
    OutDirectory.Height = Height;
    OutDirectory.TileWidth = TileWidth;
    OutDirectory.TileHeight = TileHeight;
    OutDirectory.TileCount = static_cast<uint32>(ExpectedTileCount);
    OutDirectory.Compression = Compression;
    OutDirectory.Predictor = Predictor;
    OutDirectory.bOverview = bOverview;
    if (!bOverview)
    {
        Report.Width = Width;
        Report.Height = Height;
        Report.TileWidth = TileWidth;
        Report.TileHeight = TileHeight;
        Report.BitsPerSample = Bits;
        Report.SamplesPerPixel = Samples;
        Report.Predictor = Predictor;
        Report.SampleFormat = TEXT("FLOAT32");
        if (Compression == COMPRESSION_LZW) Report.Compression = TEXT("LZW");
        else Report.Compression = TEXT("DEFLATE");
    }
    return true;
}

bool OpenAndValidateTiff(SkiPreparation::ICogByteSource& Source, const SkiDomain::ElevationProduct Product,
    const SkiPreparation::FCogPreflightMetadata& Metadata, const SkiPreparation::FCogPreflightLimits& Limits,
    SkiPreparation::FCogPreflightReport& Report)
{
    FString ReadError;
    if (!HasClassicTiffSignature(Source, ReadError))
    {
        Fail(Report, TEXT("COG_PREFLIGHT_CLASSIC_TIFF_REQUIRED"), TEXT("The object is not a readable classic TIFF; BigTIFF and non-TIFF bodies are rejected."));
        return false;
    }
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    FCogTiffReader Reader;
    Reader.Source = &Source;
    std::unique_ptr<TIFF, decltype(&TIFFClose)> Image(TIFFClientOpen("MountainPlannerCogPreflight", "r",
        &Reader, ReadCog, RejectWrite, SeekCog, CloseCog, SizeCog, RejectMap, RejectUnmap), &TIFFClose);
    if (!Image)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_TIFF_OPEN_FAILED"), Reader.Error.IsEmpty()
            ? TEXT("LibTiff rejected the TIFF header or first directory.") : *Reader.Error);
        return false;
    }

    if (!ValidateHorizontalAndVerticalCrs(Image.get(), Product, Metadata, Report)
        || !ValidateGeoreferencing(Image.get(), Report)
        || !ValidateBaseNoData(Image.get(), Product, Report)) return false;

    std::vector<std::pair<uint64, uint64>> Ranges;
    uint64 TotalTiles = 0;
    TSet<uint64> SeenDirectoryOffsets;
    TArray<toff_t> PendingSubIfds;
    bool bFirstDirectory = true;
    bool bReadMainDirectory = true;
    while (bReadMainDirectory)
    {
        if (Report.Directories.Num() >= static_cast<int32>(Limits.MaxDirectories))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_DIRECTORY_LIMIT_EXCEEDED"), TEXT("COG has more directories than the preflight limit."));
            return false;
        }
        const uint64 DirectoryOffset = static_cast<uint64>(TIFFCurrentDirOffset(Image.get()));
        if (DirectoryOffset < 8 || DirectoryOffset >= Source.Size() || SeenDirectoryOffsets.Contains(DirectoryOffset))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_DIRECTORY_OFFSET_INVALID"), TEXT("COG directory pointer is invalid or repeated."));
            return false;
        }
        SeenDirectoryOffsets.Add(DirectoryOffset);
        if (!bFirstDirectory)
        {
            uint32 SubfileType = 0;
            if (!TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SUBFILETYPE, &SubfileType)
                || (SubfileType & FILETYPE_REDUCEDIMAGE) == 0)
            {
                Fail(Report, TEXT("COG_PREFLIGHT_OVERVIEW_TAG_INVALID"), TEXT("Non-base COG directories must declare reduced-resolution imagery."));
                return false;
            }
        }
        SkiPreparation::FCogDirectoryPreflight Directory;
        if (!ValidateDirectory(Image.get(), !bFirstDirectory, Limits, TotalTiles, Ranges, Directory, Report))
            return false;
        if (!bFirstDirectory && (Directory.Width >= Report.Width || Directory.Height >= Report.Height))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_OVERVIEW_NOT_REDUCED"), TEXT("Every overview must be strictly smaller than the base raster."));
            return false;
        }
        Report.Directories.Add(Directory);

        uint16 SubCount = 0;
        toff_t* SubOffsets = nullptr;
        if (TIFFGetField(Image.get(), TIFFTAG_SUBIFD, &SubCount, &SubOffsets) && SubCount > 0)
        {
            const uint64 PlannedDirectoryCount = static_cast<uint64>(Report.Directories.Num())
                + static_cast<uint64>(PendingSubIfds.Num()) + static_cast<uint64>(SubCount);
            if (!SubOffsets || PlannedDirectoryCount > static_cast<uint64>(Limits.MaxDirectories))
            {
                Fail(Report, TEXT("COG_PREFLIGHT_SUBIFD_INVALID"), TEXT("COG SubIFD table is missing or exceeds the directory limit."));
                return false;
            }
            for (uint16 Index = 0; Index < SubCount; ++Index) PendingSubIfds.Add(SubOffsets[Index]);
        }
        bFirstDirectory = false;
        bReadMainDirectory = TIFFReadDirectory(Image.get()) != 0;
        if (!bReadMainDirectory && !Reader.Error.IsEmpty())
        {
            Fail(Report, TEXT("COG_PREFLIGHT_DIRECTORY_READ_FAILED"), *Reader.Error);
            return false;
        }
    }

    while (!PendingSubIfds.IsEmpty())
    {
        if (Report.Directories.Num() >= static_cast<int32>(Limits.MaxDirectories))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_DIRECTORY_LIMIT_EXCEEDED"), TEXT("COG has more SubIFDs than the preflight limit."));
            return false;
        }
        const toff_t SubOffset = PendingSubIfds.Pop(EAllowShrinking::No);
        if (static_cast<uint64>(SubOffset) < 8 || static_cast<uint64>(SubOffset) >= Source.Size()
            || SeenDirectoryOffsets.Contains(static_cast<uint64>(SubOffset))
            || !TIFFSetSubDirectory(Image.get(), SubOffset))
        {
            Fail(Report, TEXT("COG_PREFLIGHT_SUBIFD_OFFSET_INVALID"), Reader.Error.IsEmpty()
                ? TEXT("COG SubIFD pointer is invalid or repeated.") : *Reader.Error);
            return false;
        }
        SeenDirectoryOffsets.Add(static_cast<uint64>(SubOffset));
        uint32 SubfileType = 0;
        if (!TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SUBFILETYPE, &SubfileType)
            || (SubfileType & FILETYPE_REDUCEDIMAGE) == 0)
        {
            Fail(Report, TEXT("COG_PREFLIGHT_OVERVIEW_TAG_INVALID"), TEXT("SubIFD directories must declare reduced-resolution imagery."));
            return false;
        }
        SkiPreparation::FCogDirectoryPreflight Directory;
        if (!ValidateDirectory(Image.get(), true, Limits, TotalTiles, Ranges, Directory, Report)) return false;
        if (Directory.Width >= Report.Width || Directory.Height >= Report.Height)
        {
            Fail(Report, TEXT("COG_PREFLIGHT_OVERVIEW_NOT_REDUCED"), TEXT("Every overview must be strictly smaller than the base raster."));
            return false;
        }
        Report.Directories.Add(Directory);
        uint16 SubCount = 0;
        toff_t* SubOffsets = nullptr;
        if (TIFFGetField(Image.get(), TIFFTAG_SUBIFD, &SubCount, &SubOffsets) && SubCount > 0)
        {
            const uint64 PlannedDirectoryCount = static_cast<uint64>(Report.Directories.Num())
                + static_cast<uint64>(PendingSubIfds.Num()) + static_cast<uint64>(SubCount);
            if (!SubOffsets || PlannedDirectoryCount > static_cast<uint64>(Limits.MaxDirectories))
            {
                Fail(Report, TEXT("COG_PREFLIGHT_SUBIFD_INVALID"), TEXT("Nested SubIFD table is missing or exceeds the directory limit."));
                return false;
            }
            for (uint16 Index = 0; Index < SubCount; ++Index) PendingSubIfds.Add(SubOffsets[Index]);
        }
    }
    if (Report.Directories.Num() < 2)
    {
        Fail(Report, TEXT("COG_PREFLIGHT_OVERVIEW_MISSING"), TEXT("Elevation COG must contain at least one reduced-resolution overview."));
        return false;
    }

    std::sort(Ranges.begin(), Ranges.end());
    for (size_t Index = 1; Index < Ranges.size(); ++Index)
    {
        if (Ranges[Index].first < Ranges[Index - 1].second)
        {
            Fail(Report, TEXT("COG_PREFLIGHT_TILE_RANGES_OVERLAP"), TEXT("COG tile payload ranges overlap."));
            return false;
        }
    }
    return true;
}
}

bool SkiPreparation::PreflightElevationCog(IAcquisitionTransport& Transport, const FString& Url,
    const SkiDomain::ElevationProduct Product, const FCogPreflightMetadata& Metadata,
    const FCogPreflightLimits& Limits, const TSharedRef<Cancellation>& Cancellation,
    FCogPreflightReport& OutReport)
{
    OutReport = {};
    FBoundedCogRangeSource Source(Transport, Url, Limits, Cancellation);
    FString Error;
    if (!Source.Initialize(Error))
    {
        if (Error == TEXT("COG_PREFLIGHT_STRONG_ETAG_REQUIRED"))
            Fail(OutReport, TEXT("COG_PREFLIGHT_STRONG_ETAG_REQUIRED"), TEXT("The server did not provide a strong ETag for immutable range reads."));
        else if (Error == TEXT("COG_PREFLIGHT_CONTENT_RANGE_INVALID"))
            Fail(OutReport, TEXT("COG_PREFLIGHT_CONTENT_RANGE_INVALID"), TEXT("Initial range did not exactly match its Content-Range."));
        else if (Error == TEXT("COG_PREFLIGHT_RANGE_LENGTH_INVALID"))
            Fail(OutReport, TEXT("COG_PREFLIGHT_RANGE_LENGTH_INVALID"), TEXT("Initial range body length is inconsistent."));
        else Fail(OutReport, *Error, TEXT("COG initial range could not be safely established."));
        OutReport.Requests = Source.GetRequestCount();
        OutReport.TransferredBytes = Source.GetTransferredBytes();
        OutReport.ObjectBytes = Source.GetObjectBytes();
        return false;
    }
    OutReport.ObjectBytes = Source.GetObjectBytes();
    OutReport.bStrongETagPinned = Source.HasPinnedETag();
    const bool bPassed = OpenAndValidateTiff(Source, Product, Metadata, Limits, OutReport);
    OutReport.Requests = Source.GetRequestCount();
    OutReport.TransferredBytes = Source.GetTransferredBytes();
    OutReport.ObjectBytes = Source.GetObjectBytes();
    if (!bPassed) return false;
    OutReport.bPassed = true;
    OutReport.FailureCode.Empty();
    OutReport.FailureDetail.Empty();
    return true;
}
