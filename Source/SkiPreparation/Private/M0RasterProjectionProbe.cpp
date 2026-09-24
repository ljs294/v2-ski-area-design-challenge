#include "SkiPreparation/M0RasterProjectionProbe.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "tiffio.h"
#include "proj.h"

#include <cmath>
#include <memory>

namespace
{
class FLocalCogSource final : public SkiPreparation::ICogByteSource
{
public:
    explicit FLocalCogSource(const FString& Path) { Reader.Reset(IFileManager::Get().CreateFileReader(*Path)); }
    uint64 Size() const noexcept override { return Reader ? Reader->TotalSize() : 0; }
    bool Read(uint64 Offset, uint64 Length, TArray<uint8>& Out, FString& Error) override
    {
        if (!Reader || Offset > Size() || Length > Size() - Offset || Length > MAX_int32)
        { Error = TEXT("COG_LOCAL_RANGE_INVALID"); return false; }
        Reader->Seek(Offset);
        Out.SetNumUninitialized(static_cast<int32>(Length));
        Reader->Serialize(Out.GetData(), Length);
        if (Reader->IsError()) { Error = TEXT("COG_LOCAL_READ_FAILED"); return false; }
        return true;
    }
private:
    TUniquePtr<FArchive> Reader;
};
struct FReader
{
    SkiPreparation::ICogByteSource* Source = nullptr;
    uint64 Position = 0;
    FString Error;
};

tmsize_t Read(thandle_t Handle, void* Destination, tmsize_t Requested)
{
    FReader& R = *static_cast<FReader*>(Handle);
    if (!R.Source || !Destination || Requested <= 0 || R.Position >= R.Source->Size()) return 0;
    const uint64 Count = FMath::Min<uint64>(Requested, R.Source->Size() - R.Position);
    TArray<uint8> Bytes;
    if (!R.Source->Read(R.Position, Count, Bytes, R.Error) || static_cast<uint64>(Bytes.Num()) != Count) return 0;
    FMemory::Memcpy(Destination, Bytes.GetData(), Bytes.Num());
    R.Position += Count;
    return static_cast<tmsize_t>(Count);
}

}

namespace
{
bool IsSafeStrongETag(const FString& Tag)
{
    if (Tag.Len() < 2 || Tag.Len() > 128 || Tag[0] != '"' || Tag[Tag.Len() - 1] != '"') return false;
    for (int32 I = 1; I < Tag.Len() - 1; ++I)
    {
        const TCHAR C = Tag[I];
        if (C < 0x21 || C > 0x7e || C == '"' || C == ':' || C == '\\') return false;
    }
    return true;
}
}

SkiPreparation::FM0GatewayCogByteSource::FM0GatewayCogByteSource(FString InUrl,
    uint64 InMaxTransferredBytes, uint64 InMaxRequests, IAcquisitionTransport* InTransport)
    : Url(MoveTemp(InUrl)), Transport(InTransport ? InTransport : &Gateway),
      MaxTransferredBytes(InMaxTransferredBytes), MaxRequests(InMaxRequests) {}

bool SkiPreparation::FM0GatewayCogByteSource::Initialize(FString& OutError)
{
    ObjectSize = 0;
    PinnedETag.Reset();
    if (Requests >= MaxRequests || TransferredBytesCount > MaxTransferredBytes - FMath::Min<uint64>(16, MaxTransferredBytes))
    { OutError = TEXT("GATEWAY_COG_BUDGET_EXCEEDED"); return false; }
    // The first exact range supplies Content-Range's authoritative total.
    HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.ByteRange = HttpByteRange{0, 16};
    Request.MaximumResponseBytes = 16;
    Request.TotalTimeoutSeconds = 15.0F;
    ++Requests;
    const HttpAcquisitionResult Result = Transport->Get(Request, Cancel);
    uint64 Total = 0;
    if (!Result.Ok() || Result.HttpStatus != 206 ||
        !ValidateContentRange(Result.ContentRange, Request.ByteRange.GetValue(), Result.Bytes.Num(), Total)
        || Total < 16 || Total > 1024ULL * 1024 * 1024 || !IsSafeStrongETag(Result.ETag))
    {
        OutError = Result.Ok() && Result.HttpStatus == 206 && !IsSafeStrongETag(Result.ETag)
            ? TEXT("GATEWAY_COG_STRONG_ETAG_REQUIRED")
            : TEXT("GATEWAY_COG_INITIAL_RANGE_FAILED: ") + Result.RequestStatus;
        return false;
    }
    ObjectSize = Total;
    PinnedETag = Result.ETag;
    TransferredBytesCount += Result.Bytes.Num();
    return true;
}

