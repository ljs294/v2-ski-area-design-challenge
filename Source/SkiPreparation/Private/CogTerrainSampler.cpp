#include "SkiPreparation/CogTerrainSampler.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/WorldCoverCogDecoder.h"
#include "tiffio.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr uint64 OutputTileBytes = static_cast<uint64>(SkiPreparation::TerrainScratchTileSamples)
    * SkiPreparation::TerrainScratchTileSamples * SkiPreparation::TerrainScratchBytesPerSample;

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

bool ParseFiniteNumber(const FString& Text, double& OutValue)
{
    OutValue = 0.0;
    const FTCHARToUTF8 Utf8(*Text);
    errno = 0;
    char* End = nullptr;
    const double Parsed = std::strtod(Utf8.Get(), &End);
    if (errno != 0 || End == Utf8.Get() || !std::isfinite(Parsed)) return false;
    while (*End == ' ' || *End == '\t' || *End == '\r' || *End == '\n') ++End;
    if (*End != '\0') return false;
    OutValue = Parsed;
    return true;
}

int32 ProductPriority(const SkiDomain::ElevationProduct Product)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return 0;
    case SkiDomain::ElevationProduct::Project1m: return 1;
    case SkiDomain::ElevationProduct::ArcSec13: return 2;
    default: return MAX_int32;
    }
}

const char* ProductName(const SkiDomain::ElevationProduct Product)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return "S1M";
    case SkiDomain::ElevationProduct::Project1m: return "Project1m";
    case SkiDomain::ElevationProduct::ArcSec13: return "ArcSec13";
    default: return "";
    }
}

int32 ProvenanceCounterIndex(const SkiPreparation::TerrainScratchProvenance Provenance)
{
    const uint8 Value = static_cast<uint8>(Provenance);
    if (Value <= static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::ArcSec13)) return Value;
    return Provenance == SkiPreparation::TerrainScratchProvenance::NoData ? 6 : INDEX_NONE;
}

void SetFailure(SkiPreparation::FCogTerrainSamplerReport& Report, const TCHAR* Code,
    const FString& Detail)
{
    Report.bPassed = false;
    Report.FailureCode = Code;
    Report.FailureDetail = Detail;
}

class FRequestBudget final
{
public:
    FRequestBudget(SkiPreparation::IAcquisitionTransport& InTransport,
        const TSharedRef<SkiPreparation::Cancellation>& InCancellation,
        const uint64 InMaximumRequests, const uint64 InMaximumBytes)
        : Transport(InTransport), Cancellation(InCancellation), MaximumRequests(InMaximumRequests),
          MaximumBytes(InMaximumBytes) {}

    bool Send(const SkiPreparation::HttpAcquisitionRequest& Request,
        SkiPreparation::HttpAcquisitionResult& OutResult, FString& OutError)
    {
        LastBudgetFailure.Reset();
        if (Cancellation->IsCancelled())
        {
            LastBudgetFailure = TEXT("COG_SAMPLER_CANCELLED");
            OutError = LastBudgetFailure;
            OutResult = {};
            OutResult.FailureReason = SkiPreparation::TransportFailureReason::Cancelled;
            return false;
        }
        if (RequestCount >= MaximumRequests)
        {
            LastBudgetFailure = TEXT("COG_SAMPLER_REQUEST_BUDGET_EXCEEDED");
            OutError = LastBudgetFailure;
            OutResult = {};
            OutResult.FailureReason = SkiPreparation::TransportFailureReason::ResponseTooLarge;
            return false;
        }
        const uint64 ExpectedResponse = Request.ByteRange.IsSet()
            ? Request.ByteRange->Length : Request.MaximumResponseBytes;
        if (ExpectedResponse == 0 || ExpectedResponse > MaximumBytes
            || TransferredBytes > MaximumBytes - ExpectedResponse)
        {
            LastBudgetFailure = TEXT("COG_SAMPLER_TRANSFER_BUDGET_EXCEEDED");
            OutError = LastBudgetFailure;
            OutResult = {};
            OutResult.FailureReason = SkiPreparation::TransportFailureReason::ResponseTooLarge;
            return false;
        }
        ++RequestCount;
        OutResult = Transport.Get(Request, Cancellation);
        const uint64 Received = static_cast<uint64>(FMath::Max(OutResult.Bytes.Num(), 0));
        if (Received > MaximumBytes || TransferredBytes > MaximumBytes - Received)
        {
            TransferredBytes = MaximumBytes;
            LastBudgetFailure = TEXT("COG_SAMPLER_TRANSFER_BUDGET_EXCEEDED");
            OutError = LastBudgetFailure;
            OutResult = {};
            OutResult.FailureReason = SkiPreparation::TransportFailureReason::ResponseTooLarge;
            return false;
        }
        TransferredBytes += Received;
        return true;
    }

    SkiPreparation::IAcquisitionTransport& Transport;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation;
    const uint64 MaximumRequests;
    const uint64 MaximumBytes;
    uint64 RequestCount = 0;
    uint64 TransferredBytes = 0;
    FString LastBudgetFailure;
};

class FObservedPreflightTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    explicit FObservedPreflightTransport(FRequestBudget& InBudget) : Budget(InBudget) {}

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>&) override
    {
        SkiPreparation::HttpAcquisitionResult Result;
        FString Error;
        if (!Budget.Send(Request, Result, Error)) return Result;
        if (!bSawFirstResponse)
        {
            bSawFirstResponse = true;
            PinnedETag = Result.ETag;
        }
        return Result;
    }

    FString PinnedETag;
    bool bSawFirstResponse = false;

private:
    FRequestBudget& Budget;
};

