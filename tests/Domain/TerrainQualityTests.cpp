#include "SkiDomain/TerrainQuality.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
SkiDomain::TerrainQualitySourceFacts KnownS1mSource()
{
    SkiDomain::TerrainQualitySourceFacts Source;
    Source.SourceId = "s1m-tile-01";
    Source.Product = SkiDomain::ElevationProduct::S1M;
    Source.QualityLevel = 2;
    Source.QualityLevelUnknown = false;
    Source.AcquisitionStartDate = "2020-01-01";
    Source.AcquisitionEndDate = "2020-12-31";
    Source.AcquisitionDateRangeUnknown = false;
    Source.VerticalRmseMeters = 0.45;
    Source.VerticalRmseUnknown = false;
    Source.VerticalDatum = SkiDomain::TerrainVerticalDatum::NAVD88;
    Source.DatumProofOrigin = SkiDomain::TerrainDatumProofOrigin::VerifiedS1mLineage;
    Source.DatumUnknown = false;
    Source.DatumProven = true;
    Source.SampleFraction = 1.0;
    Source.SampleFractionUnknown = false;
    return Source;
}
}

int main()
{
    int Total = 0;
    int Failed = 0;
    const auto Check = [&](const bool Condition, const char* Name)
    {
        ++Total;
        if (!Condition) { ++Failed; std::cerr << "FAIL " << Name << '\n'; }
    };
    SkiDomain::TerrainQualityReport Report;
    SkiDomain::TerrainProvenanceCounts Counts;
    Counts.S1MNativeQualified = 98;
    Counts.S1MBlend = 2;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Report)
        && Report.Grade == SkiDomain::TerrainGrade::A
        && std::abs(Report.QualifiedLidarFraction - 0.98) < 1.0e-12
        && std::abs(Report.SourceMix.S1M - 1.0) < 1.0e-12
        && !Report.SourceMix.Unknown && !Report.SourceMix.Estimated,
        "A boundary accepts 98 percent qualified lidar and 2 percent blend");
    Counts = {};
    Counts.Project1mQualified = 90;
    Counts.S1MBackfill = 10;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Report)
        && Report.Grade == SkiDomain::TerrainGrade::B,
        "B accepts 90 percent qualified project lidar with backfill");
    Counts = {};
    Counts.S1MNativeQualified = 90;
    Counts.NoData = 10;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Report)
        && Report.Grade == SkiDomain::TerrainGrade::C && Report.HasNoDataWarning,
        "nodata caps an otherwise B quality mix at C");
    Counts = {};
    Counts.S1MNativeQualified = 49;
    Counts.ArcSec13 = 51;
    Counts.UnknownMetadata = 51;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Report)
        && Report.Grade == SkiDomain::TerrainGrade::D
        && std::abs(Report.CoarseFraction - 0.51) < 1.0e-12
        && std::abs(Report.UnknownMetadataFraction - 0.51) < 1.0e-12,
        "coarse fallback and missing metadata remain visible in D");
    Counts = {};
    Check(!SkiDomain::TrySummarizeTerrainQuality(Counts, Report),
        "empty quality tally is rejected");
    Counts.S1MNativeQualified = std::numeric_limits<std::uint64_t>::max();
    Counts.NoData = 1;
    Check(!SkiDomain::TrySummarizeTerrainQuality(Counts, Report),
        "overflowing quality tally is rejected");

    Counts = {};
    Counts.S1MNativeQualified = 98;
    Counts.S1MBlend = 2;
    std::vector<SkiDomain::TerrainQualitySourceFacts> Sources{KnownS1mSource()};
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Sources, Report)
        && Report.Grade == SkiDomain::TerrainGrade::A
        && Report.Sources.size() == 1
        && Report.Sources[0].QualityLevel == 2
        && Report.Sources[0].DatumProven
        && std::abs(Report.Sources[0].VerticalRmseMeters - 0.45) < 1.0e-12,
        "verified per-source QL, acquisition range, RMSE, datum origin, and sample fraction are retained");

    Sources[0] = {};
    Sources[0].SourceId = "project-source-unknown";
    Sources[0].Product = SkiDomain::ElevationProduct::Project1m;
    Sources[0].SampleFraction = 1.0;
    Sources[0].SampleFractionUnknown = false;
    Counts = {};
    Counts.Project1mOther = 100;
    Counts.UnknownMetadata = 100;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Sources, Report)
        && Report.Grade == SkiDomain::TerrainGrade::D
        && Report.Sources[0].QualityLevelUnknown
        && Report.Sources[0].AcquisitionDateRangeUnknown
        && Report.Sources[0].VerticalRmseUnknown
        && Report.Sources[0].DatumUnknown
        && std::abs(Report.SourceMix.Project1m - 1.0) < 1.0e-12,
        "unknown source metadata remains explicit and cannot raise the count-derived grade");

    Sources[0] = KnownS1mSource();
    Sources[0].QualityLevelEstimated = true;
    Sources[0].AcquisitionDateRangeEstimated = true;
    Sources[0].VerticalRmseEstimated = true;
    Sources[0].DatumEstimated = true;
    Sources[0].DatumProven = false;
    Counts = {};
    Counts.S1MNativeOther = 100;
    Check(SkiDomain::TrySummarizeTerrainQuality(Counts, Sources, Report)
        && Report.Grade == SkiDomain::TerrainGrade::D
        && Report.Sources[0].QualityLevelEstimated
        && Report.Sources[0].AcquisitionDateRangeEstimated
        && Report.Sources[0].VerticalRmseEstimated
        && Report.Sources[0].DatumEstimated,
        "estimated lineage remains marked and does not change the provenance grade");

    Sources[0].AcquisitionEndDate = "2019-12-31";
    Check(!SkiDomain::TryValidateTerrainQualitySourceFacts(Sources),
        "reversed acquisition date ranges are rejected");
    Sources[0] = KnownS1mSource();
    Sources[0].VerticalRmseMeters = std::numeric_limits<double>::quiet_NaN();
    Check(!SkiDomain::TryValidateTerrainQualitySourceFacts(Sources),
        "non-finite source RMSE is rejected");
    Sources[0] = KnownS1mSource();
    Sources[0].DatumProven = false;
    Check(!SkiDomain::TryValidateTerrainQualitySourceFacts(Sources),
        "unestimated datum facts require proof from an explicit origin");
    Sources[0] = KnownS1mSource();
    Sources[0].SampleFraction = 0.99;
    Counts = {};
    Counts.S1MNativeQualified = 98;
    Counts.S1MBlend = 2;
    Check(!SkiDomain::TrySummarizeTerrainQuality(Counts, Sources, Report),
        "source fractions that disagree with the canonical provenance mix are rejected");
    Check(!SkiDomain::TryValidateTerrainQualitySourceFacts({}),
        "an empty per-source lineage set is rejected");

    std::cout << "SKI_TEST_RESULT {\"total\":" << Total << ",\"passed\":"
        << Total - Failed << ",\"failed\":" << Failed << "}\n";
    return Failed ? 1 : 0;
}
