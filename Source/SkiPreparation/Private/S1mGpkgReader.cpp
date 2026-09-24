#include "SkiPreparation/S1mGpkgReader.h"

#include "HAL/FileManager.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

#include <cmath>

namespace
{
using namespace SkiPreparation;

constexpr uint64 GeoPackageApplicationId = 0x47504B47ULL;
constexpr int32 GeoPackageSrsId = 6350;
constexpr double S1mTileSideMeters = 10000.0;
constexpr double MaximumFootprintToleranceMeters = 0.01;
constexpr int32 MaximumSchemaRows = 10000;
constexpr int32 MaximumFieldLength = 512;
constexpr int32 MaximumPolygonBytes = 4 * 1024 * 1024;
constexpr uint32 MaximumPolygonRings = 64;
constexpr uint64 MaximumPolygonPoints = 200000;

struct FFeatureMetadata
{
    FS1mLineageBounds Extent;
    int32 ContentsSrsId = 0;
    int32 GeometrySrsId = 0;
    bool bHasExtent = false;
};

struct FObservedColumn
{
    const TCHAR* Name;
    const TCHAR* DeclaredType;
};

const FObservedColumn SourceColumns[] = {
    {TEXT("fid"), TEXT("INTEGER")}, {TEXT("geom"), TEXT("POLYGON")},
    {TEXT("workunit_id"), TEXT("REAL")}, {TEXT("workunit_name"), TEXT("TEXT")},
    {TEXT("priority_rank"), TEXT("INTEGER")}, {TEXT("percent_area"), TEXT("REAL")},
    {TEXT("collect_start"), TEXT("TEXT")}, {TEXT("collect_end"), TEXT("TEXT")},
    {TEXT("source_dem_pub_date"), TEXT("TEXT")}, {TEXT("quality_level"), TEXT("REAL")},
    {TEXT("data_type"), TEXT("TEXT")}, {TEXT("source_resolution_meters"), TEXT("REAL")},
    {TEXT("horiz_crs_epsg"), TEXT("INTEGER")}, {TEXT("horiz_crs_name"), TEXT("TEXT")},
    {TEXT("vert_crs_epsg"), TEXT("INTEGER")}, {TEXT("vert_crs_name"), TEXT("TEXT")},
    {TEXT("geoid"), TEXT("TEXT")}, {TEXT("sourcedem_link"), TEXT("TEXT")},
    {TEXT("metadata_link"), TEXT("TEXT")}
};

const FObservedColumn BlendColumns[] = {
    {TEXT("fid"), TEXT("INTEGER")}, {TEXT("geom"), TEXT("POLYGON")},
    {TEXT("workunit_name"), TEXT("TEXT")}, {TEXT("secondary_workunit_name"), TEXT("TEXT")},
    {TEXT("blend_type"), TEXT("TEXT")}, {TEXT("min_z_difference"), TEXT("REAL")},
    {TEXT("max_z_difference"), TEXT("REAL")}, {TEXT("mean_z_difference"), TEXT("REAL")},
    {TEXT("pixel_count"), TEXT("REAL")}, {TEXT("std_dev_difference"), TEXT("REAL")}
};

void SetFailure(FString& Code, FString& Detail, const TCHAR* InCode, const FString& InDetail)
{
    Code = InCode;
    Detail = InDetail;
}

bool IsFiniteBounds(const FS1mLineageBounds& Bounds)
{
    return std::isfinite(Bounds.MinX) && std::isfinite(Bounds.MinY)
        && std::isfinite(Bounds.MaxX) && std::isfinite(Bounds.MaxY)
        && Bounds.MinX < Bounds.MaxX && Bounds.MinY < Bounds.MaxY;
}

bool IsCanonicalS1mBounds(const FS1mLineageBounds& Bounds, const double Tolerance)
{
    if (!IsFiniteBounds(Bounds)
        || FMath::Abs((Bounds.MaxX - Bounds.MinX) - S1mTileSideMeters) > Tolerance
        || FMath::Abs((Bounds.MaxY - Bounds.MinY) - S1mTileSideMeters) > Tolerance)
        return false;

    const double GridX = std::round(Bounds.MinX / S1mTileSideMeters) * S1mTileSideMeters;
    const double GridY = std::round(Bounds.MinY / S1mTileSideMeters) * S1mTileSideMeters;
    return FMath::Abs(Bounds.MinX - GridX) <= Tolerance
        && FMath::Abs(Bounds.MinY - GridY) <= Tolerance;
}

bool IsInside(const FS1mLineageBounds& Inner, const FS1mLineageBounds& Outer, const double Tolerance)
{
    return Inner.MinX >= Outer.MinX - Tolerance && Inner.MinY >= Outer.MinY - Tolerance
        && Inner.MaxX <= Outer.MaxX + Tolerance && Inner.MaxY <= Outer.MaxY + Tolerance;
}

uint32 ReadU32(const uint8* Data, const bool bLittleEndian)
{
    return bLittleEndian
        ? static_cast<uint32>(Data[0]) | (static_cast<uint32>(Data[1]) << 8)
            | (static_cast<uint32>(Data[2]) << 16) | (static_cast<uint32>(Data[3]) << 24)
        : static_cast<uint32>(Data[3]) | (static_cast<uint32>(Data[2]) << 8)
            | (static_cast<uint32>(Data[1]) << 16) | (static_cast<uint32>(Data[0]) << 24);
}

double ReadF64(const uint8* Data, const bool bLittleEndian)
{
    uint64 Bits = 0;
    if (bLittleEndian)
        for (int32 Index = 7; Index >= 0; --Index) Bits = (Bits << 8) | Data[Index];
    else
        for (int32 Index = 0; Index < 8; ++Index) Bits = (Bits << 8) | Data[Index];

    double Value = 0.0;
    FMemory::Memcpy(&Value, &Bits, sizeof(Value));
    return Value;
}

bool DecodePolygon(const TArray<uint8>& Blob, FS1mLineageBounds& OutBounds, const double Tolerance)
{
    OutBounds = {};
    if (Blob.Num() < 8 + 1 + 4 + 4 + 4 || Blob.Num() > MaximumPolygonBytes
        || Blob[0] != 'G' || Blob[1] != 'P' || Blob[2] != 0)
        return false;

    const uint8 Flags = Blob[3];
    const uint8 EnvelopeCode = (Flags >> 1) & 7;
    // This reader supports only ordinary 2D GeoPackage POLYGONs in the observed schema.
    if ((Flags & 0xF0) != 0 || EnvelopeCode > 1
        || ReadU32(Blob.GetData() + 4, (Flags & 1) != 0) != GeoPackageSrsId)
        return false;

    int64 Position = 8;
    double HeaderMinX = 0.0, HeaderMinY = 0.0, HeaderMaxX = 0.0, HeaderMaxY = 0.0;
    if (EnvelopeCode == 1)
    {
        if (Position + 32 > Blob.Num()) return false;
        const bool bHeaderLittle = (Flags & 1) != 0;
        HeaderMinX = ReadF64(Blob.GetData() + Position, bHeaderLittle); Position += 8;
        HeaderMaxX = ReadF64(Blob.GetData() + Position, bHeaderLittle); Position += 8;
        HeaderMinY = ReadF64(Blob.GetData() + Position, bHeaderLittle); Position += 8;
        HeaderMaxY = ReadF64(Blob.GetData() + Position, bHeaderLittle); Position += 8;
        if (!std::isfinite(HeaderMinX) || !std::isfinite(HeaderMinY)
            || !std::isfinite(HeaderMaxX) || !std::isfinite(HeaderMaxY)
            || HeaderMinX >= HeaderMaxX || HeaderMinY >= HeaderMaxY)
            return false;
    }

    if (Position + 9 > Blob.Num()) return false;
    const uint8 WkbEndian = Blob[Position++];
    if (WkbEndian > 1) return false;
    const bool bWkbLittle = WkbEndian == 1;
    if (ReadU32(Blob.GetData() + Position, bWkbLittle) != 3) return false;
    Position += 4;
    const uint32 RingCount = ReadU32(Blob.GetData() + Position, bWkbLittle);
    Position += 4;
    if (RingCount == 0 || RingCount > MaximumPolygonRings) return false;

    FS1mLineageBounds Bounds;
    Bounds.MinX = TNumericLimits<double>::Max();
    Bounds.MinY = TNumericLimits<double>::Max();
    Bounds.MaxX = -TNumericLimits<double>::Max();
    Bounds.MaxY = -TNumericLimits<double>::Max();
    uint64 TotalPoints = 0;
    for (uint32 Ring = 0; Ring < RingCount; ++Ring)
    {
        if (Position + 4 > Blob.Num()) return false;
        const uint32 PointCount = ReadU32(Blob.GetData() + Position, bWkbLittle);
        Position += 4;
        TotalPoints += PointCount;
        if (PointCount < 4 || TotalPoints > MaximumPolygonPoints
            || static_cast<uint64>(PointCount) > static_cast<uint64>(Blob.Num() - Position) / 16)
            return false;

        double FirstX = 0.0, FirstY = 0.0, LastX = 0.0, LastY = 0.0;
        for (uint32 Point = 0; Point < PointCount; ++Point)
        {
            const double X = ReadF64(Blob.GetData() + Position, bWkbLittle);
            const double Y = ReadF64(Blob.GetData() + Position + 8, bWkbLittle);
            Position += 16;
            if (!std::isfinite(X) || !std::isfinite(Y)) return false;
            if (Point == 0) { FirstX = X; FirstY = Y; }
            if (Point == PointCount - 1) { LastX = X; LastY = Y; }
            Bounds.MinX = FMath::Min(Bounds.MinX, X);
            Bounds.MinY = FMath::Min(Bounds.MinY, Y);
            Bounds.MaxX = FMath::Max(Bounds.MaxX, X);
            Bounds.MaxY = FMath::Max(Bounds.MaxY, Y);
        }
        if (FirstX != LastX || FirstY != LastY) return false;
    }
    if (Position != Blob.Num() || !IsFiniteBounds(Bounds)) return false;
    if (EnvelopeCode == 1 && (FMath::Abs(Bounds.MinX - HeaderMinX) > Tolerance
        || FMath::Abs(Bounds.MinY - HeaderMinY) > Tolerance
        || FMath::Abs(Bounds.MaxX - HeaderMaxX) > Tolerance
        || FMath::Abs(Bounds.MaxY - HeaderMaxY) > Tolerance))
        return false;

    OutBounds = Bounds;
    return true;
}

bool HasExactColumns(FSQLiteDatabase& Db, const TCHAR* Table,
    const FObservedColumn* Expected, const int32 ExpectedCount)
{
    const FString Sql = FString::Printf(TEXT("PRAGMA table_info('%s')"), Table);
    FSQLitePreparedStatement Statement = Db.PrepareStatement(*Sql);
    if (!Statement.IsValid()) return false;
    TMap<FString, FString> Remaining;
    for (int32 Index = 0; Index < ExpectedCount; ++Index)
        Remaining.Add(Expected[Index].Name, Expected[Index].DeclaredType);
    int32 ReadColumns = 0;
    while (Statement.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Name, DeclaredType;
        if (!Statement.GetColumnValueByIndex(1, Name)
            || !Statement.GetColumnValueByIndex(2, DeclaredType)) return false;
        const FString* ExpectedType = Remaining.Find(Name);
        if (!ExpectedType || !DeclaredType.Equals(*ExpectedType, ESearchCase::IgnoreCase)) return false;
        Remaining.Remove(Name);
        ++ReadColumns;
    }
    return ReadColumns == ExpectedCount && Remaining.IsEmpty();
}

bool ReadApplicationId(FSQLiteDatabase& Db, int64& OutApplicationId)
{
    FSQLitePreparedStatement Statement = Db.PrepareStatement(TEXT("PRAGMA application_id"));
    return Statement.IsValid()
        && Statement.Step() == ESQLitePreparedStatementStepResult::Row
        && Statement.GetColumnValueByIndex(0, OutApplicationId);
}

bool ReadCount(FSQLiteDatabase& Db, const TCHAR* Sql, int64& OutCount)
{
    FSQLitePreparedStatement Statement = Db.PrepareStatement(Sql);
    return Statement.IsValid()
        && Statement.Step() == ESQLitePreparedStatementStepResult::Row
        && Statement.GetColumnValueByIndex(0, OutCount);
}

bool ReadFeatureMetadata(FSQLiteDatabase& Db, const TCHAR* Table, FFeatureMetadata& Out,
    FString& FailureCode, FString& FailureDetail)
{
    const int64 TableNameCount = [&]()
    {
        const FString CountSql = FString::Printf(TEXT("SELECT COUNT(*) FROM gpkg_contents WHERE table_name='%s'"), Table);
        int64 Count = 0;
        return ReadCount(Db, *CountSql, Count) ? Count : -1;
    }();
    const int64 GeometryNameCount = [&]()
    {
        const FString CountSql = FString::Printf(TEXT("SELECT COUNT(*) FROM gpkg_geometry_columns WHERE table_name='%s'"), Table);
        int64 Count = 0;
        return ReadCount(Db, *CountSql, Count) ? Count : -1;
    }();
    if (TableNameCount != 1 || GeometryNameCount != 1)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            FString::Printf(TEXT("%s is missing or has duplicate GeoPackage metadata rows."), Table));
        return false;
    }

    const FString ContentsSql = FString::Printf(
        TEXT("SELECT data_type, identifier, srs_id, min_x, min_y, max_x, max_y FROM gpkg_contents WHERE table_name='%s'"), Table);
    FSQLitePreparedStatement Contents = Db.PrepareStatement(*ContentsSql);
    FString DataType, Identifier;
    int32 ContentsSrs = 0;
    if (!Contents.IsValid() || Contents.Step() != ESQLitePreparedStatementStepResult::Row
        || !Contents.GetColumnValueByIndex(0, DataType)
        || !Contents.GetColumnValueByIndex(1, Identifier)
        || !Contents.GetColumnValueByIndex(2, ContentsSrs)
        || !DataType.Equals(TEXT("features"), ESearchCase::CaseSensitive)
        || !Identifier.Equals(Table, ESearchCase::CaseSensitive)
        || ContentsSrs != GeoPackageSrsId)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            FString::Printf(TEXT("%s gpkg_contents feature type or SRS is invalid."), Table));
        return false;
    }

    ESQLiteColumnType ExtentTypes[4] = {ESQLiteColumnType::Null, ESQLiteColumnType::Null,
        ESQLiteColumnType::Null, ESQLiteColumnType::Null};
    bool bExtentTypesValid = true;
    for (int32 Index = 0; Index < 4; ++Index)
        bExtentTypesValid = bExtentTypesValid && Contents.GetColumnTypeByIndex(Index + 3, ExtentTypes[Index]);
    const bool bAllExtentNull = ExtentTypes[0] == ESQLiteColumnType::Null
        && ExtentTypes[1] == ESQLiteColumnType::Null && ExtentTypes[2] == ESQLiteColumnType::Null
        && ExtentTypes[3] == ESQLiteColumnType::Null;
    const bool bAllExtentNumeric = bExtentTypesValid
        && ExtentTypes[0] != ESQLiteColumnType::Null && ExtentTypes[1] != ESQLiteColumnType::Null
        && ExtentTypes[2] != ESQLiteColumnType::Null && ExtentTypes[3] != ESQLiteColumnType::Null;
    if (bAllExtentNumeric)
    {
        Out.bHasExtent = Contents.GetColumnValueByIndex(3, Out.Extent.MinX)
            && Contents.GetColumnValueByIndex(4, Out.Extent.MinY)
            && Contents.GetColumnValueByIndex(5, Out.Extent.MaxX)
            && Contents.GetColumnValueByIndex(6, Out.Extent.MaxY)
            && IsFiniteBounds(Out.Extent);
    }
    if (!bExtentTypesValid || (!bAllExtentNull && !bAllExtentNumeric)
        || (bAllExtentNumeric && !Out.bHasExtent))
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            FString::Printf(TEXT("%s gpkg_contents extent must be either fully absent or finite and ordered."), Table));
        return false;
    }

    const FString GeometrySql = FString::Printf(
        TEXT("SELECT column_name, geometry_type_name, srs_id, z, m FROM gpkg_geometry_columns WHERE table_name='%s'"), Table);
    FSQLitePreparedStatement Geometry = Db.PrepareStatement(*GeometrySql);
    FString ColumnName, GeometryType;
    int32 GeometrySrs = 0, Z = -1, M = -1;
    if (!Geometry.IsValid() || Geometry.Step() != ESQLitePreparedStatementStepResult::Row
        || !Geometry.GetColumnValueByIndex(0, ColumnName)
        || !Geometry.GetColumnValueByIndex(1, GeometryType)
        || !Geometry.GetColumnValueByIndex(2, GeometrySrs)
        || !Geometry.GetColumnValueByIndex(3, Z) || !Geometry.GetColumnValueByIndex(4, M)
        || !ColumnName.Equals(TEXT("geom"), ESearchCase::CaseSensitive)
        || !GeometryType.Equals(TEXT("POLYGON"), ESearchCase::CaseSensitive)
        || GeometrySrs != GeoPackageSrsId || Z != 0 || M != 0)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            FString::Printf(TEXT("%s gpkg_geometry_columns must declare 2D POLYGON geom in EPSG:6350."), Table));
        return false;
    }

    Out.ContentsSrsId = ContentsSrs;
    Out.GeometrySrsId = GeometrySrs;
    return true;
}