class FEtagPinnedRangeSource final : public SkiPreparation::ICogByteSource
{
public:
    FEtagPinnedRangeSource(FRequestBudget& InBudget, const FString& InUrl, const FString& InETag,
        const uint64 InObjectBytes, const uint32 InBlockBytes, const uint64 InCacheBytes,
        const uint64 InMaximumReadBytes)
        : Budget(InBudget), Url(InUrl), PinnedETag(InETag), ObjectBytes(InObjectBytes),
          BlockBytes(InBlockBytes), MaximumCacheBytes(InCacheBytes), MaximumReadBytes(InMaximumReadBytes) {}

    uint64 Size() const noexcept override { return ObjectBytes; }

    bool Read(const uint64 Offset, const uint64 Length, TArray<uint8>& OutBytes,
        FString& OutError) override
    {
        OutBytes.Reset();
        if (Error.Len() > 0)
        {
            OutError = Error;
            return false;
        }
        if (Length == 0 || Length > MaximumReadBytes || Length > MAX_int32
            || Offset > ObjectBytes || Length > ObjectBytes - Offset)
        {
            OutError = TEXT("COG_SAMPLER_RANGE_READ_INVALID");
            return false;
        }
        OutBytes.Reserve(static_cast<int32>(Length));
        uint64 Position = Offset;
        uint64 Remaining = Length;
        while (Remaining > 0)
        {
            if (Budget.Cancellation->IsCancelled())
            {
                Error = TEXT("COG_SAMPLER_CANCELLED");
                OutError = Error;
                OutBytes.Reset();
                return false;
            }
            const uint64 BlockOffset = (Position / BlockBytes) * BlockBytes;
            const uint64 BlockLength = FMath::Min<uint64>(BlockBytes, ObjectBytes - BlockOffset);
            TArray<uint8>* Block = Cache.Find(BlockOffset);
            if (!Block)
            {
                TArray<uint8> Fetched;
                if (!Fetch(BlockOffset, BlockLength, Fetched, OutError))
                {
                    OutBytes.Reset();
                    return false;
                }
                InsertCache(BlockOffset, MoveTemp(Fetched));
                Block = Cache.Find(BlockOffset);
                if (!Block)
                {
                    // The block can be larger than the cache allowance. Copy from this read's
                    // temporary directly rather than retaining it.
                    OutError = TEXT("COG_SAMPLER_RANGE_CACHE_TOO_SMALL");
                    OutBytes.Reset();
                    return false;
                }
            }
            else Touch(BlockOffset);
            const uint64 Within = Position - BlockOffset;
            const uint64 Count = FMath::Min(Remaining, BlockLength - Within);
            OutBytes.Append(Block->GetData() + Within, static_cast<int32>(Count));
            Position += Count;
            Remaining -= Count;
        }
        return static_cast<uint64>(OutBytes.Num()) == Length;
    }

    const FString& LastError() const noexcept { return Error; }

private:
    bool Fetch(const uint64 Offset, const uint64 Length, TArray<uint8>& OutBytes, FString& OutError)
    {
        OutBytes.Reset();
        if (Length == 0 || Length > MAX_int32 || Length > MaximumReadBytes
            || Offset > MAX_uint64 - (Length - 1))
        {
            Error = TEXT("COG_SAMPLER_RANGE_INVALID");
            OutError = Error;
            return false;
        }
        SkiPreparation::HttpAcquisitionRequest Request;
        Request.Url = Url;
        Request.Product = SkiPreparation::ProviderProduct::CoreElevation;
        Request.MaximumResponseBytes = Length;
        Request.TotalTimeoutSeconds = 30.0F;
        Request.ActivityTimeoutSeconds = 15.0F;
        Request.ByteRange = SkiPreparation::HttpByteRange{Offset, Length};
        Request.IfMatchETag = PinnedETag;
        SkiPreparation::HttpAcquisitionResult Result;
        if (!Budget.Send(Request, Result, OutError))
        {
            Error = Budget.LastBudgetFailure.IsEmpty() ? OutError : Budget.LastBudgetFailure;
            OutError = Error;
            return false;
        }
        if (Result.HttpStatus == 412)
        {
            Error = TEXT("COG_SAMPLER_ETAG_PRECONDITION_FAILED");
            OutError = Error;
            return false;
        }
        if (Result.FailureReason != SkiPreparation::TransportFailureReason::None
            || Result.HttpStatus != 206 || Result.Bytes.IsEmpty())
        {
            Error = TEXT("COG_SAMPLER_RANGE_REQUEST_FAILED");
            OutError = Error;
            return false;
        }
        if (Result.ETag != PinnedETag)
        {
            Error = Result.ETag.IsEmpty() ? TEXT("COG_SAMPLER_ETAG_MISSING") : TEXT("COG_SAMPLER_ETAG_CHANGED");
            OutError = Error;
            return false;
        }
        uint64 TotalBytes = 0;
        if (static_cast<uint64>(Result.Bytes.Num()) != Length
            || (Result.BytesReceived != 0 && Result.BytesReceived != Length)
            || !SkiPreparation::ValidateContentRange(Result.ContentRange,
                Request.ByteRange.GetValue(), static_cast<uint64>(Result.Bytes.Num()), TotalBytes)
            || TotalBytes != ObjectBytes)
        {
            Error = TEXT("COG_SAMPLER_CONTENT_RANGE_INVALID");
            OutError = Error;
            return false;
        }
        OutBytes = MoveTemp(Result.Bytes);
        return true;
    }

    void Touch(const uint64 Offset)
    {
        CacheOrder.Remove(Offset);
        CacheOrder.Add(Offset);
    }

