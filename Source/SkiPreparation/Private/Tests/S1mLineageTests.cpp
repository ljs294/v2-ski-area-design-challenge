#include "SkiPreparation/S1mLineage.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

#include <limits>

namespace
{
SkiPreparation::FS1mLineageBounds TestTileBounds()
{
    SkiPreparation::FS1mLineageBounds Bounds;
    Bounds.MinX = 500000.0;
    Bounds.MinY = 5200000.0;
    Bounds.MaxX = 510000.0;
    Bounds.MaxY = 5210000.0;
    return Bounds;
}

SkiPreparation::FS1mLineageEvidence ValidEvidence()
{
    using namespace SkiPreparation;
    FS1mLineageEvidence Evidence;
    const auto FillArtifact = [](FS1mLineageArtifactEvidence& Artifact, const uint64 Bytes)
    {
        Artifact.Product = TEXT("S1M");
        Artifact.TileId = TEXT("n4420w07120");
        Artifact.PublicationDate = TEXT("2026-06-16");
        Artifact.ObjectBytes = Bytes;
        Artifact.bExactSizeProven = true;
    };

    FillArtifact(Evidence.Cog.Artifact, 2048);
    Evidence.Cog.bHeaderValidated = true;
    Evidence.Cog.HorizontalCrs = TEXT("EPSG:6350");
    Evidence.Cog.bHorizontalCrsExplicit = true;
    Evidence.Cog.VerticalDatum = TEXT("NAVD88");
    Evidence.Cog.bVerticalDatumExplicit = true;
    Evidence.Cog.TileFootprint = TestTileBounds();

    FillArtifact(Evidence.GeoPackage.Artifact, 4096);
    Evidence.GeoPackage.bRequiredTablesPresent = true;
    Evidence.GeoPackage.bFeatureMetadataValidated = true;
    Evidence.GeoPackage.SourceInputsContentsSrsId = 6350;
    Evidence.GeoPackage.SourceInputsGeometrySrsId = 6350;
    Evidence.GeoPackage.BlendContentsSrsId = 6350;
    Evidence.GeoPackage.BlendGeometrySrsId = 6350;
    Evidence.GeoPackage.bTileFootprintExplicit = true;
    Evidence.GeoPackage.TileFootprint = TestTileBounds();

    FS1mSourceInputEvidence Source;
    Source.FeatureId = TEXT("source-input-1");
    Source.SourceId = TEXT("project-184689");
    Source.ProjectName = TEXT("Mount Washington lidar 2020");
    Source.HorizontalCrs = TEXT("EPSG:26919");
    Source.VerticalDatum = TEXT("NAVD88");
    Source.CollectionStartDate = TEXT("2020-04-08");
    Source.CollectionEndDate = TEXT("2020-05-04");
    Source.QualityLevel = 2;
    Source.ResolutionMeters = 1.0;
    Source.Footprint = TestTileBounds();
    Source.bGeometryValidated = true;
    Source.bHorizontalCrsExplicit = true;
    Source.bVerticalDatumExplicit = true;
    Source.bCollectionDatesExplicit = true;
    Source.bQualityLevelExplicit = true;
    Source.bResolutionExplicit = true;
    Evidence.GeoPackage.SourceInputs.Add(Source);

    FS1mLineageAreaEvidence BlendArea;
    BlendArea.FeatureId = TEXT("blend-area-1");
    BlendArea.Kind = ES1mLineageAreaKind::Blend;
    BlendArea.Footprint.MinX = 504000.0;
    BlendArea.Footprint.MinY = 5204000.0;
    BlendArea.Footprint.MaxX = 506000.0;
    BlendArea.Footprint.MaxY = 5206000.0;
    BlendArea.bGeometryValidated = true;
    BlendArea.bKindExplicit = true;
    Evidence.GeoPackage.BlendAreas.Add(BlendArea);

    FillArtifact(Evidence.Xml.Artifact, 1024);
    Evidence.Xml.bWellFormed = true;
    Evidence.Xml.HorizontalCrs = TEXT("EPSG:6350");
    Evidence.Xml.bHorizontalCrsExplicit = true;
    Evidence.Xml.VerticalDatum = TEXT("North American Vertical Datum of 1988");
    Evidence.Xml.bVerticalDatumExplicit = true;
    Evidence.Xml.VerticalRmseMeters = 0.45;
    Evidence.Xml.bVerticalRmseExplicit = true;
    Evidence.Xml.bTileFootprintExplicit = true;
    Evidence.Xml.TileFootprint = TestTileBounds();
    return Evidence;
}

SkiPreparation::FS1mLineageExpectation ValidExpectation()
{
    SkiPreparation::FS1mLineageExpectation Expected;
    Expected.Product = TEXT("S1M");
    Expected.TileId = TEXT("n4420w07120");
    Expected.PublicationDate = TEXT("2026-06-16");
    Expected.CogObjectBytes = 2048;
    Expected.GeoPackageObjectBytes = 4096;
    Expected.XmlObjectBytes = 1024;
    return Expected;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mLineageValidEvidenceTest,
    "MountainPlanner.M3.S1mLineage.ValidNormalizedEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mLineageValidEvidenceTest::RunTest(const FString&)
{
    SkiPreparation::FS1mLineageReport Report;
    TestTrue(TEXT("complete aligned S1M evidence passes"), SkiPreparation::VerifyS1mLineageEvidence(
        ValidEvidence(), ValidExpectation(), SkiPreparation::FS1mLineageLimits(), Report));
    TestTrue(TEXT("report passes"), Report.bPassed);
    TestEqual(TEXT("stable verified status"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_VERIFIED")));
    TestEqual(TEXT("tile identity retained"), Report.TileId, FString(TEXT("n4420w07120")));
    TestEqual(TEXT("horizontal CRS retained"), Report.HorizontalCrs, FString(TEXT("EPSG:6350")));
    TestEqual(TEXT("vertical datum retained"), Report.VerticalDatum, FString(TEXT("NAVD88")));
    TestTrue(TEXT("RMSE retained"), FMath::IsNearlyEqual(Report.VerticalRmseMeters, 0.45));
    TestEqual(TEXT("source row count"), Report.SourceInputRows, 1);
    TestEqual(TEXT("blend row count"), Report.BlendAreaRows, 1);
    TestEqual(TEXT("all exact object sizes retained"), Report.CogObjectBytes + Report.GeoPackageObjectBytes
        + Report.XmlObjectBytes, 7168ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mLineageRejectsContradictionsTest,
    "MountainPlanner.M3.S1mLineage.RejectsMissingAndContradictoryProofs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mLineageRejectsContradictionsTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.VerticalDatum = TEXT("NGVD29");
        FS1mLineageReport Report;
        TestFalse(TEXT("conflicting output vertical datum rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("datum failure code"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_DATUM_UNPROVEN")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.GeoPackage.Artifact.TileId = TEXT("different-tile");
        FS1mLineageReport Report;
        TestFalse(TEXT("sidecar bound to another tile rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("identity failure code"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_IDENTITY_MISMATCH")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.HorizontalCrs = TEXT("EPSG:4269");
        FS1mLineageReport Report;
        TestFalse(TEXT("contradictory horizontal CRS rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("CRS failure code"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_HORIZONTAL_CRS_UNPROVEN")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.TileFootprint.MaxX += 1.0;
        FS1mLineageReport Report;
        TestFalse(TEXT("contradictory tile footprint rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("footprint failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_TILE_FOOTPRINT_MISMATCH")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.bVerticalDatumExplicit = false;
        FS1mLineageReport Report;
        TestFalse(TEXT("missing explicit vertical proof rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("missing datum failure code"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_DATUM_UNPROVEN")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.VerticalRmseMeters = std::numeric_limits<double>::quiet_NaN();
        FS1mLineageReport Report;
        TestFalse(TEXT("nonfinite XML RMSE rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("RMSE failure uses datum proof gate"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_DATUM_UNPROVEN")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.GeoPackage.SourceInputsGeometrySrsId = 4326;
        FS1mLineageReport Report;
        TestFalse(TEXT("wrong GeoPackage geometry CRS rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("GPKG metadata failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_GPKG_METADATA_INVALID")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.bWellFormed = false;
        FS1mLineageReport Report;
        TestFalse(TEXT("malformed or absent XML proof rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("malformed XML fails CRS evidence gate"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_HORIZONTAL_CRS_UNPROVEN")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.GeoPackage.Artifact.ObjectBytes += 1;
        FS1mLineageReport Report;
        TestFalse(TEXT("pinned exact object size mismatch rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("size failure code"), Report.FailureCode, FString(TEXT("S1M_LINEAGE_SIZE_UNPROVEN")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mLineageRejectsInvalidSourceRowsTest,
    "MountainPlanner.M3.S1mLineage.RejectsInvalidSourceRowsAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mLineageRejectsInvalidSourceRowsTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.GeoPackage.SourceInputs[0].CollectionStartDate.Empty();
        FS1mLineageReport Report;
        TestFalse(TEXT("missing source acquisition date rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("source row failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_SOURCE_RECORD_INVALID")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        FS1mSourceInputEvidence Contradiction = Evidence.GeoPackage.SourceInputs[0];
        Contradiction.FeatureId = TEXT("source-input-2");
        Contradiction.ResolutionMeters = 2.0;
        Evidence.GeoPackage.SourceInputs.Add(Contradiction);
        FS1mLineageReport Report;
        TestFalse(TEXT("same source id with contradictory metadata rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("contradiction failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_SOURCE_CONTRADICTORY")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.GeoPackage.SourceInputs[0].Footprint.MinX -= 0.1;
        FS1mLineageReport Report;
        TestFalse(TEXT("source geometry outside tile rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("outside source geometry failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_SOURCE_RECORD_INVALID")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Cog.TileFootprint.MaxX += 10.0;
        FS1mLineageReport Report;
        TestFalse(TEXT("noncanonical raster footprint rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("noncanonical footprint failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_TILE_FOOTPRINT_MISMATCH")));
    }
    {
        FS1mLineageEvidence Evidence = ValidEvidence();
        Evidence.Xml.Artifact.bExactSizeProven = false;
        FS1mLineageReport Report;
        TestFalse(TEXT("inexact sidecar size rejected"), VerifyS1mLineageEvidence(
            Evidence, ValidExpectation(), FS1mLineageLimits(), Report));
        TestEqual(TEXT("unproven size failure code"), Report.FailureCode,
            FString(TEXT("S1M_LINEAGE_SIZE_UNPROVEN")));
    }
    return true;
}
#endif