bool SkiPreparation::FM0GatewayCogByteSource::Read(uint64 Offset, uint64 Length,
    TArray<uint8>& OutBytes, FString& OutError)
{
    OutBytes.Reset();
    if (ObjectSize == 0 || Offset >= ObjectSize || Length == 0 ||
        Length > ObjectSize - Offset || Length > 16ULL * 1024 * 1024 ||
        Requests >= MaxRequests || Length > MaxTransferredBytes - FMath::Min(TransferredBytesCount, MaxTransferredBytes))
    {
        OutError = TEXT("GATEWAY_COG_RANGE_INVALID"); return false;
    }
    HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.ByteRange = HttpByteRange{Offset, Length};
    Request.IfMatchETag = PinnedETag;
    Request.MaximumResponseBytes = Length;
    Request.TotalTimeoutSeconds = 30.0F;
    ++Requests;
    const HttpAcquisitionResult Result = Transport->Get(Request, Cancel);
    if (Result.HttpStatus == 412 || (Result.HttpStatus == 206 && Result.ETag != PinnedETag))
    {
        ObjectSize = 0;
        PinnedETag.Reset();
        OutError = Result.HttpStatus == 412 ? TEXT("GATEWAY_COG_ETAG_PRECONDITION_FAILED")
            : TEXT("GATEWAY_COG_ETAG_CHANGED");
        return false;
    }
    uint64 Total = 0;
    if (!Result.Ok() || Result.HttpStatus != 206 ||
        !ValidateContentRange(Result.ContentRange, Request.ByteRange.GetValue(), Result.Bytes.Num(), Total)
        || Total != ObjectSize)
    {
        OutError = TEXT("GATEWAY_COG_RANGE_FAILED: ") + Result.RequestStatus;
        return false;
    }
    OutBytes = Result.Bytes;
    TransferredBytesCount += Result.Bytes.Num();
    return true;
}

namespace
{
tmsize_t Write(thandle_t, void*, tmsize_t) { return 0; }
toff_t Seek(thandle_t Handle, toff_t Offset, int Origin)
{
    FReader& R = *static_cast<FReader*>(Handle);
    const uint64 Base = Origin == SEEK_SET ? 0 : Origin == SEEK_CUR ? R.Position : Origin == SEEK_END ? R.Source->Size() : MAX_uint64;
    if (Base > R.Source->Size() || Offset > R.Source->Size() - Base) return static_cast<toff_t>(-1);
    R.Position = Base + Offset;
    return R.Position;
}
int Close(thandle_t) { return 0; }
toff_t Size(thandle_t Handle) { return static_cast<FReader*>(Handle)->Source->Size(); }
int Map(thandle_t, void**, toff_t*) { return 0; }
void Unmap(thandle_t, void*, toff_t) {}

bool ProbeCog(SkiPreparation::ICogByteSource& Source, SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
    if (Source.Size() < 16 || Source.Size() > 1024ULL * 1024 * 1024)
    {
        Receipt.CogError = TEXT("COG_SIZE_INVALID"); return false;
    }
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    FReader R{&Source};
    std::unique_ptr<TIFF, decltype(&TIFFClose)> Image(TIFFClientOpen("M0Elevation", "r", &R,
        Read, Write, Seek, Close, Size, Map, Unmap), &TIFFClose);
    if (!Image) { Receipt.CogError = TEXT("COG_OPEN_FAILED: ") + R.Error; return false; }
    uint32 Width = 0, Height = 0, TileWidth = 0, TileHeight = 0;
    uint16 Bits = 0, Samples = 0, Format = 0, Compression = 0, Predictor = 0, Orientation = 0;
    if (!TIFFGetField(Image.get(), TIFFTAG_IMAGEWIDTH, &Width)
        || !TIFFGetField(Image.get(), TIFFTAG_IMAGELENGTH, &Height)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_BITSPERSAMPLE, &Bits)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLESPERPIXEL, &Samples)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLEFORMAT, &Format)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_COMPRESSION, &Compression)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_PREDICTOR, &Predictor)
        || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_ORIENTATION, &Orientation)
        || !TIFFIsTiled(Image.get())
        || !TIFFGetField(Image.get(), TIFFTAG_TILEWIDTH, &TileWidth)
        || !TIFFGetField(Image.get(), TIFFTAG_TILELENGTH, &TileHeight)
        || Width < 2 || Height < 2 || TileWidth == 0 || TileHeight == 0
        || Bits != 32 || Samples != 1 || Format != SAMPLEFORMAT_IEEEFP
        || Compression != COMPRESSION_LZW || Predictor != 3 || Orientation != ORIENTATION_TOPLEFT)
    {
        Receipt.CogError = TEXT("COG_ENCODING_UNSUPPORTED"); return false;
    }
    Receipt.Width = Width; Receipt.Height = Height;
    uint32 NoDataCount = 0; char* NoDataText = nullptr;
    if (!TIFFGetField(Image.get(), TIFFTAG_GDAL_NODATA, &NoDataCount, &NoDataText) || !NoDataText)
    {
        Receipt.CogError = TEXT("COG_NODATA_MISSING"); return false;
    }
    const double NoData = FCString::Atod(UTF8_TO_TCHAR(NoDataText));
    if (!std::isfinite(NoData)) { Receipt.CogError = TEXT("COG_NODATA_INVALID"); return false; }
    const tmsize_t TileBytes = TIFFTileSize(Image.get());
    if (TileBytes <= 0 || TileBytes > 64 * 1024 * 1024 || static_cast<uint64>(TileBytes) < static_cast<uint64>(TileWidth) * TileHeight * sizeof(float))
    {
        Receipt.CogError = TEXT("COG_TILE_SIZE_INVALID"); return false;
    }
    TArray<uint8> Buffer; Buffer.SetNumUninitialized(static_cast<int32>(TileBytes));
    for (uint32 Y = 0; Y < Height; Y += TileHeight)
    for (uint32 X = 0; X < Width; X += TileWidth)
    {
        const ttile_t Tile = TIFFComputeTile(Image.get(), X, Y, 0, 0);
        if (TIFFReadEncodedTile(Image.get(), Tile, Buffer.GetData(), TileBytes) != TileBytes)
        {
            Receipt.CogError = TEXT("COG_TILE_DECODE_FAILED: ") + R.Error; return false;
        }
        const float* Values = reinterpret_cast<const float*>(Buffer.GetData());
        for (uint32 LocalY = 0; LocalY < FMath::Min(TileHeight, Height - Y); ++LocalY)
        for (uint32 LocalX = 0; LocalX < FMath::Min(TileWidth, Width - X); ++LocalX)
        {
            const float Value = Values[static_cast<uint64>(LocalY) * TileWidth + LocalX];
            if (!std::isfinite(Value) || Value == NoData) ++Receipt.NoDataSamples;
            else ++Receipt.ValidSamples;
        }
    }
    while (TIFFReadDirectory(Image.get())) ++Receipt.OverviewCount;
    return Receipt.ValidSamples + Receipt.NoDataSamples == static_cast<uint64>(Width) * Height;
}

