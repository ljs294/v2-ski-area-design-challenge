#include "SkiPreparation/S1mGpkgReader.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

namespace
{
constexpr int32 MaximumPolygonBytes = 4 * 1024 * 1024;

struct FGpkgFixtureOptions
{
    bool bExtraSourceColumn = false;
    bool bMissingSourceColumn = false;
    bool bOversizedFirstPolygon = false;
    FString BlendType = TEXT("linear blend");
};

void AppendU32(TArray<uint8>& Bytes, const uint32 Value)
{
    for (int32 Index = 0; Index < 4; ++Index)
        Bytes.Add(static_cast<uint8>(Value >> (Index * 8)));
}

void AppendF64(TArray<uint8>& Bytes, const double Value)
{
    uint64 Bits = 0;
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    for (int32 Index = 0; Index < 8; ++Index)
        Bytes.Add(static_cast<uint8>(Bits >> (Index * 8)));
}

TArray<uint8> MakePolygon(const double MinX, const double MinY, const double MaxX, const double MaxY)
{
    TArray<uint8> Blob = {'G', 'P', 0, 1}; // GeoPackage v0, little-endian header, no envelope.
    AppendU32(Blob, 6350);
    Blob.Add(1); // WKB little endian.
    AppendU32(Blob, 3); // Polygon.
    AppendU32(Blob, 1); // One exterior ring.
    AppendU32(Blob, 5);
    const double Points[5][2] = {
        {MinX, MinY}, {MaxX, MinY}, {MaxX, MaxY}, {MinX, MaxY}, {MinX, MinY}
    };
    for (const auto& Point : Points)
    {
        AppendF64(Blob, Point[0]);
        AppendF64(Blob, Point[1]);
    }
    return Blob;
}

bool InsertSource(FSQLiteDatabase& Db, const TArray<uint8>& Geometry,
    const int32 WorkunitId, const TCHAR* WorkunitName, const TCHAR* CollectStart,
    const TCHAR* CollectEnd, const int32 Priority, const double PercentArea)
{
    FSQLitePreparedStatement Statement = Db.PrepareStatement(TEXT(
        "INSERT INTO s1m_source_inputs (geom,workunit_id,workunit_name,priority_rank,percent_area,"
        "collect_start,collect_end,source_dem_pub_date,quality_level,data_type,source_resolution_meters,"
        "horiz_crs_epsg,horiz_crs_name,vert_crs_epsg,vert_crs_name,geoid,sourcedem_link,metadata_link) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    return Statement.IsValid()
        && Statement.SetBindingValueByIndex(1, Geometry.GetData(), Geometry.Num())
        && Statement.SetBindingValueByIndex(2, static_cast<double>(WorkunitId))
        && Statement.SetBindingValueByIndex(3, WorkunitName)
        && Statement.SetBindingValueByIndex(4, Priority)
        && Statement.SetBindingValueByIndex(5, PercentArea)
        && Statement.SetBindingValueByIndex(6, CollectStart)
        && Statement.SetBindingValueByIndex(7, CollectEnd)
        && Statement.SetBindingValueByIndex(8, TEXT("2024-05-20"))
        && Statement.SetBindingValueByIndex(9, 2.0)
        && Statement.SetBindingValueByIndex(10, TEXT("linear-mode lidar"))
        && Statement.SetBindingValueByIndex(11, 1.0)
        && Statement.SetBindingValueByIndex(12, 6348)
        && Statement.SetBindingValueByIndex(13, TEXT("NAD83(2011) / UTM zone 19N"))
        && Statement.SetBindingValueByIndex(14, 5703)
        && Statement.SetBindingValueByIndex(15, TEXT("NAVD88 height"))
        && Statement.SetBindingValueByIndex(16, TEXT("GEOID12B"))
        && Statement.SetBindingValueByIndex(17, TEXT("https://example.invalid/source"))
        && Statement.SetBindingValueByIndex(18, TEXT("https://example.invalid/metadata"))
        && Statement.Execute();
}

bool InsertBlend(FSQLiteDatabase& Db, const TArray<uint8>& Geometry, const FString& BlendType)
{
    FSQLitePreparedStatement Statement = Db.PrepareStatement(TEXT(
        "INSERT INTO s1m_blending_area_statistics (geom,workunit_name,secondary_workunit_name,blend_type,"
        "min_z_difference,max_z_difference,mean_z_difference,pixel_count,std_dev_difference) "
        "VALUES (?,?,?,?,?,?,?,?,?)"));
    return Statement.IsValid()
        && Statement.SetBindingValueByIndex(1, Geometry.GetData(), Geometry.Num())
        && Statement.SetBindingValueByIndex(2, TEXT("NH_Umbagog_2016"))
        && Statement.SetBindingValueByIndex(3, TEXT("NH_CT_RiverNorthL6_P2_2015"))
        && Statement.SetBindingValueByIndex(4, BlendType)
        && Statement.SetBindingValueByIndex(5, -2.2401)
        && Statement.SetBindingValueByIndex(6, 1.9623)
        && Statement.SetBindingValueByIndex(7, 0.018)
        && Statement.SetBindingValueByIndex(8, 106294.0)
        && Statement.SetBindingValueByIndex(9, 0.1143)
        && Statement.Execute();
}

bool WriteFixture(const FString& Path, const FGpkgFixtureOptions& Options)
{
    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate)) return false;
    bool Ok = Db.Execute(TEXT("PRAGMA application_id=1196444487"))
        && Db.Execute(TEXT("PRAGMA user_version=10400"))
        && Db.Execute(TEXT("CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT NOT NULL,srs_id INTEGER NOT NULL PRIMARY KEY,organization TEXT NOT NULL,organization_coordsys_id INTEGER NOT NULL,definition TEXT NOT NULL,description TEXT)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_spatial_ref_sys VALUES ('NAD83(2011) / Conus Albers',6350,'EPSG',6350,'EPSG:6350','fixture')"))
        && Db.Execute(TEXT("CREATE TABLE gpkg_contents (table_name TEXT NOT NULL PRIMARY KEY,data_type TEXT NOT NULL,identifier TEXT UNIQUE,description TEXT DEFAULT '',last_change DATETIME NOT NULL DEFAULT '2026-01-02T00:00:00Z',min_x DOUBLE,min_y DOUBLE,max_x DOUBLE,max_y DOUBLE,srs_id INTEGER)"))
        && Db.Execute(TEXT("CREATE TABLE gpkg_geometry_columns (table_name TEXT NOT NULL,column_name TEXT NOT NULL,geometry_type_name TEXT NOT NULL,srs_id INTEGER NOT NULL,z TINYINT NOT NULL,m TINYINT NOT NULL,PRIMARY KEY(table_name,column_name))"))
        && Db.Execute(TEXT("INSERT INTO gpkg_contents VALUES ('s1m_source_inputs','features','s1m_source_inputs','', '2026-01-02T00:00:00Z',1940000,2610000,1950000,2620000,6350)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_contents VALUES ('s1m_blending_area_statistics','features','s1m_blending_area_statistics','', '2026-01-02T00:00:00Z',1944000,2614000,1946000,2616000,6350)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_geometry_columns VALUES ('s1m_source_inputs','geom','POLYGON',6350,0,0)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_geometry_columns VALUES ('s1m_blending_area_statistics','geom','POLYGON',6350,0,0)"));

    FString SourceColumns = TEXT(
        "fid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, geom POLYGON, workunit_id REAL, workunit_name TEXT, "
        "priority_rank INTEGER, percent_area REAL, collect_start TEXT, collect_end TEXT, source_dem_pub_date TEXT, "
        "quality_level REAL, data_type TEXT, source_resolution_meters REAL, horiz_crs_epsg INTEGER, "
        "horiz_crs_name TEXT, vert_crs_epsg INTEGER, vert_crs_name TEXT, geoid TEXT, sourcedem_link TEXT");
    if (!Options.bMissingSourceColumn) SourceColumns += TEXT(", metadata_link TEXT");
    if (Options.bExtraSourceColumn) SourceColumns += TEXT(", unexpected_column TEXT");
    Ok = Ok && Db.Execute(*FString::Printf(TEXT("CREATE TABLE s1m_source_inputs (%s)"), *SourceColumns))
        && Db.Execute(TEXT("CREATE TABLE s1m_blending_area_statistics (fid INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, geom POLYGON, workunit_name TEXT, secondary_workunit_name TEXT, blend_type TEXT, min_z_difference REAL, max_z_difference REAL, mean_z_difference REAL, pixel_count REAL, std_dev_difference REAL)"));

    // The exact-column rejection happens before row access, so keep this fixture intentionally empty.
    if (Options.bMissingSourceColumn) return Db.Close() && Ok;

    TArray<uint8> Left = MakePolygon(1940000, 2610000, 1945000, 2620000);
    TArray<uint8> Right = MakePolygon(1945000, 2610000, 1950000, 2620000);
    if (Options.bOversizedFirstPolygon)
    {
        Left.SetNum(MaximumPolygonBytes + 1);
        Left.Init(0, Left.Num());
    }
    Ok = Ok && InsertSource(Db, Left, 59156, TEXT("NH_Umbagog_2016"), TEXT("2016-04-06"), TEXT("2018-05-24"), 1, 0.839)
        && InsertSource(Db, Right, 71535, TEXT("NH_CT_RiverNorthL6_P2_2015"), TEXT("2015-10-24"), TEXT("2015-11-23"), 2, 0.161)
        && InsertBlend(Db, MakePolygon(1944000, 2614000, 1946000, 2616000), Options.BlendType);
    return Db.Close() && Ok;
}

FString FixturePath(const TCHAR* Name)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("S1mGpkgReaderFixtures"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    return FPaths::Combine(Directory, Name);
}

SkiPreparation::FS1mLineageArtifactEvidence FixtureArtifact(const FString& Path)
{
    SkiPreparation::FS1mLineageArtifactEvidence Artifact;
    Artifact.Product = TEXT("S1M");
    Artifact.TileId = TEXT("n2620e1940");
    Artifact.PublicationDate = TEXT("2026-01-02");
    const int64 Size = IFileManager::Get().FileSize(*Path);
    Artifact.ObjectBytes = Size > 0 ? static_cast<uint64>(Size) : 0;
    Artifact.bExactSizeProven = Size > 0;
    return Artifact;
}

bool ReadFixture(const FString& Path, SkiPreparation::FS1mGeoPackageEvidence& Out,
    FString& FailureCode, FString& FailureDetail, const uint64 ClaimedBytes = 0)
{
    SkiPreparation::FS1mLineageArtifactEvidence Artifact = FixtureArtifact(Path);
    if (ClaimedBytes > 0) Artifact.ObjectBytes = ClaimedBytes;
    return SkiPreparation::ReadS1mGeoPackage(Path, Artifact,
        SkiPreparation::FS1mLineageLimits(), Out, FailureCode, FailureDetail);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mGpkgReaderObservedFixtureTest,
    "MountainPlanner.M3.S1mGpkgReader.ReadsObservedSchemaWithoutXMLProof",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mGpkgReaderObservedFixtureTest::RunTest(const FString&)
{
    const FString Path = FixturePath(TEXT("observed-schema.gpkg"));
    IFileManager::Get().Delete(*Path);
    TestTrue(TEXT("generated observed-schema GeoPackage"), WriteFixture(Path, FGpkgFixtureOptions()));

    SkiPreparation::FS1mGeoPackageEvidence Evidence;
    FString FailureCode, FailureDetail;
    TestTrue(TEXT("strict read-only adapter accepts the fixture"), ReadFixture(Path, Evidence, FailureCode, FailureDetail));
    TestTrue(TEXT("both required feature tables recorded"), Evidence.bRequiredTablesPresent);
    TestTrue(TEXT("feature metadata recorded"), Evidence.bFeatureMetadataValidated);
    TestTrue(TEXT("table and geometry SRS IDs are EPSG:6350"),
        Evidence.SourceInputsContentsSrsId == 6350 && Evidence.SourceInputsGeometrySrsId == 6350
        && Evidence.BlendContentsSrsId == 6350 && Evidence.BlendGeometrySrsId == 6350);
    TestTrue(TEXT("source table extent provides the explicit tile footprint"), Evidence.bTileFootprintExplicit);
    TestEqual(TEXT("source rows"), Evidence.SourceInputs.Num(), 2);
    TestEqual(TEXT("blend rows"), Evidence.BlendAreas.Num(), 1);
    if (Evidence.SourceInputs.Num() == 2 && Evidence.BlendAreas.Num() == 1)
    {
        TestEqual(TEXT("source CRS is derived from its explicit EPSG code"), Evidence.SourceInputs[0].HorizontalCrs,
            FString(TEXT("EPSG:6348")));
        TestEqual(TEXT("source workunit identity retained"), Evidence.SourceInputs[0].SourceId, FString(TEXT("59156")));
        TestEqual(TEXT("source QL retained"), Evidence.SourceInputs[0].QualityLevel, 2);
        TestTrue(TEXT("source dates, datum, resolution and polygon are explicit"),
            Evidence.SourceInputs[0].bCollectionDatesExplicit && Evidence.SourceInputs[0].bVerticalDatumExplicit
            && Evidence.SourceInputs[0].bResolutionExplicit && Evidence.SourceInputs[0].bGeometryValidated);
        TestTrue(TEXT("observed blend enum maps to blend"),
            Evidence.BlendAreas[0].Kind == SkiPreparation::ES1mLineageAreaKind::Blend);
        TestTrue(TEXT("artifact identity and exact local byte count retained"),
            Evidence.Artifact.bExactSizeProven && Evidence.Artifact.ObjectBytes == FixtureArtifact(Path).ObjectBytes);
    }
    TestTrue(TEXT("no failure reason on success"), FailureCode.IsEmpty() && FailureDetail.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mGpkgReaderRejectsSchemaAndEnumTest,
    "MountainPlanner.M3.S1mGpkgReader.RejectsUnknownSchemaAndEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mGpkgReaderRejectsSchemaAndEnumTest::RunTest(const FString&)
{
    struct FCase { const TCHAR* Name; FGpkgFixtureOptions Options; const TCHAR* ExpectedCode; };
    FCase Cases[3];
    Cases[0].Name = TEXT("extra-column.gpkg");
    Cases[0].Options.bExtraSourceColumn = true;
    Cases[0].ExpectedCode = TEXT("S1M_GPKG_SCHEMA_INVALID");
    Cases[1].Name = TEXT("missing-column.gpkg");
    Cases[1].Options.bMissingSourceColumn = true;
    Cases[1].ExpectedCode = TEXT("S1M_GPKG_SCHEMA_INVALID");
    Cases[2].Name = TEXT("unknown-enum.gpkg");
    Cases[2].Options.BlendType = TEXT("future blend");
    Cases[2].ExpectedCode = TEXT("S1M_GPKG_BLEND_ENUM_UNKNOWN");
    for (const FCase& Case : Cases)
    {
        const FString Path = FixturePath(Case.Name);
        IFileManager::Get().Delete(*Path);
        if (!TestTrue(FString::Printf(TEXT("fixture %s writes"), Case.Name), WriteFixture(Path, Case.Options))) return false;
        SkiPreparation::FS1mGeoPackageEvidence Evidence;
        FString FailureCode, FailureDetail;
        TestFalse(FString::Printf(TEXT("%s rejected"), Case.Name), ReadFixture(Path, Evidence, FailureCode, FailureDetail));
        TestEqual(FString::Printf(TEXT("%s failure code"), Case.Name), FailureCode, FString(Case.ExpectedCode));
        TestTrue(TEXT("partial rows are cleared on failure"), Evidence.SourceInputs.IsEmpty() && Evidence.BlendAreas.IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mGpkgReaderRejectsOversizeAndSizeMismatchTest,
    "MountainPlanner.M3.S1mGpkgReader.RejectsOversizedGeometryAndArtifactMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mGpkgReaderRejectsOversizeAndSizeMismatchTest::RunTest(const FString&)
{
    const FString OversizedPath = FixturePath(TEXT("oversized-geometry.gpkg"));
    IFileManager::Get().Delete(*OversizedPath);
    FGpkgFixtureOptions Options;
    Options.bOversizedFirstPolygon = true;
    TestTrue(TEXT("oversized fixture writes"), WriteFixture(OversizedPath, Options));
    SkiPreparation::FS1mGeoPackageEvidence Evidence;
    FString FailureCode, FailureDetail;
    TestFalse(TEXT("oversized geometry is rejected"), ReadFixture(OversizedPath, Evidence, FailureCode, FailureDetail));
    TestEqual(TEXT("oversized geometry reason"), FailureCode, FString(TEXT("S1M_GPKG_POLYGON_INVALID")));

    const FString ValidPath = FixturePath(TEXT("artifact-size-mismatch.gpkg"));
    IFileManager::Get().Delete(*ValidPath);
    TestTrue(TEXT("valid fixture writes"), WriteFixture(ValidPath, FGpkgFixtureOptions()));
    FailureCode.Empty(); FailureDetail.Empty();
    const uint64 WrongBytes = FixtureArtifact(ValidPath).ObjectBytes + 1;
    TestFalse(TEXT("wrong caller-pinned byte count rejected"), ReadFixture(ValidPath, Evidence, FailureCode, FailureDetail, WrongBytes));
    TestEqual(TEXT("artifact size reason"), FailureCode, FString(TEXT("S1M_GPKG_SIZE_INVALID")));
    return true;
}
#endif
