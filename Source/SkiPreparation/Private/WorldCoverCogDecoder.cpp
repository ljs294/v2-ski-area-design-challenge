#include "SkiPreparation/WorldCoverCogDecoder.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "tiffio.h"

#include <cmath>
#include <memory>

namespace
{
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint64 MaxCoverCells = 16ULL * 1024ULL * 1024ULL;
constexpr uint64 MaxCogBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64 MaxDecodedTileBytes = 64ULL * 1024ULL * 1024ULL;

struct CogHandle
{
    SkiPreparation::ICogByteSource* Source = nullptr;
    uint64 Position = 0;
    FString Error;
};

tmsize_t ReadCog(thandle_t Handle, void* Destination, const tmsize_t Requested)
{
    CogHandle& Reader = *static_cast<CogHandle*>(Handle);
    if (!Reader.Source || !Destination || Requested <= 0) return 0;
    const uint64 Size = Reader.Source->Size();
    if (Reader.Position >= Size) return 0;
    const uint64 Length = FMath::Min<uint64>(static_cast<uint64>(Requested), Size - Reader.Position);
    TArray<uint8> Bytes;
    if (!Reader.Source->Read(Reader.Position, Length, Bytes, Reader.Error)
        || static_cast<uint64>(Bytes.Num()) != Length) return 0;
    FMemory::Memcpy(Destination, Bytes.GetData(), Bytes.Num());
    Reader.Position += Length;
    return static_cast<tmsize_t>(Length);
}

tmsize_t RejectWrite(thandle_t, void*, tmsize_t) { return 0; }
toff_t SeekCog(thandle_t Handle, const toff_t Offset, const int Origin)
{
    CogHandle& Reader = *static_cast<CogHandle*>(Handle);
    if (!Reader.Source) return static_cast<toff_t>(-1);
    const uint64 Size = Reader.Source->Size();
    uint64 Base = 0;
    if (Origin == SEEK_CUR) Base = Reader.Position;
    else if (Origin == SEEK_END) Base = Size;
    else if (Origin != SEEK_SET) return static_cast<toff_t>(-1);
    if (static_cast<uint64>(Offset) > MAX_uint64 - Base) return static_cast<toff_t>(-1);
    const uint64 Target = Base + static_cast<uint64>(Offset);
    if (Target > Size) return static_cast<toff_t>(-1);
    Reader.Position = Target;
    return static_cast<toff_t>(Target);
}
int CloseCog(thandle_t) { return 0; }
toff_t SizeCog(thandle_t Handle)
{
    const CogHandle& Reader = *static_cast<CogHandle*>(Handle);
    return Reader.Source ? static_cast<toff_t>(Reader.Source->Size()) : 0;
}
int RejectMap(thandle_t, void**, toff_t*) { return 0; }
void RejectUnmap(thandle_t, void*, toff_t) {}

void Fail(SkiPreparation::ProviderFailure& Failure, const TCHAR* Code, const TCHAR* Summary)
{
    Failure.Code = Code;
    Failure.Stage = SkiPreparation::FailureStage::Decoding;
    Failure.Product = SkiPreparation::ProviderProduct::WorldCover;
    Failure.Retry = SkiPreparation::RetryClassification::NotRetryable;
    Failure.Summary = Summary;
}

bool IsWorldCoverClass(const uint8 Value)
{
    switch (Value)
    {
    case 10: case 20: case 30: case 40: case 50: case 60:
    case 70: case 80: case 90: case 95: case 100: return true;
    default: return false;
    }
}

bool HasEpsg4326(TIFF* Image)
{
    uint32 Count = 0;
    uint16* Keys = nullptr;
    if (!TIFFGetField(Image, GeoKeyDirectoryTag, &Count, &Keys) || !Keys || Count < 4
        || Keys[3] > (Count - 4) / 4) return false;
    bool Geographic = false;
    bool Epsg4326 = false;
    for (uint32 Index = 0; Index < Keys[3]; ++Index)
    {
        const uint16* Key = Keys + 4 + Index * 4;
        if (Key[1] != 0 || Key[2] != 1) continue;
        if (Key[0] == 1024) Geographic = Key[3] == 2;
        if (Key[0] == 2048) Epsg4326 = Key[3] == 4326;
        if (Key[0] == 3072) return false;
    }
    return Geographic && Epsg4326;
}
}