bool ProbeRealS1M(SkiPreparation::ICogByteSource& Source, SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
    Receipt.bRealS1MProbe = true;
    if (Source.Size() < 16 || Source.Size() > 1024ULL * 1024 * 1024)
    { Receipt.CogError = TEXT("S1M_OBJECT_SIZE_INVALID"); return false; }
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    FReader R{&Source};
    std::unique_ptr<TIFF, decltype(&TIFFClose)> Image(TIFFClientOpen("M0RealS1M", "r", &R,
        Read, Write, Seek, Close, Size, Map, Unmap), &TIFFClose);
    if (!Image) { Receipt.CogError = TEXT("S1M_OPEN_FAILED: ") + R.Error; return false; }
    const auto DecodeOneTile = [&](bool bOverview) -> bool
    {
        uint32 Width = 0, Height = 0, TileWidth = 0, TileHeight = 0;
        uint16 Bits = 0, Samples = 0, Format = 0, Compression = 0, Predictor = 0;
        if (!TIFFGetField(Image.get(), TIFFTAG_IMAGEWIDTH, &Width)
            || !TIFFGetField(Image.get(), TIFFTAG_IMAGELENGTH, &Height)
            || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_BITSPERSAMPLE, &Bits)
            || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLESPERPIXEL, &Samples)
            || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_SAMPLEFORMAT, &Format)
            || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_COMPRESSION, &Compression)
            || !TIFFGetFieldDefaulted(Image.get(), TIFFTAG_PREDICTOR, &Predictor)
            || !TIFFIsTiled(Image.get())
            || !TIFFGetField(Image.get(), TIFFTAG_TILEWIDTH, &TileWidth)
            || !TIFFGetField(Image.get(), TIFFTAG_TILELENGTH, &TileHeight)
            || Width < 2 || Height < 2 || TileWidth == 0 || TileHeight == 0
            || Bits != 32 || Samples != 1 || Format != SAMPLEFORMAT_IEEEFP
            || Compression != COMPRESSION_LZW || Predictor != 3)
        { Receipt.CogError = TEXT("S1M_IFD_ENCODING_INVALID"); return false; }
        if (!bOverview)
        {
            Receipt.Width = Width; Receipt.Height = Height;
            uint32 KeyCount = 0; uint16* Keys = nullptr;
            bool Has6350 = false;
            if (TIFFGetField(Image.get(), 34735, &KeyCount, &Keys) && Keys && KeyCount >= 4
                && Keys[3] <= (KeyCount - 4) / 4)
            {
                for (uint32 K = 0; K < Keys[3]; ++K)
                {
                    const uint16* Key = Keys + 4 + K * 4;
                    if (Key[0] == 3072 && Key[1] == 0 && Key[2] == 1 && Key[3] == 6350)
                        Has6350 = true;
                }
            }
            if (!Has6350) { Receipt.CogError = TEXT("S1M_EPSG_6350_MISSING"); return false; }
            uint32 NoDataCount = 0; char* NoDataText = nullptr;
            if (!TIFFGetField(Image.get(), TIFFTAG_GDAL_NODATA, &NoDataCount, &NoDataText)
                || !NoDataText || FCString::Atod(UTF8_TO_TCHAR(NoDataText)) != -999999.0)
            { Receipt.CogError = TEXT("S1M_NODATA_INVALID"); return false; }
        }
        else if (Width >= Receipt.Width || Height >= Receipt.Height)
        { Receipt.CogError = TEXT("S1M_OVERVIEW_NOT_REDUCED"); return false; }
        const tmsize_t TileBytes = TIFFTileSize(Image.get());
        if (TileBytes <= 0 || TileBytes > 4 * 1024 * 1024)
        { Receipt.CogError = TEXT("S1M_TILE_SIZE_INVALID"); return false; }
        TArray<uint8> Buffer; Buffer.SetNumUninitialized(static_cast<int32>(TileBytes));
        if (TIFFReadEncodedTile(Image.get(), 0, Buffer.GetData(), TileBytes) != TileBytes)
        { Receipt.CogError = TEXT("S1M_TILE_DECODE_FAILED: ") + R.Error; return false; }
        const float* Values = reinterpret_cast<const float*>(Buffer.GetData());
        for (int32 I = 0; I < TileBytes / static_cast<tmsize_t>(sizeof(float)); ++I)
        {
            if (std::isfinite(Values[I]) && Values[I] != -999999.0f) ++Receipt.ValidSamples;
            else ++Receipt.NoDataSamples;
        }
        if (bOverview) Receipt.bOverviewTileDecoded = true;
        else Receipt.bBaseTileDecoded = true;
        return true;
    };
    if (!DecodeOneTile(false)) return false;
    uint16 SubIfdCount = 0; toff_t* SubIfdOffsets = nullptr;
    const bool HasSubIfd = TIFFGetField(Image.get(), TIFFTAG_SUBIFD, &SubIfdCount, &SubIfdOffsets)
        && SubIfdCount > 0 && SubIfdOffsets;
    const toff_t FirstSubIfd = HasSubIfd ? SubIfdOffsets[0] : 0;
    if (!(HasSubIfd ? TIFFSetSubDirectory(Image.get(), FirstSubIfd) : TIFFReadDirectory(Image.get())))
    { Receipt.CogError = TEXT("S1M_OVERVIEW_MISSING"); return false; }
    Receipt.OverviewCount = 1;
    return DecodeOneTile(true);
}