    void InsertCache(const uint64 Offset, TArray<uint8>&& Bytes)
    {
        if (Bytes.Num() == 0 || static_cast<uint64>(Bytes.Num()) > MaximumCacheBytes) return;
        while (CachedBytes > MaximumCacheBytes - static_cast<uint64>(Bytes.Num()) && !CacheOrder.IsEmpty())
        {
            const uint64 Oldest = CacheOrder[0];
            CacheOrder.RemoveAt(0);
            if (const TArray<uint8>* Existing = Cache.Find(Oldest)) CachedBytes -= Existing->Num();
            Cache.Remove(Oldest);
        }
        CachedBytes += Bytes.Num();
        Cache.Add(Offset, MoveTemp(Bytes));
        Touch(Offset);
    }

    FRequestBudget& Budget;
    FString Url;
    FString PinnedETag;
    uint64 ObjectBytes = 0;
    uint32 BlockBytes = 0;
    uint64 MaximumCacheBytes = 0;
    uint64 MaximumReadBytes = 0;
    uint64 CachedBytes = 0;
    TMap<uint64, TArray<uint8>> Cache;
    TArray<uint64> CacheOrder;
    FString Error;
};

struct FCogTiffHandle
{
    SkiPreparation::ICogByteSource* Source = nullptr;
    uint64 Position = 0;
    FString Error;
};

tmsize_t ReadCog(thandle_t Handle, void* Destination, const tmsize_t Requested)
{
    FCogTiffHandle& Reader = *static_cast<FCogTiffHandle*>(Handle);
    if (!Reader.Source || !Destination || Requested <= 0) return 0;
    const uint64 ObjectBytes = Reader.Source->Size();
    if (Reader.Position >= ObjectBytes)
    {
        Reader.Error = TEXT("COG_SAMPLER_READ_OUT_OF_BOUNDS");
        return 0;
    }
    const uint64 Length = FMath::Min<uint64>(static_cast<uint64>(Requested), ObjectBytes - Reader.Position);
    TArray<uint8> Bytes;
    if (!Reader.Source->Read(Reader.Position, Length, Bytes, Reader.Error)
        || static_cast<uint64>(Bytes.Num()) != Length)
        return 0;
    FMemory::Memcpy(Destination, Bytes.GetData(), static_cast<SIZE_T>(Length));
    Reader.Position += Length;
    return static_cast<tmsize_t>(Length);
}

tmsize_t RejectCogWrite(thandle_t, void*, tmsize_t) { return 0; }

toff_t SeekCog(thandle_t Handle, const toff_t Offset, const int Origin)
{
    FCogTiffHandle& Reader = *static_cast<FCogTiffHandle*>(Handle);
    if (!Reader.Source) return static_cast<toff_t>(-1);
    const uint64 ObjectBytes = Reader.Source->Size();
    uint64 Base = 0;
    if (Origin == SEEK_SET) Base = 0;
    else if (Origin == SEEK_CUR) Base = Reader.Position;
    else if (Origin == SEEK_END) Base = ObjectBytes;
    else return static_cast<toff_t>(-1);
    if (static_cast<uint64>(Offset) > MAX_uint64 - Base
        || Base + static_cast<uint64>(Offset) > ObjectBytes)
    {
        Reader.Error = TEXT("COG_SAMPLER_SEEK_OUT_OF_BOUNDS");
        return static_cast<toff_t>(-1);
    }
    Reader.Position = Base + static_cast<uint64>(Offset);
    return static_cast<toff_t>(Reader.Position);
}

int CloseCog(thandle_t) { return 0; }
toff_t SizeCog(thandle_t Handle)
{
    const FCogTiffHandle& Reader = *static_cast<FCogTiffHandle*>(Handle);
    return Reader.Source ? static_cast<toff_t>(Reader.Source->Size()) : 0;
}
int RejectCogMap(thandle_t, void**, toff_t*) { return 0; }
void RejectUnmap(thandle_t, void*, toff_t) {}

struct FDecodedCogTile
{
    TArray<float> Heights;
    uint64 DecodedBytes = 0;
    uint64 BytesRead = 0;
};

class FCogSourceReader final
{
public:
    FCogSourceReader(const SkiPreparation::FCogTerrainSamplerSource& InSource,
        FRequestBudget& InBudget,
        const SkiPreparation::FCogPreflightReport& InPreflight,
        const FString& InPinnedETag, const uint64 InRangeCacheBytes,
        const uint64 InDecodedCacheBytes,
        const uint64 InMaximumReadBytes, const SkiPreparation::FCogTerrainSamplerLimits& InLimits)
        : Definition(InSource), Budget(InBudget), Preflight(InPreflight),
          PinnedETag(InPinnedETag), RangeCacheBytes(InRangeCacheBytes),
          DecodedCacheBytesLimit(InDecodedCacheBytes), MaximumReadBytes(InMaximumReadBytes),
          Limits(InLimits) {}

    ~FCogSourceReader()
    {
        if (Image) TIFFClose(Image);
    }