bool ValidateSrs6350(FSQLiteDatabase& Db, FString& FailureCode, FString& FailureDetail)
{
    FSQLitePreparedStatement Count = Db.PrepareStatement(
        TEXT("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id=6350"));
    int64 Rows = 0;
    if (!Count.IsValid() || Count.Step() != ESQLitePreparedStatementStepResult::Row
        || !Count.GetColumnValueByIndex(0, Rows) || Rows != 1)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            TEXT("gpkg_spatial_ref_sys must contain one EPSG:6350 definition."));
        return false;
    }
    FSQLitePreparedStatement Statement = Db.PrepareStatement(
        TEXT("SELECT organization, organization_coordsys_id FROM gpkg_spatial_ref_sys WHERE srs_id=6350"));
    FString Organization;
    int32 OrganizationCode = 0;
    if (!Statement.IsValid() || Statement.Step() != ESQLitePreparedStatementStepResult::Row
        || !Statement.GetColumnValueByIndex(0, Organization)
        || !Statement.GetColumnValueByIndex(1, OrganizationCode)
        || !Organization.Equals(TEXT("EPSG"), ESearchCase::CaseSensitive)
        || OrganizationCode != GeoPackageSrsId)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            TEXT("gpkg_spatial_ref_sys EPSG:6350 authority metadata is inconsistent."));
        return false;
    }
    return true;
}