bool CountRows(FSQLiteDatabase& Db, const TCHAR* Table, int64& Count, FString& Error)
{
    const FString Sql = FString::Printf(TEXT("SELECT COUNT(*) FROM %s"), Table);
    FSQLitePreparedStatement Statement = Db.PrepareStatement(*Sql);
    if (!Statement.IsValid() || Statement.Step() != ESQLitePreparedStatementStepResult::Row
        || !Statement.GetColumnValueByIndex(0, Count))
    {
        Error = FString::Printf(TEXT("GPKG_TABLE_UNREADABLE: %s: %s"), Table, *Db.GetLastError()); return false;
    }
    return true;
}

uint32 ReadU32(const uint8* Data, bool bLittle)
{
    return bLittle ? static_cast<uint32>(Data[0]) | (static_cast<uint32>(Data[1]) << 8)
        | (static_cast<uint32>(Data[2]) << 16) | (static_cast<uint32>(Data[3]) << 24)
        : static_cast<uint32>(Data[3]) | (static_cast<uint32>(Data[2]) << 8)
        | (static_cast<uint32>(Data[1]) << 16) | (static_cast<uint32>(Data[0]) << 24);
}

double ReadF64(const uint8* Data, bool bLittle)
{
    uint64 Bits = 0;
    if (bLittle) for (int32 I = 7; I >= 0; --I) Bits = (Bits << 8) | Data[I];
    else for (int32 I = 0; I < 8; ++I) Bits = (Bits << 8) | Data[I];
    double Value = 0;
    FMemory::Memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

bool DecodeGpkgPolygon(const TArray<uint8>& Blob)
{
    if (Blob.Num() < 8 + 1 + 4 + 4 + 4 || Blob.Num() > 4 * 1024 * 1024
        || Blob[0] != 'G' || Blob[1] != 'P' || Blob[2] != 0) return false;
    const uint8 Flags = Blob[3];
    const bool bHeaderLittle = (Flags & 1) != 0;
    const uint8 EnvelopeCode = (Flags >> 1) & 7;
    if ((Flags & 0x10) != 0 || EnvelopeCode > 4
        || ReadU32(Blob.GetData() + 4, bHeaderLittle) != 6350) return false;
    const int32 EnvelopeBytes = EnvelopeCode == 0 ? 0 : EnvelopeCode == 1 ? 32
        : EnvelopeCode == 4 ? 64 : 48;
    int64 Position = 8 + EnvelopeBytes;
    if (Position + 9 > Blob.Num()) return false;
    const uint8 WkbEndian = Blob[Position++];
    if (WkbEndian > 1) return false;
    const bool bWkbLittle = WkbEndian == 1;
    if (ReadU32(Blob.GetData() + Position, bWkbLittle) != 3) return false;
    Position += 4;
    const uint32 RingCount = ReadU32(Blob.GetData() + Position, bWkbLittle);
    Position += 4;
    if (RingCount == 0 || RingCount > 64) return false;
    uint64 TotalPoints = 0;
    for (uint32 Ring = 0; Ring < RingCount; ++Ring)
    {
        if (Position + 4 > Blob.Num()) return false;
        const uint32 Points = ReadU32(Blob.GetData() + Position, bWkbLittle);
        Position += 4;
        TotalPoints += Points;
        if (Points < 4 || TotalPoints > 200000 || static_cast<uint64>(Points) >
            static_cast<uint64>(Blob.Num() - Position) / 16) return false;
        double FirstX = 0, FirstY = 0, LastX = 0, LastY = 0;
        for (uint32 Point = 0; Point < Points; ++Point)
        {
            const double X = ReadF64(Blob.GetData() + Position, bWkbLittle);
            const double Y = ReadF64(Blob.GetData() + Position + 8, bWkbLittle);
            Position += 16;
            if (!std::isfinite(X) || !std::isfinite(Y)) return false;
            if (Point == 0) { FirstX = X; FirstY = Y; }
            if (Point == Points - 1) { LastX = X; LastY = Y; }
        }
        if (FirstX != LastX || FirstY != LastY) return false;
    }
    return Position == Blob.Num();
}

bool CheckGpkgFeatureMetadata(FSQLiteDatabase& Db, const TCHAR* Table, FString& Error)
{
    FSQLitePreparedStatement Contents = Db.PrepareStatement(
        TEXT("SELECT data_type, srs_id FROM gpkg_contents WHERE table_name=?"));
    FSQLitePreparedStatement Geometry = Db.PrepareStatement(
        TEXT("SELECT column_name, geometry_type_name, srs_id FROM gpkg_geometry_columns WHERE table_name=?"));
    FString DataType, ColumnName, GeometryType;
    int32 ContentSrs = 0, GeometrySrs = 0;
    if (!Contents.IsValid() || !Geometry.IsValid()
        || !Contents.SetBindingValueByIndex(1, Table)
        || !Geometry.SetBindingValueByIndex(1, Table)
        || Contents.Step() != ESQLitePreparedStatementStepResult::Row
        || Geometry.Step() != ESQLitePreparedStatementStepResult::Row
        || !Contents.GetColumnValueByIndex(0, DataType)
        || !Contents.GetColumnValueByIndex(1, ContentSrs)
        || !Geometry.GetColumnValueByIndex(0, ColumnName)
        || !Geometry.GetColumnValueByIndex(1, GeometryType)
        || !Geometry.GetColumnValueByIndex(2, GeometrySrs)
        || !DataType.Equals(TEXT("features"), ESearchCase::IgnoreCase)
        || !ColumnName.Equals(TEXT("geom"), ESearchCase::IgnoreCase)
        || !GeometryType.Equals(TEXT("POLYGON"), ESearchCase::IgnoreCase)
        || ContentSrs != 6350 || GeometrySrs != 6350)
    {
        Error = FString::Printf(TEXT("GPKG_FEATURE_METADATA_INVALID: %s"), Table);
        return false;
    }
    return true;
}

bool ReadLineageRow(FSQLiteDatabase& Db, const TCHAR* Table, uint32& GeometryBytes,
    int32* QualityLevel, FString& Error)
{
    const FString Sql = FString::Printf(TEXT("SELECT * FROM %s LIMIT 1"), Table);
    FSQLitePreparedStatement Statement = Db.PrepareStatement(*Sql);
    if (!Statement.IsValid() || Statement.Step() != ESQLitePreparedStatementStepResult::Row)
    { Error = FString::Printf(TEXT("GPKG_LINEAGE_ROW_UNREADABLE: %s"), Table); return false; }
    for (int32 Index = 0; Index < Statement.GetColumnNames().Num(); ++Index)
    {
        const FString& Name = Statement.GetColumnNames()[Index];
        ESQLiteColumnType Type = ESQLiteColumnType::Null;
        if (!Statement.GetColumnTypeByIndex(Index, Type)) continue;
        if (Name.Equals(TEXT("quality_level"), ESearchCase::IgnoreCase) && QualityLevel)
            Statement.GetColumnValueByIndex(Index, *QualityLevel);
        if (Name.Equals(TEXT("geom"), ESearchCase::IgnoreCase) && Type == ESQLiteColumnType::Blob)
        {
            TArray<uint8> Blob;
            if (!Statement.GetColumnValueByIndex(Index, Blob) || !DecodeGpkgPolygon(Blob))
            { Error = FString::Printf(TEXT("GPKG_POLYGON_INVALID: %s"), Table); return false; }
            GeometryBytes = Blob.Num();
        }
    }
    if (GeometryBytes == 0)
    { Error = FString::Printf(TEXT("GPKG_LINEAGE_GEOMETRY_MISSING: %s"), Table); return false; }
    return true;
}

bool ProbeGpkg(const FString& Path, SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly))
    {
        Receipt.GeoPackageError = TEXT("GPKG_OPEN_FAILED: ") + Db.GetLastError(); return false;
    }
    bool Ok = CheckGpkgFeatureMetadata(Db, TEXT("s1m_source_inputs"), Receipt.GeoPackageError)
        && CheckGpkgFeatureMetadata(Db, TEXT("s1m_blending_area_statistics"), Receipt.GeoPackageError)
        && CountRows(Db, TEXT("s1m_source_inputs"), Receipt.SourceInputRows, Receipt.GeoPackageError)
        && CountRows(Db, TEXT("s1m_blending_area_statistics"), Receipt.BlendingRows, Receipt.GeoPackageError);
    if (Ok && Receipt.SourceInputRows == 0)
    {
        Receipt.GeoPackageError = TEXT("GPKG_SOURCE_INPUTS_EMPTY");
        Ok = false;
    }
    if (Ok) Ok = ReadLineageRow(Db, TEXT("s1m_source_inputs"), Receipt.SourceGeometryBytes,
            &Receipt.QualityLevel, Receipt.GeoPackageError)
        && (Receipt.BlendingRows == 0 || ReadLineageRow(Db, TEXT("s1m_blending_area_statistics"),
            Receipt.BlendingGeometryBytes, nullptr, Receipt.GeoPackageError));
    if (!Db.Close())
    {
        Receipt.GeoPackageError = TEXT("GPKG_CLOSE_FAILED");
        return false;
    }
    return Ok;
}