    bool Initialize(FString& OutError)
    {
        if (!Preflight.bPassed || !Preflight.bStrongETagPinned || Preflight.ObjectBytes == 0
            || !IsSafeStrongETag(PinnedETag) || Preflight.Directories.IsEmpty())
        {
            OutError = TEXT("COG_SAMPLER_PREFLIGHT_PROOF_INVALID");
            return false;
        }
        if (Preflight.Width < 2 || Preflight.Height < 2 || Preflight.TileWidth == 0 || Preflight.TileHeight == 0)
        {
            OutError = TEXT("COG_SAMPLER_PREFLIGHT_LAYOUT_INVALID");
            return false;
        }
        if (static_cast<uint64>(Preflight.TileWidth) * Preflight.TileHeight * sizeof(float)
                > Limits.MaxDecodedTileBytes
            || static_cast<uint64>(Preflight.TileWidth) * Preflight.TileHeight * sizeof(float)
                > DecodedCacheBytesLimit)
        {
            OutError = TEXT("COG_SAMPLER_DECODED_TILE_BUDGET_EXCEEDED");
            return false;
        }

        ByteSource = std::make_unique<FEtagPinnedRangeSource>(Budget, Definition.Url, PinnedETag,
            Preflight.ObjectBytes, Limits.Preflight.RangeBlockBytes, RangeCacheBytes, MaximumReadBytes);
        TiffHandle.Source = ByteSource.get();
        SkiPreparation::EnsureGeoTiffTagsRegistered();
        Image = TIFFClientOpen("MountainPlannerCogTerrainSampler", "r", &TiffHandle,
            ReadCog, RejectCogWrite, SeekCog, CloseCog, SizeCog, RejectCogMap, RejectUnmap);
        if (!Image)
        {
            OutError = ByteSource->LastError().IsEmpty() ? TiffHandle.Error : ByteSource->LastError();
            if (OutError.IsEmpty()) OutError = TEXT("COG_SAMPLER_TIFF_OPEN_FAILED");
            return false;
        }

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
            OutError = TiffHandle.Error.IsEmpty() ? TEXT("COG_SAMPLER_REQUIRED_TAG_MISSING") : TiffHandle.Error;
            return false;
        }
        if (Width != Preflight.Width || Height != Preflight.Height
            || TileWidth != Preflight.TileWidth || TileHeight != Preflight.TileHeight
            || Bits != 32 || Samples != 1 || SampleFormat != SAMPLEFORMAT_IEEEFP
            || Planar != PLANARCONFIG_CONTIG || Orientation != ORIENTATION_TOPLEFT
            || Compression != Preflight.Directories[0].Compression
            || Predictor != Preflight.Directories[0].Predictor)
        {
            OutError = TEXT("COG_SAMPLER_PREFLIGHT_HEADER_CHANGED");
            return false;
        }
        SourceWidth = Width;
        SourceHeight = Height;
        SourceTileWidth = TileWidth;
        SourceTileHeight = TileHeight;

        uint32 NoDataCount = 0;
        char* NoDataText = nullptr;
        double HeaderNoData = 0.0;
        double ProofNoData = 0.0;
        if (!TIFFGetField(Image, TIFFTAG_GDAL_NODATA, &NoDataCount, &NoDataText)
            || !NoDataText || !ParseFiniteNumber(Preflight.NoDataText, ProofNoData))
        {
            OutError = TEXT("COG_SAMPLER_NODATA_MISSING");
            return false;
        }
        const FString HeaderNoDataText = FString(UTF8_TO_TCHAR(NoDataText)).TrimStartAndEnd();
        if (!ParseFiniteNumber(HeaderNoDataText, HeaderNoData) || HeaderNoData != ProofNoData)
        {
            OutError = TEXT("COG_SAMPLER_NODATA_CHANGED");
            return false;
        }
        NoDataValue = HeaderNoData;

        const uint64 TileCount = static_cast<uint64>(TIFFNumberOfTiles(Image));
        toff_t* Offsets = nullptr;
        uint64* ByteCounts = nullptr;
        if (TileCount != Preflight.Directories[0].TileCount
            || !TIFFGetField(Image, TIFFTAG_TILEOFFSETS, &Offsets)
            || !TIFFGetField(Image, TIFFTAG_TILEBYTECOUNTS, &ByteCounts)
            || !Offsets || !ByteCounts)
        {
            OutError = TEXT("COG_SAMPLER_TILE_INDEX_INVALID");
            return false;
        }
        for (uint64 Tile = 0; Tile < TileCount; ++Tile)
        {
            const uint64 EncodedBytes = ByteCounts[Tile];
            const uint64 Offset = static_cast<uint64>(Offsets[Tile]);
            if (EncodedBytes == 0 || EncodedBytes > Limits.MaxEncodedTileBytes
                || EncodedBytes > MaximumReadBytes || Offset < 8 || Offset > Preflight.ObjectBytes
                || EncodedBytes > Preflight.ObjectBytes - Offset)
            {
                OutError = TEXT("COG_SAMPLER_ENCODED_TILE_BUDGET_EXCEEDED");
                return false;
            }
        }
        const tmsize_t TileSize = TIFFTileSize(Image);
        const tmsize_t RowSize = TIFFTileRowSize(Image);
        if (TileSize <= 0 || static_cast<uint64>(TileSize) > Limits.MaxDecodedTileBytes
            || static_cast<uint64>(TileSize) > DecodedCacheBytesLimit
            || RowSize < static_cast<tmsize_t>(static_cast<uint64>(TileWidth) * sizeof(float)))
        {
            OutError = TEXT("COG_SAMPLER_TILE_LAYOUT_INVALID");
            return false;
        }
        DecodedTileBytes = static_cast<uint64>(TileSize);
        TileRowBytes = RowSize;
        return true;
    }

    bool SampleBilinear(const double Column, const double Row, double& OutHeight, FString& OutError)
    {
        OutHeight = 0.0;
        if (!FMath::IsFinite(Column) || !FMath::IsFinite(Row)
            || Column < 0.0 || Row < 0.0
            || Column >= static_cast<double>(SourceWidth - 1U)
            || Row >= static_cast<double>(SourceHeight - 1U))
            return false;
        const uint32 Column0 = static_cast<uint32>(FMath::FloorToDouble(Column));
        const uint32 Row0 = static_cast<uint32>(FMath::FloorToDouble(Row));
        const uint32 Column1 = Column0 + 1U;
        const uint32 Row1 = Row0 + 1U;
        float V00 = 0.0F, V10 = 0.0F, V01 = 0.0F, V11 = 0.0F;
        if (!ReadPixel(Column0, Row0, V00, OutError)
            || !ReadPixel(Column1, Row0, V10, OutError)
            || !ReadPixel(Column0, Row1, V01, OutError)
            || !ReadPixel(Column1, Row1, V11, OutError))
        {
            if (!OutError.IsEmpty()) return false;
            return false;
        }
        if (!IsValidHeight(V00) || !IsValidHeight(V10) || !IsValidHeight(V01) || !IsValidHeight(V11))
            return false;
        const double Fx = Column - Column0;
        const double Fy = Row - Row0;
        OutHeight = (1.0 - Fy) * ((1.0 - Fx) * V00 + Fx * V10)
            + Fy * ((1.0 - Fx) * V01 + Fx * V11);
        return FMath::IsFinite(OutHeight);
    }