bool IsNumericColumn(FSQLitePreparedStatement& Statement, const int32 Index)
{
    ESQLiteColumnType Type = ESQLiteColumnType::Null;
    return Statement.GetColumnTypeByIndex(Index, Type)
        && (Type == ESQLiteColumnType::Integer || Type == ESQLiteColumnType::Float);
}

bool IsTextColumn(FSQLitePreparedStatement& Statement, const int32 Index)
{
    ESQLiteColumnType Type = ESQLiteColumnType::Null;
    return Statement.GetColumnTypeByIndex(Index, Type) && Type == ESQLiteColumnType::String;
}

bool IsBlobColumn(FSQLitePreparedStatement& Statement, const int32 Index)
{
    ESQLiteColumnType Type = ESQLiteColumnType::Null;
    return Statement.GetColumnTypeByIndex(Index, Type) && Type == ESQLiteColumnType::Blob;
}

bool ReadText(FSQLitePreparedStatement& Statement, const int32 Index, FString& Out,
    const int32 MaxLength)
{
    return IsTextColumn(Statement, Index) && Statement.GetColumnValueByIndex(Index, Out)
        && !Out.IsEmpty() && Out.Len() <= MaxLength;
}

bool ReadNumeric(FSQLitePreparedStatement& Statement, const int32 Index, double& Out)
{
    return IsNumericColumn(Statement, Index) && Statement.GetColumnValueByIndex(Index, Out)
        && std::isfinite(Out);
}