bool ProbeProj(SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
#if UE_BUILD_SHIPPING
    // Only GeoReferencing's PROJ context has Unreal's UFS SQLite/file adapters.
    Receipt.ProjError = TEXT("RAW_PROJ_CONTEXT_UNAVAILABLE_IN_SHIPPING_USE_GEOREFERENCING");
    return false;
#else
    PJ_CONTEXT* Context = proj_context_create();
    if (!Context) { Receipt.ProjError = TEXT("PROJ_CONTEXT_FAILED"); return false; }
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GeoReferencing"));
    if (!Plugin)
    {
        Receipt.ProjError = TEXT("GEOREFERENCING_PLUGIN_NOT_FOUND");
        proj_context_destroy(Context);
        return false;
    }
    const FString DataPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/PROJ"));
    const FTCHARToUTF8 Utf8Path(*DataPath);
    const char* SearchPaths[] = {Utf8Path.Get()};
    proj_context_set_search_paths(Context, 1, SearchPaths);
    const auto Transform = [&](const char* From, const char* To, double X, double Y, double& OutX, double& OutY)
    {
        PJ* Raw = proj_create_crs_to_crs(Context, From, To, nullptr);
        if (!Raw) return false;
        PJ* Projection = proj_normalize_for_visualization(Context, Raw);
        proj_destroy(Raw);
        if (!Projection) return false;
        const PJ_COORD Value = proj_trans(Projection, PJ_FWD, proj_coord(X, Y, 0, 0));
        OutX = Value.xy.x; OutY = Value.xy.y;
        proj_destroy(Projection);
        return std::isfinite(OutX) && std::isfinite(OutY);
    };
    // Known Mount Washington point in NAD83(2011); projected CRS 6350 must resolve from staged proj.db.
    double East = 0, North = 0, Lon = 0, Lat = 0;
    bool Ok = Transform("EPSG:6318", "EPSG:6350", -71.3033, 44.2706, East, North)
        && Transform("EPSG:6350", "EPSG:6318", East, North, Lon, Lat);
    if (Ok)
    {
        Receipt.ProjRoundTripMeters = FMath::Sqrt(FMath::Square((Lon + 71.3033) * 78500.0)
            + FMath::Square((Lat - 44.2706) * 111000.0));
        Ok = Receipt.ProjRoundTripMeters < 0.01;
    }
    double DummyX = 0, DummyY = 0;
    Ok = Ok && Transform("EPSG:26910", "EPSG:6318", 580000, 5200000, DummyX, DummyY)
        && Transform("EPSG:4269", "EPSG:6318", -121.0, 47.0, DummyX, DummyY)
        && Transform("EPSG:4326", "EPSG:6318", -121.0, 47.0, DummyX, DummyY);
    const double Start = FPlatformTime::Seconds();
    for (int32 I = 0; Ok && I < 32; ++I)
        Ok = Transform("EPSG:6350", "EPSG:6318", East + I, North + I, DummyX, DummyY);
    const double Elapsed = FPlatformTime::Seconds() - Start;
    Receipt.ProjSamplesPerSecond = Elapsed > 0 ? 32.0 / Elapsed : 0;
    if (!Ok) Receipt.ProjError = FString::Printf(TEXT("PROJ_TRANSFORM_FAILED: %s"), UTF8_TO_TCHAR(proj_errno_string(proj_context_errno(Context))));
    proj_context_destroy(Context);
    return Ok;
#endif
}
}