private:
    static bool IsValidHeight(const float Value) { return FMath::IsFinite(Value); }

    bool ReadPixel(const uint32 Column, const uint32 Row, float& OutValue, FString& OutError)
    {
        OutValue = 0.0F;
        const uint32 TileX = Column / SourceTileWidth;
        const uint32 TileY = Row / SourceTileHeight;
        const uint32 TileId = static_cast<uint32>(TIFFComputeTile(Image, Column, Row, 0, 0));
        FDecodedCogTile* Tile = DecodedTiles.Find(TileId);
        if (!Tile)
        {
            const uint64 Required = DecodedTileBytes;
            if (Required > Limits.MaxDecodedTileBytes || Required > DecodedCacheBytesLimit
                || Required > MAX_int32)
            {
                OutError = TEXT("COG_SAMPLER_DECODED_TILE_BUDGET_EXCEEDED");
                return false;
            }
            while (CachedDecodedBytes > DecodedCacheBytesLimit - Required && !DecodedTileLru.IsEmpty())
            {
                const uint32 Oldest = DecodedTileLru[0];
                DecodedTileLru.RemoveAt(0);
                if (const FDecodedCogTile* Existing = DecodedTiles.Find(Oldest))
                    CachedDecodedBytes -= Existing->DecodedBytes;
                DecodedTiles.Remove(Oldest);
            }
            FDecodedCogTile NewTile;
            NewTile.Heights.SetNumUninitialized(static_cast<int32>(Required / sizeof(float)));
            const tmsize_t Read = TIFFReadEncodedTile(Image, TileId,
                reinterpret_cast<uint8*>(NewTile.Heights.GetData()), static_cast<tmsize_t>(Required));
            if (Read < 0 || static_cast<uint64>(Read) > Required)
            {
                OutError = ByteSource->LastError().IsEmpty()
                    ? TEXT("COG_SAMPLER_TILE_DECODE_FAILED") : ByteSource->LastError();
                return false;
            }
            if (!ByteSource->LastError().IsEmpty())
            {
                OutError = ByteSource->LastError();
                return false;
            }
            NewTile.DecodedBytes = Required;
            NewTile.BytesRead = static_cast<uint64>(Read);
            CachedDecodedBytes += Required;
            DecodedTiles.Add(TileId, MoveTemp(NewTile));
            DecodedTileLru.Add(TileId);
            Tile = DecodedTiles.Find(TileId);
        }
        else
        {
            DecodedTileLru.Remove(TileId);
            DecodedTileLru.Add(TileId);
        }
        if (!Tile) return false;
        const uint32 TileLocalColumn = Column - TileX * SourceTileWidth;
        const uint32 TileLocalRow = Row - TileY * SourceTileHeight;
        const uint64 Index = static_cast<uint64>(TileLocalRow) * SourceTileWidth + TileLocalColumn;
        if (Index >= Tile->Heights.Num() || (Index + 1ULL) * sizeof(float) > Tile->BytesRead)
        {
            OutError = TEXT("COG_SAMPLER_TRUNCATED_DECODED_TILE");
            return false;
        }
        OutValue = Tile->Heights[static_cast<int32>(Index)];
        if (OutValue == static_cast<float>(NoDataValue)) OutValue = std::numeric_limits<float>::quiet_NaN();
        return true;
    }

    const SkiPreparation::FCogTerrainSamplerSource& Definition;
    FRequestBudget& Budget;
    const SkiPreparation::FCogPreflightReport& Preflight;
    FString PinnedETag;
    uint64 RangeCacheBytes = 0;
    uint64 DecodedCacheBytesLimit = 0;
    uint64 MaximumReadBytes = 0;
    const SkiPreparation::FCogTerrainSamplerLimits& Limits;
    std::unique_ptr<FEtagPinnedRangeSource> ByteSource;
    FCogTiffHandle TiffHandle;
    TIFF* Image = nullptr;
    uint32 SourceWidth = 0;
    uint32 SourceHeight = 0;
    uint32 SourceTileWidth = 0;
    uint32 SourceTileHeight = 0;
    double NoDataValue = 0.0;
    tmsize_t TileRowBytes = 0;
    uint64 DecodedTileBytes = 0;
    uint64 CachedDecodedBytes = 0;
    TMap<uint32, FDecodedCogTile> DecodedTiles;
    TArray<uint32> DecodedTileLru;
};