bool ReadInteger(FSQLitePreparedStatement& Statement, const int32 Index, int64& Out)
{
    ESQLiteColumnType Type = ESQLiteColumnType::Null;
    return Statement.GetColumnTypeByIndex(Index, Type) && Type == ESQLiteColumnType::Integer
        && Statement.GetColumnValueByIndex(Index, Out);
}

bool AddBounds(FS1mLineageBounds& Aggregate, bool& bHasBounds, const FS1mLineageBounds& Bounds)
{
    if (!IsFiniteBounds(Bounds)) return false;
    if (!bHasBounds)
    {
        Aggregate = Bounds;
        bHasBounds = true;
        return true;
    }
    Aggregate.MinX = FMath::Min(Aggregate.MinX, Bounds.MinX);
    Aggregate.MinY = FMath::Min(Aggregate.MinY, Bounds.MinY);
    Aggregate.MaxX = FMath::Max(Aggregate.MaxX, Bounds.MaxX);
    Aggregate.MaxY = FMath::Max(Aggregate.MaxY, Bounds.MaxY);
    return true;
}

bool BoundsEqual(const FS1mLineageBounds& A, const FS1mLineageBounds& B, const double Tolerance)
{
    return FMath::Abs(A.MinX - B.MinX) <= Tolerance && FMath::Abs(A.MinY - B.MinY) <= Tolerance
        && FMath::Abs(A.MaxX - B.MaxX) <= Tolerance && FMath::Abs(A.MaxY - B.MaxY) <= Tolerance;
}