SkiPreparation::FM0RasterProjectionReceipt SkiPreparation::RunM0RasterProjectionProbe(
    ICogByteSource& Cog, const FString& GeoPackagePath)
{
    FM0RasterProjectionReceipt Receipt;
    Receipt.bCogPassed = ProbeCog(Cog, Receipt);
    Receipt.bGeoPackagePassed = ProbeGpkg(GeoPackagePath, Receipt);
    Receipt.bProjPassed = ProbeProj(Receipt);
    return Receipt;
}

SkiPreparation::FM0RasterProjectionReceipt SkiPreparation::RunM0RealS1MCogProbe(const FString& CogUrl)
{
    FM0RasterProjectionReceipt Receipt;
    FM0GatewayCogByteSource Source(CogUrl, 4ULL * 1024 * 1024, 32);
    FString Error;
    if (!Source.Initialize(Error)) Receipt.CogError = Error;
    else Receipt.bCogPassed = ProbeRealS1M(Source, Receipt);
    Receipt.bRealS1MProbe = true;
    Receipt.GatewayRangeRequests = Source.RequestCount();
    Receipt.GatewayRangeBytes = Source.TransferredBytes();
    if (Receipt.GatewayRangeRequests > 32 || Receipt.GatewayRangeBytes > 4ULL * 1024 * 1024)
    { Receipt.bCogPassed = false; Receipt.CogError = TEXT("S1M_RANGE_BUDGET_EXCEEDED"); }
    return Receipt;
}