bool ProductMatchesTerrainSource(const SkiDomain::TerrainCoreSource& Source,
    const SkiDomain::ElevationProduct Product)
{
    const FString Actual = UTF8_TO_TCHAR(Source.Product.c_str());
    return Actual == UTF8_TO_TCHAR(ProductName(Product));
}

bool ResolveSampleProvenance(const SkiPreparation::FCogTerrainSamplerSource& Source,
    const double SourceColumn, const double SourceRow,
    SkiPreparation::TerrainScratchProvenance& OutProvenance, FString& OutError)
{
    using SkiPreparation::TerrainScratchProvenance;
    switch (Source.Product)
    {
    case SkiDomain::ElevationProduct::S1M:
        if (!Source.ResolveS1mProvenance
            || !Source.ResolveS1mProvenance(SourceColumn, SourceRow, OutProvenance))
        {
            OutError = TEXT("COG_SAMPLER_S1M_PROVENANCE_UNAVAILABLE");
            return false;
        }
        if (OutProvenance != TerrainScratchProvenance::S1MNative
            && OutProvenance != TerrainScratchProvenance::S1MBlend
            && OutProvenance != TerrainScratchProvenance::S1MBackfill
            && OutProvenance != TerrainScratchProvenance::S1MInterpolated)
        {
            OutError = TEXT("COG_SAMPLER_S1M_PROVENANCE_INVALID");
            return false;
        }
        return true;
    case SkiDomain::ElevationProduct::Project1m:
        OutProvenance = TerrainScratchProvenance::Project1m;
        return true;
    case SkiDomain::ElevationProduct::ArcSec13:
        OutProvenance = TerrainScratchProvenance::ArcSec13;
        return true;
    default:
        OutError = TEXT("COG_SAMPLER_PRODUCT_UNSUPPORTED");
        return false;
    }
}
}