bool IsQualityLevel(const double Value, int32& OutQuality)
{
    if (Value < 0 || Value > 5 || FMath::FloorToDouble(Value) != Value) return false;
    const int32 Quality = static_cast<int32>(Value);
    if (Quality != 0 && Quality != 1 && Quality != 2 && Quality != 3 && Quality != 5) return false;
    OutQuality = Quality;
    return true;
}

bool IsNavd88SourceVerticalCrs(const int32 Epsg, const FString& Name)
{
    FString Token;
    for (const TCHAR Character : Name)
        if (FChar::IsAlnum(Character)) Token.AppendChar(FChar::ToUpper(Character));
    return Epsg == 5703 && (Token == TEXT("NAVD88HEIGHT") || Token == TEXT("NAVD88"));
}

bool ReadSourceRows(FSQLiteDatabase& Db, const FFeatureMetadata& Metadata,
    const FS1mLineageLimits& Limits, FS1mGeoPackageEvidence& Out,
    FString& FailureCode, FString& FailureDetail)
{
    if (!Metadata.bHasExtent)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_TILE_EXTENT_INVALID"),
            TEXT("s1m_source_inputs must declare its canonical tile extent in gpkg_contents."));
        return false;
    }
    int64 RowCount = 0;
    if (!ReadCount(Db, TEXT("SELECT COUNT(*) FROM s1m_source_inputs"), RowCount)
        || RowCount <= 0 || RowCount > Limits.MaxSourceInputRows)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_SOURCE_ROWS_INVALID"),
            TEXT("s1m_source_inputs is unreadable, empty, or exceeds the configured row bound."));
        return false;
    }

    FSQLitePreparedStatement Rows = Db.PrepareStatement(TEXT(
        "SELECT fid, geom, workunit_id, workunit_name, collect_start, collect_end, quality_level, "
        "data_type, source_resolution_meters, horiz_crs_epsg, horiz_crs_name, vert_crs_epsg, vert_crs_name "
        "FROM s1m_source_inputs ORDER BY fid"));
    if (!Rows.IsValid())
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_SOURCE_ROWS_INVALID"),
            TEXT("s1m_source_inputs could not be prepared for the bounded read."));
        return false;
    }

    FS1mLineageBounds Aggregate;
    bool bHasAggregate = false;
    TSet<FString> FeatureIds;
    int32 ReadRows = 0;
    while (Rows.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        int64 FID = 0;
        double WorkunitId = 0.0, QualityValue = 0.0, Resolution = 0.0;
        FString WorkunitName, CollectStart, CollectEnd, DataType, HorizontalName, VerticalName;
        int32 HorizontalEpsg = 0, VerticalEpsg = 0;
        TArray<uint8> Geometry;
        if (!ReadInteger(Rows, 0, FID) || FID <= 0 || !IsBlobColumn(Rows, 1)
            || !Rows.GetColumnValueByIndex(1, Geometry)
            || !ReadNumeric(Rows, 2, WorkunitId) || WorkunitId <= 0
            || WorkunitId > 9.0e15 || FMath::FloorToDouble(WorkunitId) != WorkunitId
            || !ReadText(Rows, 3, WorkunitName, Limits.MaxStringLength)
            || !ReadText(Rows, 4, CollectStart, Limits.MaxStringLength)
            || !ReadText(Rows, 5, CollectEnd, Limits.MaxStringLength)
            || !ReadNumeric(Rows, 6, QualityValue)
            || !ReadText(Rows, 7, DataType, Limits.MaxStringLength)
            || !ReadNumeric(Rows, 8, Resolution) || Resolution <= 0.0
            || !Rows.GetColumnValueByIndex(9, HorizontalEpsg) || HorizontalEpsg <= 0
            || !ReadText(Rows, 10, HorizontalName, Limits.MaxStringLength)
            || !Rows.GetColumnValueByIndex(11, VerticalEpsg)
            || !ReadText(Rows, 12, VerticalName, Limits.MaxStringLength))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_SOURCE_RECORD_INVALID"),
                FString::Printf(TEXT("s1m_source_inputs row %d has a missing, wrongly typed, or unbounded value."), ReadRows));
            return false;
        }
        if (Geometry.Num() > MaximumPolygonBytes)
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                FString::Printf(TEXT("s1m_source_inputs row %d exceeds the per-polygon byte bound."), ReadRows));
            return false;
        }

        FS1mSourceInputEvidence Source;
        Source.FeatureId = LexToString(FID);
        Source.SourceId = FString::Printf(TEXT("%.0f"), WorkunitId);
        Source.ProjectName = WorkunitName;
        Source.HorizontalCrs = FString::Printf(TEXT("EPSG:%d"), HorizontalEpsg);
        Source.VerticalDatum = TEXT("NAVD88");
        Source.CollectionStartDate = CollectStart;
        Source.CollectionEndDate = CollectEnd;
        if (!IsQualityLevel(QualityValue, Source.QualityLevel)
            || !IsNavd88SourceVerticalCrs(VerticalEpsg, VerticalName))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_SOURCE_ENUM_INVALID"),
                FString::Printf(TEXT("s1m_source_inputs row %d has an unsupported quality level or vertical CRS."), ReadRows));
            return false;
        }
        Source.ResolutionMeters = Resolution;
        if (!DecodePolygon(Geometry, Source.Footprint, Limits.FootprintToleranceMeters)
            || !IsInside(Source.Footprint, Metadata.Extent, Limits.FootprintToleranceMeters))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                FString::Printf(TEXT("s1m_source_inputs row %d has an invalid, oversized, or out-of-extent polygon."), ReadRows));
            return false;
        }
        Source.bGeometryValidated = true;
        Source.bHorizontalCrsExplicit = true;
        Source.bVerticalDatumExplicit = true;
        Source.bCollectionDatesExplicit = true;
        Source.bQualityLevelExplicit = true;
        Source.bResolutionExplicit = true;
        if (FeatureIds.Contains(Source.FeatureId))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_SOURCE_DUPLICATE"),
                TEXT("s1m_source_inputs repeats a feature id."));
            return false;
        }
        FeatureIds.Add(Source.FeatureId);
        if (!AddBounds(Aggregate, bHasAggregate, Source.Footprint))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                TEXT("s1m_source_inputs contains a polygon with invalid bounds."));
            return false;
        }
        Out.SourceInputs.Add(MoveTemp(Source));
        ++ReadRows;
    }

    if (ReadRows != RowCount || !bHasAggregate || !Metadata.bHasExtent
        || !IsCanonicalS1mBounds(Metadata.Extent, Limits.FootprintToleranceMeters)
        || !BoundsEqual(Aggregate, Metadata.Extent, Limits.FootprintToleranceMeters))
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_TILE_EXTENT_INVALID"),
            TEXT("s1m_source_inputs feature extents must agree with its canonical aligned 10 km gpkg_contents extent."));
        return false;
    }
    Out.TileFootprint = Metadata.Extent;
    Out.bTileFootprintExplicit = true;
    return true;
}