#if !UE_BUILD_SHIPPING
SkiPreparation::FM0RasterProjectionReceipt SkiPreparation::RunM0GatewayRasterProbe(
    const FString& CogUrl, const FString& CertificateAuthorityPemPath, const FString& GeoPackagePath)
{
    FM0RasterProjectionReceipt Failed;
    if (!CogUrl.StartsWith(TEXT("https://localhost:")))
    {
        Failed.CogError = TEXT("GATEWAY_COG_TEST_URL_INVALID"); return Failed;
    }
    const FString AfterAuthority = CogUrl.Mid(18);
    int32 Slash = INDEX_NONE, Port = 0;
    if (!AfterAuthority.FindChar('/', Slash) ||
        !LexTryParseString(Port, *AfterAuthority.Left(Slash)) || Port < 1 || Port > 65535)
    {
        Failed.CogError = TEXT("GATEWAY_COG_TEST_PORT_INVALID"); return Failed;
    }
    SkiNetGateway::SetTestLoopbackPort(static_cast<uint16>(Port));
    SkiNetGateway::SetTestCertificateAuthority(CertificateAuthorityPemPath);
    FM0GatewayCogByteSource Source(CogUrl);
    FString Error;
    const bool Initialized = Source.Initialize(Error);
    FM0RasterProjectionReceipt Receipt = RunM0RasterProjectionProbe(Source, GeoPackagePath);
    if (!Initialized) { Receipt.bCogPassed = false; Receipt.CogError = Error; }
    Receipt.GatewayRangeRequests = Source.RequestCount();
    Receipt.GatewayRangeBytes = Source.TransferredBytes();
    SkiNetGateway::SetTestCertificateAuthority(FString());
    SkiNetGateway::SetTestLoopbackPort(0);
    return Receipt;
}
#endif

