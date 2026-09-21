#include "SkiPreparation/GeoTiffDecoder.h"

#include "SkiDomain/Coordinates.h"
#include "tiffio.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

namespace
{
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr int32 MaxMetadataTextBytes = 128;
constexpr int32 MaxElevationResponseBytes = 64 * 1024 * 1024;

TIFFExtendProc PreviousTagExtender = nullptr;
std::once_flag TagExtenderOnce;

void Fail(SkiPreparation::ProviderFailure& Failure, const TCHAR* Code, const TCHAR* Summary,
    const SkiPreparation::RetryClassification Retry = SkiPreparation::RetryClassification::NotRetryable)
{
    Failure.Code = Code;
    Failure.Stage = SkiPreparation::FailureStage::Decoding;
    Failure.Summary = Summary;
    Failure.Retry = Retry;
}

void RecordFieldShape(SkiPreparation::ProviderFailure& Failure, const uint32 Tag, const TIFFField* Field)
{
    Failure.MetadataTag = Tag;
    Failure.MetadataType = Field ? static_cast<int32>(TIFFFieldDataType(Field)) : 0;
    Failure.MetadataReadCount = Field ? TIFFFieldReadCount(Field) : 0;
    Failure.MetadataPassCount = Field && TIFFFieldPassCount(Field) != 0;
}

void GeoTagExtender(TIFF* Image)
{
    if (PreviousTagExtender) PreviousTagExtender(Image);
    static const TIFFFieldInfo Fields[] = {
        {ModelPixelScaleTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelPixelScaleTag")},
        {ModelTiepointTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelTiepointTag")},
        {GeoKeyDirectoryTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT, FIELD_CUSTOM, true, true,
            const_cast<char*>("GeoKeyDirectoryTag")},
        {TIFFTAG_GDAL_NODATA, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, true, true,
            const_cast<char*>("GDALNoDataValue")},
    };
    for (const TIFFFieldInfo& Field : Fields)
    {
        if (!TIFFFindField(Image, Field.field_tag, TIFF_ANY)) TIFFMergeFieldInfo(Image, &Field, 1);
    }
}

void EnsureGeoTagsRegistered()
{
    std::call_once(TagExtenderOnce, [] { PreviousTagExtender = TIFFSetTagExtender(GeoTagExtender); });
}

struct MemoryTiff
{
    const uint8* Data = nullptr;
    uint64 Size = 0;
    uint64 Position = 0;
};

tmsize_t ReadMemory(thandle_t Handle, void* Destination, const tmsize_t Requested)
{
    MemoryTiff& Source = *static_cast<MemoryTiff*>(Handle);
    if (!Destination || Requested <= 0 || Source.Position > Source.Size) return 0;
    const uint64 Available = Source.Size - Source.Position;
    const uint64 Count = FMath::Min<uint64>(Available, static_cast<uint64>(Requested));
    if (Count == 0 || Count > static_cast<uint64>(MAX_int64)) return 0;
    FMemory::Memcpy(Destination, Source.Data + Source.Position, static_cast<SIZE_T>(Count));
    Source.Position += Count;
    return static_cast<tmsize_t>(Count);
}

tmsize_t RejectWrite(thandle_t, void*, tmsize_t) { return 0; }

toff_t SeekMemory(thandle_t Handle, const toff_t Offset, const int Origin)
{
    MemoryTiff& Source = *static_cast<MemoryTiff*>(Handle);
    uint64 Base = 0;
    if (Origin == SEEK_SET) Base = 0;
    else if (Origin == SEEK_CUR) Base = Source.Position;
    else if (Origin == SEEK_END) Base = Source.Size;
    else return static_cast<toff_t>(-1);
    if (static_cast<uint64>(Offset) > MAX_uint64 - Base) return static_cast<toff_t>(-1);
    const uint64 Target = Base + static_cast<uint64>(Offset);
    if (Target > Source.Size) return static_cast<toff_t>(-1);
    Source.Position = Target;
    return static_cast<toff_t>(Target);
}

int CloseMemory(thandle_t) { return 0; }
toff_t SizeMemory(thandle_t Handle) { return static_cast<toff_t>(static_cast<MemoryTiff*>(Handle)->Size); }
int RejectMap(thandle_t, void**, toff_t*) { return 0; }
void UnmapMemory(thandle_t, void*, toff_t) {}

bool HasTiffSignature(const TArray<uint8>& Bytes)
{
    return Bytes.Num() >= 4
        && ((Bytes[0] == 'I' && Bytes[1] == 'I' && Bytes[2] == 42 && Bytes[3] == 0)
            || (Bytes[0] == 'M' && Bytes[1] == 'M' && Bytes[2] == 0 && Bytes[3] == 42));
}

enum class FieldResult : uint8 { Missing, Valid, Invalid };

template<typename Element>
FieldResult ReadArrayField(TIFF* Image, const uint32 Tag, const TIFFDataType ExpectedType,
    Element*& OutData, uint32& OutCount, SkiPreparation::ProviderFailure& Failure)
{
    OutData = nullptr;
    OutCount = 0;
    const TIFFField* Field = TIFFFieldWithTag(Image, Tag);
    if (!Field) return FieldResult::Missing;
    RecordFieldShape(Failure, Tag, Field);
    if (TIFFFieldDataType(Field) != ExpectedType) return FieldResult::Invalid;
    const int ReadCount = TIFFFieldReadCount(Field);
    int ReadOk = 0;
    if (TIFFFieldPassCount(Field))
    {
        if (ReadCount == TIFF_VARIABLE2)
        {
            uint32 Count = 0;
            ReadOk = TIFFGetField(Image, Tag, &Count, &OutData);
            OutCount = Count;
        }
        else if (ReadCount == TIFF_VARIABLE)
        {
            uint16 Count = 0;
            ReadOk = TIFFGetField(Image, Tag, &Count, &OutData);
            OutCount = Count;
        }
        else return FieldResult::Invalid;
    }
    else
    {
        ReadOk = TIFFGetField(Image, Tag, &OutData);
        if (ReadCount > 0) OutCount = static_cast<uint32>(ReadCount);
        else return ReadOk ? FieldResult::Invalid : FieldResult::Missing;
    }
    if (!ReadOk) return FieldResult::Missing;
    return OutData && OutCount > 0 ? FieldResult::Valid : FieldResult::Invalid;
}

FieldResult ReadAsciiField(TIFF* Image, const uint32 Tag, std::string& Out,
    SkiPreparation::ProviderFailure& Failure)
{
    Out.clear();
    const TIFFField* Field = TIFFFieldWithTag(Image, Tag);
    if (!Field) return FieldResult::Missing;
    RecordFieldShape(Failure, Tag, Field);
    if (TIFFFieldDataType(Field) != TIFF_ASCII) return FieldResult::Invalid;
    char* Text = nullptr;
    uint32 Count = 0;
    int ReadOk = 0;
    const int ReadCount = TIFFFieldReadCount(Field);
    if (TIFFFieldPassCount(Field))
    {
        if (ReadCount == TIFF_VARIABLE2)
        {
            uint32 ValueCount = 0;
            ReadOk = TIFFGetField(Image, Tag, &ValueCount, &Text);
            Count = ValueCount;
        }
        else if (ReadCount == TIFF_VARIABLE)
        {
            uint16 ValueCount = 0;
            ReadOk = TIFFGetField(Image, Tag, &ValueCount, &Text);
            Count = ValueCount;
        }
        else return FieldResult::Invalid;
    }
    else
    {
        ReadOk = TIFFGetField(Image, Tag, &Text);
        if (ReadOk && Text)
        {
            while (Count <= MaxMetadataTextBytes && Text[Count] != '\0') ++Count;
            if (Count <= MaxMetadataTextBytes) ++Count;
        }
    }
    if (!ReadOk) return FieldResult::Missing;
    if (!Text || Count == 0 || Count > MaxMetadataTextBytes + 1) return FieldResult::Invalid;
    uint32 Length = 0;
    while (Length < Count && Text[Length] != '\0') ++Length;
    if (Length == Count) return FieldResult::Invalid;
    Out.assign(Text, Text + Length);
    return FieldResult::Valid;
}

bool ParseNoData(const std::string& Text, double& OutValue)
{
    errno = 0;
    char* End = nullptr;
    const double Value = std::strtod(Text.c_str(), &End);
    if (errno != 0 || End == Text.c_str() || !std::isfinite(Value)) return false;
    while (*End && std::isspace(static_cast<unsigned char>(*End))) ++End;
    if (*End != '\0') return false;
    OutValue = Value;
    return true;
}

bool ValidateGeoKeys(TIFF* Image, FString& OutStatus, SkiPreparation::ProviderFailure& Failure)
{
    uint16* Keys = nullptr;
    uint32 Count = 0;
    const FieldResult Result = ReadArrayField(Image, GeoKeyDirectoryTag, TIFF_SHORT, Keys, Count, Failure);
    if (Result == FieldResult::Missing)
    {
        OutStatus = TEXT("inferred-provider-request-epsg:4326");
        return true;
    }
    if (Result != FieldResult::Valid || Count < 4 || Keys[3] > (Count - 4) / 4)
    {
        Fail(Failure, TEXT("TIFF_GEOREFERENCE_INVALID"), TEXT("GeoTIFF GeoKey directory is malformed."));
        Failure.GeoreferenceStatus = TEXT("invalid-geokey-directory");
        return false;
    }
    bool Explicit4326 = false;
    for (uint32 Index = 0; Index < Keys[3]; ++Index)
    {
        const uint16* Key = Keys + 4 + Index * 4;
        if (Key[1] != 0 || Key[2] != 1) continue;
        if (Key[0] == 1024 && Key[3] != 2)
        {
            Fail(Failure, TEXT("TIFF_CRS_UNSUPPORTED"), TEXT("GeoTIFF model type is not geographic EPSG:4326."));
            Failure.GeoreferenceStatus = TEXT("explicit-crs-conflict");
            return false;
        }
        if (Key[0] == 2048)
        {
            if (Key[3] != 4326)
            {
                Fail(Failure, TEXT("TIFF_CRS_UNSUPPORTED"), TEXT("GeoTIFF CRS conflicts with the required EPSG:4326 frame."));
                Failure.GeoreferenceStatus = TEXT("explicit-crs-conflict");
                return false;
            }
            Explicit4326 = true;
        }
        if (Key[0] == 3072)
        {
            Fail(Failure, TEXT("TIFF_CRS_UNSUPPORTED"), TEXT("Projected GeoTIFF responses are not accepted for this provider request."));
            Failure.GeoreferenceStatus = TEXT("explicit-projected-crs");
            return false;
        }
    }
    OutStatus = Explicit4326 ? TEXT("explicit-epsg:4326") : TEXT("inferred-provider-request-epsg:4326");
    return true;
}
}

bool SkiPreparation::DecodeElevationGeoTiff(const TArray<uint8>& Bytes,
    const SkiDomain::GeographicBounds& RequestedBounds, const ProviderProduct Product,
    DecodedElevationRaster& OutRaster, ProviderFailure& OutFailure)
{
    static_cast<void>(RequestedBounds);
    OutRaster = {};
    OutFailure = {};
    OutFailure.Product = Product;
    OutFailure.ResponseBytes = Bytes.Num();
    if (!HasTiffSignature(Bytes))
    {
        Fail(OutFailure, TEXT("TIFF_INVALID_SIGNATURE"),
            TEXT("Provider response is not a classic TIFF (HTML, JSON, and BigTIFF are rejected)."),
            RetryClassification::Retryable);
        return false;
    }
    if (Bytes.Num() > MaxElevationResponseBytes)
    {
        Fail(OutFailure, TEXT("TIFF_RESPONSE_TOO_LARGE"), TEXT("GeoTIFF response exceeds the 64 MiB elevation limit."),
            RetryClassification::ChangeSelection);
        return false;
    }

    EnsureGeoTagsRegistered();
    MemoryTiff Source{Bytes.GetData(), static_cast<uint64>(Bytes.Num()), 0};
    std::unique_ptr<TIFF, decltype(&TIFFClose)> Image(
        TIFFClientOpen("MountainPlannerElevation", "r", &Source, ReadMemory, RejectWrite, SeekMemory,
            CloseMemory, SizeMemory, RejectMap, UnmapMemory), &TIFFClose);
    if (!Image)
    {
        Fail(OutFailure, TEXT("TIFF_OPEN_FAILED"), TEXT("Provider response is not a readable TIFF."),
            RetryClassification::Retryable);
        return false;
    }

    uint32 Width = 0, Height = 0;
    uint16 Bits = 0, Samples = 0, Format = 0, Planar = 0, Orientation = 0, Compression = 0;
    if (!TIFFGetField(Image.get(), TIFFTAG_IMAGEWIDTH, &Width)
        || !TIFFGetField(Image.get(), TIFFTAG_IMAGELENGTH, &Height)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_BITSPERSAMPLE, &Bits)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLESPERPIXEL, &Samples)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLEFORMAT, &Format)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_PLANARCONFIG, &Planar)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_ORIENTATION, &Orientation)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_COMPRESSION, &Compression))
    {
        Fail(OutFailure, TEXT("TIFF_REQUIRED_TAG_MISSING"), TEXT("GeoTIFF omits a required image tag."));
        return false;
    }
    OutFailure.Width = Width;
    OutFailure.Height = Height;
    OutFailure.Orientation = Orientation;
    OutFailure.Compression = Compression;
    OutFailure.SampleFormat = Format;
    OutFailure.Organization = TIFFIsTiled(Image.get()) ? TEXT("tiled") : TEXT("stripped");
    const uint64 Count = static_cast<uint64>(Width) * static_cast<uint64>(Height);
    if (Width < 2 || Height < 2 || Count > SkiDomain::MaxHeightSamples
        || Count > static_cast<uint64>(MAX_int32) / sizeof(float))
    {
        Fail(OutFailure, TEXT("TIFF_DIMENSIONS_UNSUPPORTED"),
            TEXT("GeoTIFF dimensions overflow or exceed the four-million-sample limit."),
            RetryClassification::ChangeSelection);
        return false;
    }
    if (Bits != 32 || Samples != 1 || Format != SAMPLEFORMAT_IEEEFP || Planar != PLANARCONFIG_CONTIG)
    {
        Fail(OutFailure, TEXT("TIFF_ENCODING_UNSUPPORTED"),
            TEXT("GeoTIFF must contain one contiguous 32-bit IEEE floating-point channel."));
        return false;
    }
    if (Orientation != ORIENTATION_TOPLEFT && Orientation != ORIENTATION_BOTLEFT)
    {
        Fail(OutFailure, TEXT("TIFF_ORIENTATION_UNSUPPORTED"),
            TEXT("GeoTIFF orientation must be top-left or bottom-left."));
        return false;
    }

    std::vector<float> Decoded(static_cast<size_t>(Count));
    if (TIFFIsTiled(Image.get()))
    {
        uint32 TileWidth = 0, TileHeight = 0;
        if (!TIFFGetField(Image.get(), TIFFTAG_TILEWIDTH, &TileWidth)
            || !TIFFGetField(Image.get(), TIFFTAG_TILELENGTH, &TileHeight))
        {
            Fail(OutFailure, TEXT("TIFF_TILE_LAYOUT_INVALID"), TEXT("Tiled GeoTIFF omits its tile dimensions."));
            return false;
        }
        const tmsize_t TileBytes = TIFFTileSize(Image.get());
        const tmsize_t TileRowBytes = TIFFTileRowSize(Image.get());
        if (TileWidth == 0 || TileHeight == 0 || TileBytes <= 0 || TileBytes > MAX_int32 || TileRowBytes <= 0
            || static_cast<uint64>(TileRowBytes) < static_cast<uint64>(TileWidth) * sizeof(float))
        {
            Fail(OutFailure, TEXT("TIFF_TILE_LAYOUT_INVALID"), TEXT("Tiled GeoTIFF has invalid tile dimensions or stride."));
            return false;
        }
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(static_cast<int32>(TileBytes));
        for (uint32 TileY = 0; TileY < Height; TileY += TileHeight)
        {
            for (uint32 TileX = 0; TileX < Width; TileX += TileWidth)
            {
                const uint32 Tile = TIFFComputeTile(Image.get(), TileX, TileY, 0, 0);
                const tmsize_t Read = TIFFReadEncodedTile(Image.get(), Tile, Buffer.GetData(), TileBytes);
                const uint32 CopyRows = FMath::Min(TileHeight, Height - TileY);
                const uint32 CopyColumns = FMath::Min(TileWidth, Width - TileX);
                const uint64 Required = static_cast<uint64>(CopyRows - 1) * static_cast<uint64>(TileRowBytes)
                    + static_cast<uint64>(CopyColumns) * sizeof(float);
                if (Read < 0 || static_cast<uint64>(Read) < Required)
                {
                    Fail(OutFailure, TEXT("TIFF_TILE_DECODE_FAILED"),
                        TEXT("Tiled GeoTIFF decode returned a truncated or invalid tile."), RetryClassification::Retryable);
                    return false;
                }
                for (uint32 LocalRow = 0; LocalRow < CopyRows; ++LocalRow)
                {
                    const uint32 SourceRow = TileY + LocalRow;
                    const uint32 DestinationRow = Orientation == ORIENTATION_TOPLEFT ? SourceRow : Height - 1 - SourceRow;
                    FMemory::Memcpy(Decoded.data() + static_cast<size_t>(DestinationRow) * Width + TileX,
                        Buffer.GetData() + static_cast<int64>(LocalRow) * TileRowBytes,
                        static_cast<SIZE_T>(CopyColumns) * sizeof(float));
                }
            }
        }
    }
    else
    {
        uint32 RowsPerStrip = 0;
        if (!TIFFGetFieldDefaulted(Image.get(), TIFFTAG_ROWSPERSTRIP, &RowsPerStrip))
        {
            Fail(OutFailure, TEXT("TIFF_STRIP_LAYOUT_INVALID"), TEXT("Stripped GeoTIFF omits rows-per-strip."));
            return false;
        }
        const tmsize_t StripBytes = TIFFStripSize(Image.get());
        const tmsize_t RowBytes = TIFFScanlineSize(Image.get());
        const uint32 StripCount = TIFFNumberOfStrips(Image.get());
        if (RowsPerStrip == 0 || StripBytes <= 0 || StripBytes > MAX_int32
            || RowBytes < static_cast<tmsize_t>(static_cast<uint64>(Width) * sizeof(float)) || StripCount == 0)
        {
            Fail(OutFailure, TEXT("TIFF_STRIP_LAYOUT_INVALID"), TEXT("Stripped GeoTIFF has invalid strip dimensions or stride."));
            return false;
        }
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(static_cast<int32>(StripBytes));
        for (uint32 Strip = 0; Strip < StripCount; ++Strip)
        {
            const uint64 FirstRow64 = static_cast<uint64>(Strip) * RowsPerStrip;
            if (FirstRow64 >= Height) break;
            const uint32 FirstRow = static_cast<uint32>(FirstRow64);
            const uint32 CopyRows = FMath::Min(RowsPerStrip, Height - FirstRow);
            const tmsize_t Read = TIFFReadEncodedStrip(Image.get(), Strip, Buffer.GetData(), StripBytes);
            const uint64 Required = static_cast<uint64>(CopyRows - 1) * static_cast<uint64>(RowBytes)
                + static_cast<uint64>(Width) * sizeof(float);
            if (Read < 0 || static_cast<uint64>(Read) < Required)
            {
                Fail(OutFailure, TEXT("TIFF_STRIP_DECODE_FAILED"),
                    TEXT("Stripped GeoTIFF decode returned a truncated or invalid strip."), RetryClassification::Retryable);
                return false;
            }
            for (uint32 LocalRow = 0; LocalRow < CopyRows; ++LocalRow)
            {
                const uint32 SourceRow = FirstRow + LocalRow;
                const uint32 DestinationRow = Orientation == ORIENTATION_TOPLEFT ? SourceRow : Height - 1 - SourceRow;
                FMemory::Memcpy(Decoded.data() + static_cast<size_t>(DestinationRow) * Width,
                    Buffer.GetData() + static_cast<int64>(LocalRow) * RowBytes,
                    static_cast<SIZE_T>(Width) * sizeof(float));
            }
        }
    }

    double NoData = -9999.0;
    std::string NoDataText;
    const FieldResult NoDataResult = ReadAsciiField(Image.get(), TIFFTAG_GDAL_NODATA, NoDataText, OutFailure);
    if (NoDataResult == FieldResult::Invalid || (NoDataResult == FieldResult::Valid && !ParseNoData(NoDataText, NoData)))
    {
        Fail(OutFailure, TEXT("TIFF_METADATA_INVALID"), TEXT("GeoTIFF GDAL nodata metadata is malformed."));
        return false;
    }
    if (NoDataResult == FieldResult::Valid) OutFailure.NoData = UTF8_TO_TCHAR(NoDataText.c_str());

    double* Scale = nullptr;
    double* Tie = nullptr;
    uint32 ScaleCount = 0, TieCount = 0;
    const FieldResult ScaleResult = ReadArrayField(Image.get(), ModelPixelScaleTag, TIFF_DOUBLE, Scale, ScaleCount, OutFailure);
    const FieldResult TieResult = ReadArrayField(Image.get(), ModelTiepointTag, TIFF_DOUBLE, Tie, TieCount, OutFailure);
    if (ScaleResult != FieldResult::Valid || TieResult != FieldResult::Valid || ScaleCount < 2 || TieCount < 6
        || !FMath::IsFinite(Scale[0]) || !FMath::IsFinite(Scale[1]) || Scale[0] <= 0.0 || Scale[1] <= 0.0
        || !FMath::IsFinite(Tie[0]) || !FMath::IsFinite(Tie[1]) || !FMath::IsFinite(Tie[3]) || !FMath::IsFinite(Tie[4]))
    {
        Fail(OutFailure, ScaleResult == FieldResult::Missing || TieResult == FieldResult::Missing
                ? TEXT("TIFF_GEOREFERENCE_MISSING") : TEXT("TIFF_GEOREFERENCE_INVALID"),
            TEXT("GeoTIFF requires a valid pixel-scale and tiepoint pair."));
        OutFailure.GeoreferenceStatus = TEXT("missing-or-invalid-scale-tiepoint");
        return false;
    }
    if (!ValidateGeoKeys(Image.get(), OutFailure.GeoreferenceStatus, OutFailure)) return false;

    const double West = Tie[3] - Tie[0] * Scale[0];
    const double North = Tie[4] + Tie[1] * Scale[1];
    const SkiDomain::GeographicBounds Outer{
        West, North - static_cast<double>(Height) * Scale[1],
        West + static_cast<double>(Width) * Scale[0], North};
    if (!FMath::IsFinite(Outer.WestDeg) || !FMath::IsFinite(Outer.SouthDeg)
        || !FMath::IsFinite(Outer.EastDeg) || !FMath::IsFinite(Outer.NorthDeg)
        || Outer.WestDeg >= Outer.EastDeg || Outer.SouthDeg >= Outer.NorthDeg)
    {
        Fail(OutFailure, TEXT("TIFF_GEOREFERENCE_INVALID"), TEXT("GeoTIFF georeferenced bounds are invalid."));
        return false;
    }
    const double CenterLat = (Outer.SouthDeg + Outer.NorthDeg) * 0.5;
    const double CenterLon = (Outer.WestDeg + Outer.EastDeg) * 0.5;
    SkiDomain::LocalFrame Frame;
    if (!SkiDomain::TryMakeLocalFrame({CenterLat, CenterLon, 0.0}, Frame))
    {
        Fail(OutFailure, TEXT("TIFF_GEOREFERENCE_INVALID"), TEXT("GeoTIFF local coordinate frame is invalid."));
        return false;
    }
    const SkiDomain::EnuPoint WestSample = SkiDomain::ToEnu(Frame, {CenterLat, CenterLon - Scale[0] * 0.5, 0.0});
    const SkiDomain::EnuPoint EastSample = SkiDomain::ToEnu(Frame, {CenterLat, CenterLon + Scale[0] * 0.5, 0.0});
    const SkiDomain::EnuPoint SouthSample = SkiDomain::ToEnu(Frame, {CenterLat - Scale[1] * 0.5, CenterLon, 0.0});
    const SkiDomain::EnuPoint NorthSample = SkiDomain::ToEnu(Frame, {CenterLat + Scale[1] * 0.5, CenterLon, 0.0});
    const double PixelEastM = EastSample.EastM - WestSample.EastM;
    const double PixelNorthM = NorthSample.NorthM - SouthSample.NorthM;
    if (!FMath::IsFinite(PixelEastM) || !FMath::IsFinite(PixelNorthM) || PixelEastM <= 0.0 || PixelNorthM <= 0.0)
    {
        Fail(OutFailure, TEXT("TIFF_GEOREFERENCE_INVALID"), TEXT("GeoTIFF sample spacing is invalid."));
        return false;
    }

    OutRaster.SourceWidth = Width;
    OutRaster.SourceHeight = Height;
    OutRaster.Orientation = Orientation;
    OutRaster.Storage = OutFailure.Organization == TEXT("tiled") ? TiffStorageOrganization::Tiled : TiffStorageOrganization::Stripped;
    OutRaster.Compression = Compression;
    OutRaster.NoDataValue = NoData;
    OutRaster.ActualOuterBounds = Outer;
    OutRaster.SampleCenterBounds = {
        Outer.WestDeg + Scale[0] * 0.5, Outer.SouthDeg + Scale[1] * 0.5,
        Outer.EastDeg - Scale[0] * 0.5, Outer.NorthDeg - Scale[1] * 0.5};
    OutRaster.Heightfield.Width = Width;
    OutRaster.Heightfield.Height = Height;
    OutRaster.Heightfield.WestM = -static_cast<double>(Width - 1) * PixelEastM * 0.5;
    OutRaster.Heightfield.NorthM = static_cast<double>(Height - 1) * PixelNorthM * 0.5;
    OutRaster.Heightfield.EastSpacingM = PixelEastM;
    OutRaster.Heightfield.NorthSpacingM = PixelNorthM;
    OutRaster.Heightfield.NoDataValue = NoData;
    OutRaster.Heightfield.CurrentRevision = 1;
    OutRaster.Heightfield.Samples = std::move(Decoded);
    if (!SkiDomain::IsValidHeightfield(OutRaster.Heightfield))
    {
        Fail(OutFailure, TEXT("TIFF_HEIGHTFIELD_INVALID"), TEXT("Decoded GeoTIFF does not form a valid finite heightfield."));
        OutRaster = {};
        return false;
    }
    return true;
}