bool ReadBlendRows(FSQLiteDatabase& Db, const FFeatureMetadata& Metadata,
    const FS1mLineageLimits& Limits, FS1mGeoPackageEvidence& Out,
    FString& FailureCode, FString& FailureDetail)
{
    int64 RowCount = 0;
    if (!ReadCount(Db, TEXT("SELECT COUNT(*) FROM s1m_blending_area_statistics"), RowCount)
        || RowCount < 0 || RowCount > Limits.MaxBlendAreaRows)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_ROWS_INVALID"),
            TEXT("s1m_blending_area_statistics is unreadable or exceeds the configured row bound."));
        return false;
    }
    if (RowCount == 0)
    {
        if (Metadata.bHasExtent)
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
                TEXT("An empty s1m_blending_area_statistics table must not declare a spatial extent."));
            return false;
        }
        return true;
    }
    if (!Metadata.bHasExtent)
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_METADATA_INVALID"),
            TEXT("A nonempty s1m_blending_area_statistics table must declare its feature extent."));
        return false;
    }

    FSQLitePreparedStatement Rows = Db.PrepareStatement(TEXT(
        "SELECT fid, geom, workunit_name, secondary_workunit_name, blend_type "
        "FROM s1m_blending_area_statistics ORDER BY fid"));
    if (!Rows.IsValid())
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_ROWS_INVALID"),
            TEXT("s1m_blending_area_statistics could not be prepared for the bounded read."));
        return false;
    }

    FS1mLineageBounds Aggregate;
    bool bHasAggregate = false;
    TSet<FString> FeatureIds;
    int32 ReadRows = 0;
    while (Rows.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        int64 FID = 0;
        FString WorkunitName, SecondaryWorkunitName, BlendType;
        TArray<uint8> Geometry;
        FS1mLineageAreaEvidence Area;
        if (!ReadInteger(Rows, 0, FID) || FID <= 0 || !IsBlobColumn(Rows, 1)
            || !Rows.GetColumnValueByIndex(1, Geometry)
            || !ReadText(Rows, 2, WorkunitName, Limits.MaxStringLength)
            || !ReadText(Rows, 3, SecondaryWorkunitName, Limits.MaxStringLength)
            || !ReadText(Rows, 4, BlendType, Limits.MaxStringLength))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_RECORD_INVALID"),
                FString::Printf(TEXT("s1m_blending_area_statistics row %d has a missing, wrongly typed, or unbounded value."), ReadRows));
            return false;
        }
        if (Geometry.Num() > MaximumPolygonBytes)
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                FString::Printf(TEXT("s1m_blending_area_statistics row %d exceeds the per-polygon byte bound."), ReadRows));
            return false;
        }

        BlendType.TrimStartAndEndInline();
        if (!BlendType.Equals(TEXT("linear blend"), ESearchCase::IgnoreCase))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_ENUM_UNKNOWN"),
                FString::Printf(TEXT("s1m_blending_area_statistics row %d has an unrecognized blend_type '%s'."), ReadRows, *BlendType));
            return false;
        }
        Area.FeatureId = LexToString(FID);
        Area.Kind = ES1mLineageAreaKind::Blend;
        if (!DecodePolygon(Geometry, Area.Footprint, Limits.FootprintToleranceMeters)
            || !IsInside(Area.Footprint, Out.TileFootprint, Limits.FootprintToleranceMeters))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                FString::Printf(TEXT("s1m_blending_area_statistics row %d has an invalid, oversized, or out-of-tile polygon."), ReadRows));
            return false;
        }
        Area.bGeometryValidated = true;
        Area.bKindExplicit = true;
        if (FeatureIds.Contains(Area.FeatureId))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_DUPLICATE"),
                TEXT("s1m_blending_area_statistics repeats a fid."));
            return false;
        }
        FeatureIds.Add(Area.FeatureId);
        if (!AddBounds(Aggregate, bHasAggregate, Area.Footprint))
        {
            SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_POLYGON_INVALID"),
                TEXT("s1m_blending_area_statistics contains a polygon with invalid bounds."));
            return false;
        }
        Out.BlendAreas.Add(MoveTemp(Area));
        ++ReadRows;
    }

    if (ReadRows != RowCount || !bHasAggregate
        || !BoundsEqual(Aggregate, Metadata.Extent, Limits.FootprintToleranceMeters))
    {
        SetFailure(FailureCode, FailureDetail, TEXT("S1M_GPKG_BLEND_EXTENT_INVALID"),
            TEXT("s1m_blending_area_statistics feature extents must agree with its gpkg_contents extent."));
        return false;
    }
    return true;
}