bool SkiPreparation::DecodeWorldCoverCogWindow(ICogByteSource& Source,
    const SkiDomain::GeographicBounds& RequestedBounds, DecodedCoverWindow& OutWindow,
    ProviderFailure& OutFailure, const TFunction<bool()>& IsCancelled)
{
    OutWindow = {};
    OutFailure = {};
    OutFailure.Product = ProviderProduct::WorldCover;
    if (Source.Size() < 16 || Source.Size() > MaxCogBytes)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_SIZE_INVALID"),
            TEXT("WorldCover COG size is outside the bounded reader envelope."));
        return false;
    }
    if (!std::isfinite(RequestedBounds.WestDeg) || !std::isfinite(RequestedBounds.EastDeg)
        || !std::isfinite(RequestedBounds.SouthDeg) || !std::isfinite(RequestedBounds.NorthDeg)
        || RequestedBounds.WestDeg >= RequestedBounds.EastDeg
        || RequestedBounds.SouthDeg >= RequestedBounds.NorthDeg)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_WINDOW_INVALID"), TEXT("WorldCover window bounds are invalid."));
        return false;
    }
    EnsureGeoTiffTagsRegistered();
    CogHandle Reader{&Source};
    std::unique_ptr<TIFF, decltype(&TIFFClose)> Image(TIFFClientOpen("MountainPlannerWorldCover", "r",
        &Reader, ReadCog, RejectWrite, SeekCog, CloseCog, SizeCog, RejectMap, RejectUnmap), &TIFFClose);
    if (!Image)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_OPEN_FAILED"),
            Reader.Error.IsEmpty() ? TEXT("WorldCover COG could not be opened.") : *Reader.Error);
        OutFailure.Retry = RetryClassification::Retryable;
        return false;
    }
    uint32 Width = 0, Height = 0, TileWidth = 0, TileHeight = 0;
    uint16 Bits = 0, Samples = 0, Format = 0, Planar = 0, Orientation = 0, Compression = 0;
    if (!TIFFGetField(Image.get(), TIFFTAG_IMAGEWIDTH, &Width)
        || !TIFFGetField(Image.get(), TIFFTAG_IMAGELENGTH, &Height)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_BITSPERSAMPLE, &Bits)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLESPERPIXEL, &Samples)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLEFORMAT, &Format)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_PLANARCONFIG, &Planar)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_ORIENTATION, &Orientation)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_COMPRESSION, &Compression)
        || !TIFFIsTiled(Image.get())
        || !TIFFGetField(Image.get(), TIFFTAG_TILEWIDTH, &TileWidth)
        || !TIFFGetField(Image.get(), TIFFTAG_TILELENGTH, &TileHeight))
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_TAG_INVALID"),
            TEXT("WorldCover COG omits required tiled-image metadata."));
        return false;
    }
    OutFailure.Width = Width; OutFailure.Height = Height; OutFailure.Compression = Compression;
    OutFailure.Orientation = Orientation; OutFailure.SampleFormat = Format;
    OutFailure.Organization = TEXT("tiled-cog");
    if (Width < 2 || Height < 2 || TileWidth == 0 || TileHeight == 0
        || Bits != 8 || Samples != 1 || (Format != SAMPLEFORMAT_UINT && Format != SAMPLEFORMAT_VOID)
        || Planar != PLANARCONFIG_CONTIG || Orientation != ORIENTATION_TOPLEFT)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_ENCODING_UNSUPPORTED"),
            TEXT("WorldCover must be a north-up tiled one-band uint8 class COG."));
        return false;
    }
    const uint64 DecodedTileBytes = static_cast<uint64>(TileWidth) * TileHeight;
    if (DecodedTileBytes == 0 || DecodedTileBytes > MaxDecodedTileBytes)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_TILE_INVALID"), TEXT("WorldCover tile dimensions are unsafe."));
        return false;
    }
    uint32 ScaleCount = 0, TieCount = 0;
    double* Scale = nullptr;
    double* Tie = nullptr;
    if (!TIFFGetField(Image.get(), ModelPixelScaleTag, &ScaleCount, &Scale) || ScaleCount < 2 || !Scale
        || !TIFFGetField(Image.get(), ModelTiepointTag, &TieCount, &Tie) || TieCount < 6 || !Tie
        || !HasEpsg4326(Image.get()) || !std::isfinite(Scale[0]) || !std::isfinite(Scale[1])
        || Scale[0] <= 0.0 || Scale[1] <= 0.0
        || !std::isfinite(Tie[0]) || !std::isfinite(Tie[1])
        || !std::isfinite(Tie[3]) || !std::isfinite(Tie[4]))
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_GEOREFERENCE_INVALID"),
            TEXT("WorldCover COG georeferencing is missing, malformed, or not EPSG:4326."));
        OutFailure.GeoreferenceStatus = TEXT("invalid-or-non-epsg4326");
        return false;
    }
    const double OuterWest = Tie[3] - Tie[0] * Scale[0];
    const double OuterNorth = Tie[4] + Tie[1] * Scale[1];
    const double OuterEast = OuterWest + Width * Scale[0];
    const double OuterSouth = OuterNorth - Height * Scale[1];
    const double CenterWest = OuterWest + Scale[0] * 0.5;
    const double CenterNorth = OuterNorth - Scale[1] * 0.5;
    const int64 MinColumn = FMath::Clamp<int64>(FMath::CeilToInt64(
        (RequestedBounds.WestDeg - CenterWest) / Scale[0] - 1.0e-9), 0, Width - 1);
    const int64 MaxColumn = FMath::Clamp<int64>(FMath::FloorToInt64(
        (RequestedBounds.EastDeg - CenterWest) / Scale[0] + 1.0e-9), 0, Width - 1);
    const int64 MinRow = FMath::Clamp<int64>(FMath::CeilToInt64(
        (CenterNorth - RequestedBounds.NorthDeg) / Scale[1] - 1.0e-9), 0, Height - 1);
    const int64 MaxRow = FMath::Clamp<int64>(FMath::FloorToInt64(
        (CenterNorth - RequestedBounds.SouthDeg) / Scale[1] + 1.0e-9), 0, Height - 1);
    if (MaxColumn < MinColumn || MaxRow < MinRow)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_NO_INTERSECTION"),
            TEXT("WorldCover COG does not intersect the selected terrain."));
        return false;
    }
    const uint64 WindowWidth = static_cast<uint64>(MaxColumn - MinColumn + 1);
    const uint64 WindowHeight = static_cast<uint64>(MaxRow - MinRow + 1);
    if (WindowWidth > MAX_uint32 || WindowHeight > MAX_uint32
        || WindowWidth * WindowHeight > MaxCoverCells)
    {
        Fail(OutFailure, TEXT("WORLDCOVER_COG_WINDOW_TOO_LARGE"),
            TEXT("WorldCover class window exceeds the sixteen-million-cell limit."));
        return false;
    }
    OutWindow.Width = static_cast<uint32>(WindowWidth);
    OutWindow.Height = static_cast<uint32>(WindowHeight);
    OutWindow.Classes.Init(0, static_cast<int32>(WindowWidth * WindowHeight));
    OutWindow.Validity.Init(0, static_cast<int32>(WindowWidth * WindowHeight));
    OutWindow.LongitudeSpacingDeg = Scale[0];
    OutWindow.LatitudeSpacingDeg = Scale[1];
    OutWindow.Compression = Compression;
    OutWindow.SourceWidth = Width;
    OutWindow.SourceHeight = Height;
    OutWindow.SampleCenterBounds = {CenterWest + MinColumn * Scale[0],
        CenterNorth - MaxRow * Scale[1], CenterWest + MaxColumn * Scale[0],
        CenterNorth - MinRow * Scale[1]};
    OutWindow.ActualOuterBounds = {OutWindow.SampleCenterBounds.WestDeg - Scale[0] * 0.5,
        OutWindow.SampleCenterBounds.SouthDeg - Scale[1] * 0.5,
        OutWindow.SampleCenterBounds.EastDeg + Scale[0] * 0.5,
        OutWindow.SampleCenterBounds.NorthDeg + Scale[1] * 0.5};

    TArray<uint8> TileBuffer;
    TileBuffer.SetNumUninitialized(static_cast<int32>(DecodedTileBytes));
    const uint32 FirstTileX = static_cast<uint32>(MinColumn) / TileWidth;
    const uint32 LastTileX = static_cast<uint32>(MaxColumn) / TileWidth;
    const uint32 FirstTileY = static_cast<uint32>(MinRow) / TileHeight;
    const uint32 LastTileY = static_cast<uint32>(MaxRow) / TileHeight;
    for (uint32 TileY = FirstTileY; TileY <= LastTileY; ++TileY)
    {
        for (uint32 TileX = FirstTileX; TileX <= LastTileX; ++TileX)
        {
            if (IsCancelled && IsCancelled())
            {
                Fail(OutFailure, TEXT("PREPARATION_CANCELLED"), TEXT("WorldCover COG decode was cancelled."));
                OutFailure.Retry = RetryClassification::Retryable;
                OutWindow = {};
                return false;
            }
            const ttile_t Tile = TIFFComputeTile(Image.get(), TileX * TileWidth, TileY * TileHeight, 0, 0);
            const tmsize_t Decoded = TIFFReadEncodedTile(Image.get(), Tile, TileBuffer.GetData(),
                static_cast<tmsize_t>(DecodedTileBytes));
            if (Decoded < 0 || static_cast<uint64>(Decoded) != DecodedTileBytes)
            {
                Fail(OutFailure, TEXT("WORLDCOVER_COG_TILE_DECODE_FAILED"),
                    TEXT("WorldCover COG tile is truncated or could not be decoded."));
                OutFailure.Retry = RetryClassification::Retryable;
                OutWindow = {};
                return false;
            }
            const uint32 SourceColumn0 = TileX * TileWidth;
            const uint32 SourceRow0 = TileY * TileHeight;
            const uint32 CopyColumn0 = FMath::Max<uint32>(SourceColumn0, static_cast<uint32>(MinColumn));
            const uint32 CopyColumn1 = FMath::Min<uint32>(SourceColumn0 + TileWidth - 1, static_cast<uint32>(MaxColumn));
            const uint32 CopyRow0 = FMath::Max<uint32>(SourceRow0, static_cast<uint32>(MinRow));
            const uint32 CopyRow1 = FMath::Min<uint32>(SourceRow0 + TileHeight - 1, static_cast<uint32>(MaxRow));
            for (uint32 Row = CopyRow0; Row <= CopyRow1; ++Row)
            {
                for (uint32 Column = CopyColumn0; Column <= CopyColumn1; ++Column)
                {
                    const uint8 Value = TileBuffer[(Row - SourceRow0) * TileWidth + Column - SourceColumn0];
                    const uint32 OutputIndex = (Row - static_cast<uint32>(MinRow)) * OutWindow.Width
                        + Column - static_cast<uint32>(MinColumn);
                    if (IsWorldCoverClass(Value))
                    {
                        OutWindow.Classes[OutputIndex] = Value;
                        OutWindow.Validity[OutputIndex] = 1;
                    }
                }
            }
        }
    }
    OutFailure.GeoreferenceStatus = TEXT("explicit-epsg:4326");
    return true;
}