bool SkiPreparation::SampleVerifiedElevationCogsToScratch(IAcquisitionTransport& Transport,
    const TArray<FCogTerrainSamplerSource>& Sources, TerrainScratchStore& Scratch,
    const FCogTerrainSamplerLimits& Limits, const TSharedRef<Cancellation>& Cancellation,
    FCogTerrainSamplerReport& OutReport)
{
    OutReport = {};
    FRequestBudget* BudgetForFailure = nullptr;
    const auto Fail = [&](const TCHAR* Code, const FString& Detail)
    {
        SetFailure(OutReport, Code, Detail);
        if (BudgetForFailure)
        {
            OutReport.Requests = BudgetForFailure->RequestCount;
            OutReport.TransferredBytes = BudgetForFailure->TransferredBytes;
        }
        return false;
    };
    if (Cancellation->IsCancelled()) return Fail(TEXT("COG_SAMPLER_CANCELLED"), TEXT("Sampling was cancelled before preflight."));
    if (Sources.IsEmpty() || Sources.Num() > SkiDomain::TerrainCoreMaxAdditionalSources + 1
        || Scratch.Width() == 0 || Scratch.Height() == 0 || Scratch.EastSpacingM() != 1.0
        || Scratch.NorthSpacingM() != 1.0 || Scratch.IsFinalized())
        return Fail(TEXT("COG_SAMPLER_REQUEST_INVALID"), TEXT("The resolved sources or canonical scratch store are invalid."));
    if (Limits.MaxRequests == 0 || Limits.MaxTransferredBytes == 0
        || Limits.MaxResidentBytes <= OutputTileBytes || Limits.MaxOutputSamples == 0
        || Limits.MaxEncodedTileBytes == 0 || Limits.MaxDecodedTileBytes == 0
        || Limits.Preflight.RangeBlockBytes == 0
        || Limits.Preflight.MaxRangeBytes < Limits.Preflight.RangeBlockBytes
        || Limits.Preflight.MaxRangeBytes > MAX_int32
        || Limits.Preflight.RangeBlockBytes > MAX_int32)
        return Fail(TEXT("COG_SAMPLER_LIMITS_INVALID"), TEXT("Sampler budgets are internally inconsistent."));
    if (Limits.Preflight.MaxCachedBytes > Limits.MaxResidentBytes)
        return Fail(TEXT("COG_SAMPLER_MEMORY_BUDGET_EXCEEDED"), TEXT("Preflight's range cache exceeds the aggregate resident-memory allowance."));

    uint64 RequiredScratchBytes = 0;
    if (!TerrainScratchStore::TryCalculateRequiredStorageBytes(Scratch.Width(), Scratch.Height(),
            RequiredScratchBytes)
        || RequiredScratchBytes > Scratch.RequiredStorageBytes()
        || static_cast<uint64>(Scratch.Width()) * Scratch.Height() > Limits.MaxOutputSamples)
        return Fail(TEXT("COG_SAMPLER_OUTPUT_BUDGET_EXCEEDED"), TEXT("Canonical LOD0 output exceeds its sample or storage budget."));

    const TArray<SkiDomain::TerrainCoreSource>& ScratchSources = Scratch.Sources();
    TSet<uint8> SeenSourceIndices;
    int32 PreviousPriority = -1;
    for (const FCogTerrainSamplerSource& Source : Sources)
    {
        if (Source.Url.IsEmpty() || !Source.MapTargetToSourcePixel
            || Source.ScratchSourceIndex == TerrainScratchNoSourceIndex
            || static_cast<int32>(Source.ScratchSourceIndex) >= ScratchSources.Num()
            || SeenSourceIndices.Contains(Source.ScratchSourceIndex)
            || ProductPriority(Source.Product) < PreviousPriority
            || !ProductMatchesTerrainSource(ScratchSources[Source.ScratchSourceIndex], Source.Product))
            return Fail(TEXT("COG_SAMPLER_SOURCE_CHAIN_INVALID"), TEXT("The source list is not a compatible resolved fallback chain."));
        if (Source.Product == SkiDomain::ElevationProduct::S1M && !Source.ResolveS1mProvenance)
            return Fail(TEXT("COG_SAMPLER_S1M_PROVENANCE_REQUIRED"), TEXT("S1M needs verified source-area classification before sampling."));
        SeenSourceIndices.Add(Source.ScratchSourceIndex);
        PreviousPriority = ProductPriority(Source.Product);
    }

    const uint64 SourceBudget = (Limits.MaxResidentBytes - OutputTileBytes) / static_cast<uint64>(Sources.Num());
    const uint64 RangeCacheBudget = SourceBudget / 4ULL;
    const uint64 DecodedCacheBudget = SourceBudget / 4ULL;
    const uint64 MaximumReadBytes = FMath::Min<uint64>(Limits.MaxEncodedTileBytes, SourceBudget / 4ULL);
    if (RangeCacheBudget < Limits.Preflight.RangeBlockBytes
        || MaximumReadBytes < Limits.Preflight.RangeBlockBytes)
        return Fail(TEXT("COG_SAMPLER_MEMORY_BUDGET_EXCEEDED"), TEXT("Per-source range and decoded-tile caches do not fit the resident-memory budget."));

    FRequestBudget Budget(Transport, Cancellation, Limits.MaxRequests, Limits.MaxTransferredBytes);
    BudgetForFailure = &Budget;
    struct FVerifiedSource
    {
        FCogPreflightReport Preflight;
        FString ETag;
    };
    TArray<FVerifiedSource> Verified;
    Verified.Reserve(Sources.Num());
    for (const FCogTerrainSamplerSource& Source : Sources)
    {
        if (Cancellation->IsCancelled())
            return Fail(TEXT("COG_SAMPLER_CANCELLED"), TEXT("Sampling was cancelled during COG preflight."));
        if (Budget.RequestCount >= Budget.MaximumRequests
            || Budget.TransferredBytes >= Budget.MaximumBytes)
            return Fail(TEXT("COG_SAMPLER_BUDGET_EXCEEDED"), TEXT("Aggregate acquisition budget was exhausted before source preflight."));

        FCogPreflightLimits PreflightLimits = Limits.Preflight;
        PreflightLimits.MaxRequests = FMath::Min<uint64>(PreflightLimits.MaxRequests,
            Budget.MaximumRequests - Budget.RequestCount);
        PreflightLimits.MaxTransferredBytes = FMath::Min<uint64>(PreflightLimits.MaxTransferredBytes,
            Budget.MaximumBytes - Budget.TransferredBytes);
        if (PreflightLimits.MaxRequests == 0 || PreflightLimits.MaxTransferredBytes < 16)
            return Fail(TEXT("COG_SAMPLER_BUDGET_EXCEEDED"), TEXT("Aggregate acquisition budget cannot perform another bounded preflight."));

        FObservedPreflightTransport Observed(Budget);
        FVerifiedSource Proof;
        if (!PreflightElevationCog(Observed, Source.Url, Source.Product, Source.Metadata,
                PreflightLimits, Cancellation, Proof.Preflight))
        {
            const FString Code = Budget.LastBudgetFailure.IsEmpty()
                ? Proof.Preflight.FailureCode : Budget.LastBudgetFailure;
            return Fail(Code.IsEmpty() ? TEXT("COG_SAMPLER_PREFLIGHT_FAILED") : *Code,
                Proof.Preflight.FailureDetail);
        }
        if (!Observed.bSawFirstResponse || !Proof.Preflight.bStrongETagPinned
            || !IsSafeStrongETag(Observed.PinnedETag))
            return Fail(TEXT("COG_SAMPLER_STRONG_ETAG_REQUIRED"), TEXT("Preflight did not establish one strong ETag for this source."));
        Proof.ETag = Observed.PinnedETag;
        Verified.Add(MoveTemp(Proof));
    }

    std::vector<std::unique_ptr<FCogSourceReader>> Readers;
    Readers.reserve(Sources.Num());
    for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
    {
        const FCogTerrainSamplerSource& Source = Sources[SourceIndex];
        std::unique_ptr<FCogSourceReader> Reader = std::make_unique<FCogSourceReader>(
            Source, Budget, Verified[SourceIndex].Preflight, Verified[SourceIndex].ETag,
            RangeCacheBudget, DecodedCacheBudget, MaximumReadBytes, Limits);
        FString Error;
        if (!Reader->Initialize(Error))
        {
            const TCHAR* Code = Error == TEXT("COG_SAMPLER_CANCELLED") ? TEXT("COG_SAMPLER_CANCELLED")
                : TEXT("COG_SAMPLER_COG_READER_FAILED");
            return Fail(Code, Error);
        }
        Readers.push_back(std::move(Reader));
    }

    const uint32 Width = Scratch.Width();
    const uint32 Height = Scratch.Height();
    const uint32 TilesX = FMath::DivideAndRoundUp(Width, TerrainScratchTileSamples);
    const uint32 TilesY = FMath::DivideAndRoundUp(Height, TerrainScratchTileSamples);
    OutReport.TotalSamples = static_cast<uint64>(Width) * Height;
    OutReport.SamplesBySource.SetNumZeroed(Sources.Num());
    OutReport.SamplesByProvenance.SetNumZeroed(7);

    for (uint32 TileY = 0; TileY < TilesY; ++TileY)
    {
        const uint32 StartRow = TileY * TerrainScratchTileSamples;
        const uint32 TileHeight = FMath::Min(TerrainScratchTileSamples, Height - StartRow);
        for (uint32 TileX = 0; TileX < TilesX; ++TileX)
        {
            if (Cancellation->IsCancelled())
                return Fail(TEXT("COG_SAMPLER_CANCELLED"), TEXT("Sampling was cancelled before the next canonical scratch tile."));
            const uint32 StartColumn = TileX * TerrainScratchTileSamples;
            const uint32 TileWidth = FMath::Min(TerrainScratchTileSamples, Width - StartColumn);
            const int32 TileSampleCount = static_cast<int32>(static_cast<uint64>(TileWidth) * TileHeight);
            TArray<float> Heights;
            TArray<uint8> Validity, Provenance, SourceIndices;
            Heights.Init(0.0F, TileSampleCount);
            Validity.Init(0, TileSampleCount);
            Provenance.Init(static_cast<uint8>(TerrainScratchProvenance::NoData), TileSampleCount);
            SourceIndices.Init(TerrainScratchNoSourceIndex, TileSampleCount);

            for (uint32 LocalRow = 0; LocalRow < TileHeight; ++LocalRow)
            {
                if (Cancellation->IsCancelled())
                    return Fail(TEXT("COG_SAMPLER_CANCELLED"), TEXT("Sampling was cancelled while producing a canonical scratch tile."));
                const uint32 TargetRow = StartRow + LocalRow;
                for (uint32 LocalColumn = 0; LocalColumn < TileWidth; ++LocalColumn)
                {
                    const uint32 TargetColumn = StartColumn + LocalColumn;
                    const int32 OutputIndex = static_cast<int32>(static_cast<uint64>(LocalRow) * TileWidth + LocalColumn);
                    bool bSampled = false;
                    for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
                    {
                        const FCogTerrainSamplerSource& Source = Sources[SourceIndex];
                        double SourceColumn = 0.0, SourceRow = 0.0;
                        if (!Source.MapTargetToSourcePixel(TargetColumn, TargetRow, SourceColumn, SourceRow)) continue;
                        double HeightValue = 0.0;
                        FString Error;
                        if (!Readers[SourceIndex]->SampleBilinear(SourceColumn, SourceRow, HeightValue, Error))
                        {
                            if (!Error.IsEmpty())
                            {
                                const TCHAR* Code = Error == TEXT("COG_SAMPLER_CANCELLED")
                                    ? TEXT("COG_SAMPLER_CANCELLED") : TEXT("COG_SAMPLER_SOURCE_READ_FAILED");
                                return Fail(Code, Error);
                            }
                            continue;
                        }
                        if (!FMath::IsFinite(HeightValue)
                            || FMath::Abs(HeightValue) > static_cast<double>(MAX_flt))
                        {
                            return Fail(TEXT("COG_SAMPLER_INTERPOLATION_INVALID"), TEXT("A valid source sample produced a non-finite or unrepresentable height."));
                        }
                        TerrainScratchProvenance SampleProvenance = TerrainScratchProvenance::NoData;
                        if (!ResolveSampleProvenance(Source, SourceColumn, SourceRow, SampleProvenance, Error))
                            return Fail(Error.IsEmpty() ? TEXT("COG_SAMPLER_PROVENANCE_INVALID") : *Error,
                                TEXT("A selected source sample has no valid per-sample provenance."));
                        const int32 ProvenanceIndex = ProvenanceCounterIndex(SampleProvenance);
                        if (ProvenanceIndex < 0 || SampleProvenance == TerrainScratchProvenance::NoData)
                            return Fail(TEXT("COG_SAMPLER_PROVENANCE_INVALID"), TEXT("A valid height cannot be labelled NoData or an unknown provenance class."));
                        Heights[OutputIndex] = static_cast<float>(HeightValue);
                        Validity[OutputIndex] = 1;
                        Provenance[OutputIndex] = static_cast<uint8>(SampleProvenance);
                        SourceIndices[OutputIndex] = Source.ScratchSourceIndex;
                        ++OutReport.ValidSamples;
                        ++OutReport.SamplesBySource[SourceIndex];
                        ++OutReport.SamplesByProvenance[ProvenanceIndex];
                        bSampled = true;
                        break;
                    }
                    if (!bSampled)
                    {
                        ++OutReport.NoDataSamples;
                        ++OutReport.SamplesByProvenance[6];
                    }
                }
            }
            FString WriteError;
            if (!Scratch.WriteLod0Tile(TileX, TileY, Heights, TileWidth,
                    Validity, TileWidth, Provenance, TileWidth, SourceIndices, TileWidth,
                    WriteError, &Cancellation.Get()))
            {
                const TCHAR* Code = Cancellation->IsCancelled()
                    ? TEXT("COG_SAMPLER_CANCELLED") : TEXT("COG_SAMPLER_SCRATCH_WRITE_FAILED");
                return Fail(Code, WriteError);
            }
        }
    }
    FString FinalizeError;
    if (!Scratch.Finalize(FinalizeError, &Cancellation.Get()))
    {
        const TCHAR* Code = Cancellation->IsCancelled()
            ? TEXT("COG_SAMPLER_CANCELLED") : TEXT("COG_SAMPLER_SCRATCH_FINALIZE_FAILED");
        return Fail(Code, FinalizeError);
    }
    OutReport.Requests = Budget.RequestCount;
    OutReport.TransferredBytes = Budget.TransferredBytes;
    OutReport.bPassed = true;
    OutReport.FailureCode.Empty();
    OutReport.FailureDetail.Empty();
    return true;
}