bool IsSafeArtifact(const FS1mLineageArtifactEvidence& Artifact)
{
    if (!Artifact.Product.Equals(TEXT("S1M"), ESearchCase::CaseSensitive)
        || Artifact.TileId.IsEmpty() || Artifact.TileId.Len() > MaximumFieldLength
        || Artifact.PublicationDate.IsEmpty() || Artifact.PublicationDate.Len() != 10
        || !Artifact.bExactSizeProven || Artifact.ObjectBytes == 0)
        return false;
    for (const TCHAR Character : Artifact.TileId)
        if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-'))) return false;
    for (int32 Index = 0; Index < Artifact.PublicationDate.Len(); ++Index)
        if (Index != 4 && Index != 7 && !FChar::IsDigit(Artifact.PublicationDate[Index])) return false;
    return Artifact.PublicationDate[4] == TEXT('-') && Artifact.PublicationDate[7] == TEXT('-');
}
}

bool SkiPreparation::ReadS1mGeoPackage(const FString& Path,
    const FS1mLineageArtifactEvidence& Artifact, const FS1mLineageLimits& Limits,
    FS1mGeoPackageEvidence& OutEvidence, FString& OutFailureCode, FString& OutFailureDetail)
{
    OutEvidence = {};
    OutFailureCode.Empty();
    OutFailureDetail.Empty();
    if (Path.IsEmpty() || !IsSafeArtifact(Artifact)
        || Limits.MaxGeoPackageBytes == 0 || Limits.MaxGeoPackageBytes > 64ULL * 1024ULL * 1024ULL
        || Limits.MaxSourceInputRows <= 0 || Limits.MaxSourceInputRows > MaximumSchemaRows
        || Limits.MaxBlendAreaRows < 0 || Limits.MaxBlendAreaRows > MaximumSchemaRows
        || Limits.MaxStringLength <= 0 || Limits.MaxStringLength > MaximumFieldLength
        || !FMath::IsFinite(Limits.FootprintToleranceMeters)
        || Limits.FootprintToleranceMeters < 0.0 || Limits.FootprintToleranceMeters > MaximumFootprintToleranceMeters)
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_INPUT_INVALID"),
            TEXT("GeoPackage path, artifact identity/size evidence, or configured bounds are invalid."));
        return false;
    }

    const int64 FileBytes = IFileManager::Get().FileSize(*Path);
    if (FileBytes <= 0 || static_cast<uint64>(FileBytes) > Limits.MaxGeoPackageBytes
        || static_cast<uint64>(FileBytes) != Artifact.ObjectBytes)
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_SIZE_INVALID"),
            TEXT("The local sidecar byte length is empty, over the configured cap, or differs from the exact artifact size."));
        return false;
    }

    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly))
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_OPEN_FAILED"),
            TEXT("SQLiteCore could not open the sidecar read-only: ") + Db.GetLastError());
        return false;
    }

    FS1mGeoPackageEvidence Evidence;
    Evidence.Artifact = Artifact;
    bool bRead = [&]()
    {
        int64 ApplicationId = 0;
        if (!ReadApplicationId(Db, ApplicationId) || static_cast<uint64>(ApplicationId) != GeoPackageApplicationId)
        {
            SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_APPLICATION_ID_INVALID"),
                TEXT("SQLite application_id is not the GeoPackage signature."));
            return false;
        }
        if (!ValidateSrs6350(Db, OutFailureCode, OutFailureDetail)) return false;
        if (!HasExactColumns(Db, TEXT("s1m_source_inputs"), SourceColumns, UE_ARRAY_COUNT(SourceColumns))
            || !HasExactColumns(Db, TEXT("s1m_blending_area_statistics"), BlendColumns, UE_ARRAY_COUNT(BlendColumns)))
        {
            SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_SCHEMA_INVALID"),
                TEXT("The sidecar must contain exactly the two observed S1M feature-table column sets."));
            return false;
        }

        FFeatureMetadata SourceMetadata, BlendMetadata;
        if (!ReadFeatureMetadata(Db, TEXT("s1m_source_inputs"), SourceMetadata, OutFailureCode, OutFailureDetail)
            || !ReadFeatureMetadata(Db, TEXT("s1m_blending_area_statistics"), BlendMetadata, OutFailureCode, OutFailureDetail))
            return false;
        if (!ReadSourceRows(Db, SourceMetadata, Limits, Evidence, OutFailureCode, OutFailureDetail)
            || !ReadBlendRows(Db, BlendMetadata, Limits, Evidence, OutFailureCode, OutFailureDetail))
            return false;

        Evidence.bRequiredTablesPresent = true;
        Evidence.bFeatureMetadataValidated = true;
        Evidence.SourceInputsContentsSrsId = SourceMetadata.ContentsSrsId;
        Evidence.SourceInputsGeometrySrsId = SourceMetadata.GeometrySrsId;
        Evidence.BlendContentsSrsId = BlendMetadata.ContentsSrsId;
        Evidence.BlendGeometrySrsId = BlendMetadata.GeometrySrsId;
        return true;
    }();

    const bool bClosed = Db.Close();
    if (!bClosed && bRead)
    {
        bRead = false;
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_GPKG_CLOSE_FAILED"),
            TEXT("SQLiteCore could not close the read-only GeoPackage connection."));
    }
    if (!bRead || !bClosed)
    {
        OutEvidence = {};
        return false;
    }

    OutEvidence = MoveTemp(Evidence);
    return true;
}