SkiPreparation::FM0RasterProjectionReceipt SkiPreparation::RunM0RasterProjectionProbe(
    const FString& CogPath, const FString& GeoPackagePath)
{
    FLocalCogSource Source(CogPath);
    return RunM0RasterProjectionProbe(Source, GeoPackagePath);
}

FString SkiPreparation::FM0RasterProjectionReceipt::ToJson() const
{
    TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("cogPassed"), bCogPassed);
    O->SetBoolField(TEXT("geoPackagePassed"), bGeoPackagePassed);
    O->SetBoolField(TEXT("projPassed"), bProjPassed);
    O->SetBoolField(TEXT("shippingProjPassed"), bShippingProjPassed);
    O->SetBoolField(TEXT("fallbackProjectionPassed"), bFallbackProjectionPassed);
    O->SetBoolField(TEXT("realS1MProbe"), bRealS1MProbe);
    O->SetBoolField(TEXT("baseTileDecoded"), bBaseTileDecoded);
    O->SetBoolField(TEXT("overviewTileDecoded"), bOverviewTileDecoded);
    O->SetStringField(TEXT("cogError"), CogError);
    O->SetStringField(TEXT("geoPackageError"), GeoPackageError);
    O->SetStringField(TEXT("projError"), ProjError);
    O->SetStringField(TEXT("fallbackProjectionError"), FallbackProjectionError);
    O->SetNumberField(TEXT("width"), Width);
    O->SetNumberField(TEXT("height"), Height);
    O->SetNumberField(TEXT("overviewCount"), OverviewCount);
    O->SetNumberField(TEXT("gatewayRangeRequests"), static_cast<double>(GatewayRangeRequests));
    O->SetNumberField(TEXT("gatewayRangeBytes"), static_cast<double>(GatewayRangeBytes));
    O->SetNumberField(TEXT("validSamples"), static_cast<double>(ValidSamples));
    O->SetNumberField(TEXT("noDataSamples"), static_cast<double>(NoDataSamples));
    O->SetNumberField(TEXT("sourceInputRows"), static_cast<double>(SourceInputRows));
    O->SetNumberField(TEXT("blendingRows"), static_cast<double>(BlendingRows));
    O->SetNumberField(TEXT("qualityLevel"), QualityLevel);
    O->SetNumberField(TEXT("sourceGeometryBytes"), SourceGeometryBytes);
    O->SetNumberField(TEXT("blendingGeometryBytes"), BlendingGeometryBytes);
    O->SetNumberField(TEXT("projRoundTripMeters"), ProjRoundTripMeters);
    O->SetNumberField(TEXT("projControlErrorMeters"), ProjControlErrorMeters);
    O->SetNumberField(TEXT("gnAlbersErrorMeters"), GnAlbersErrorMeters);
    O->SetNumberField(TEXT("gnTransverseMercatorErrorMeters"), GnTransverseMercatorErrorMeters);
    O->SetNumberField(TEXT("ngsDatumErrorMeters"), NgsDatumErrorMeters);
    O->SetNumberField(TEXT("fallbackGnAlbersErrorMeters"), FallbackGnAlbersErrorMeters);
    O->SetNumberField(TEXT("fallbackGnTransverseMercatorErrorMeters"), FallbackGnTransverseMercatorErrorMeters);
    O->SetNumberField(TEXT("fallbackS1MRoundTripMeters"), FallbackS1MRoundTripMeters);
    O->SetNumberField(TEXT("fallbackProjectRoundTripMeters"), FallbackProjectRoundTripMeters);
    O->SetNumberField(TEXT("fallbackDatumApproximationMeters"), FallbackDatumApproximationMeters);
    O->SetNumberField(TEXT("projLatticeMaxErrorMeters"), ProjLatticeMaxErrorMeters);
    O->SetNumberField(TEXT("projSamplesPerSecond"), ProjSamplesPerSecond);
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(O, Writer);
    return Json;
}
