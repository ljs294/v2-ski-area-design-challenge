#include "SkiDomain/TerrainQuality.h"

#include <cmath>
#include <limits>
#include <utility>

namespace
{
constexpr std::size_t MaximumQualitySourceFacts = 2048;
constexpr std::size_t MaximumSourceIdLength = 512;
constexpr double FractionTolerance = 1.0e-12;

bool AtLeastPercent(const std::uint64_t Part, const std::uint64_t Whole,
    const std::uint64_t Percent) noexcept
{
    const std::uint64_t Minimum = (Whole / 100U) * Percent
        + ((Whole % 100U) * Percent + 99U) / 100U;
    return Part >= Minimum;
}

bool AtMostPercent(const std::uint64_t Part, const std::uint64_t Whole,
    const std::uint64_t Percent) noexcept
{
    const std::uint64_t Maximum = (Whole / 100U) * Percent
        + ((Whole % 100U) * Percent) / 100U;
    return Part <= Maximum;
}

bool IsAsciiDigit(const char Character) noexcept
{
    return Character >= '0' && Character <= '9';
}

bool IsIsoDate(const std::string& Date) noexcept
{
    if (Date.size() != 10 || Date[4] != '-' || Date[7] != '-') return false;
    for (std::size_t Index = 0; Index < Date.size(); ++Index)
        if (Index != 4 && Index != 7 && !IsAsciiDigit(Date[Index])) return false;

    const int Year = (Date[0] - '0') * 1000 + (Date[1] - '0') * 100
        + (Date[2] - '0') * 10 + Date[3] - '0';
    const int Month = (Date[5] - '0') * 10 + Date[6] - '0';
    const int Day = (Date[8] - '0') * 10 + Date[9] - '0';
    if (Year < 1 || Month < 1 || Month > 12 || Day < 1) return false;

    constexpr int DaysInMonth[]{0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool IsLeapYear = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    return Day <= DaysInMonth[Month] + (Month == 2 && IsLeapYear ? 1 : 0);
}

bool IsValidSourceId(const std::string& SourceId) noexcept
{
    if (SourceId.empty() || SourceId.size() > MaximumSourceIdLength
        || SourceId.front() == ' ' || SourceId.back() == ' ')
        return false;
    for (const unsigned char Character : SourceId)
        if (Character < 0x20U || Character == 0x7fU) return false;
    return true;
}

bool IsValidProduct(const SkiDomain::ElevationProduct Product) noexcept
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M:
    case SkiDomain::ElevationProduct::Project1m:
    case SkiDomain::ElevationProduct::ArcSec13:
        return true;
    }
    return false;
}

bool IsValidDatum(const SkiDomain::TerrainVerticalDatum Datum) noexcept
{
    return Datum == SkiDomain::TerrainVerticalDatum::Unknown
        || Datum == SkiDomain::TerrainVerticalDatum::NAVD88
        || Datum == SkiDomain::TerrainVerticalDatum::Other;
}

bool IsValidDatumProofOrigin(const SkiDomain::TerrainDatumProofOrigin Origin) noexcept
{
    using SkiDomain::TerrainDatumProofOrigin;
    return Origin == TerrainDatumProofOrigin::Unknown
        || Origin == TerrainDatumProofOrigin::CogMetadata
        || Origin == TerrainDatumProofOrigin::GeoPackageSourceInputs
        || Origin == TerrainDatumProofOrigin::XmlSidecar
        || Origin == TerrainDatumProofOrigin::CatalogMetadata
        || Origin == TerrainDatumProofOrigin::VerifiedS1mLineage;
}

bool NearlyEqual(const double A, const double B) noexcept
{
    return std::abs(A - B) <= FractionTolerance;
}
}

bool SkiDomain::TrySummarizeTerrainQuality(const TerrainProvenanceCounts& Counts,
    TerrainQualityReport& OutReport) noexcept
{
    OutReport = {};
    std::uint64_t Total = 0;
    const std::uint64_t Parts[]{Counts.S1MNativeQualified, Counts.S1MNativeOther,
        Counts.S1MBlend, Counts.S1MBackfill, Counts.S1MInterpolated,
        Counts.Project1mQualified, Counts.Project1mOther, Counts.ArcSec13,
        Counts.NoData};
    for (const std::uint64_t Part : Parts)
    {
        if (Part > std::numeric_limits<std::uint64_t>::max() - Total) return false;
        Total += Part;
    }
    if (Total == 0 || Counts.UnknownMetadata > Total) return false;
    const std::uint64_t Qualified = Counts.S1MNativeQualified + Counts.Project1mQualified;
    const std::uint64_t Blend = Counts.S1MBlend + Counts.S1MInterpolated;
    const double Denominator = static_cast<double>(Total);
    OutReport.TotalSamples = Total;
    OutReport.QualifiedLidarFraction = static_cast<double>(Qualified) / Denominator;
    OutReport.BlendInterpolatedFraction = static_cast<double>(Blend) / Denominator;
    OutReport.BackfillFraction = static_cast<double>(Counts.S1MBackfill) / Denominator;
    OutReport.CoarseFraction = static_cast<double>(Counts.ArcSec13) / Denominator;
    OutReport.NoDataFraction = static_cast<double>(Counts.NoData) / Denominator;
    OutReport.UnknownMetadataFraction = static_cast<double>(Counts.UnknownMetadata) / Denominator;
    OutReport.HasNoDataWarning = Counts.NoData != 0;
    OutReport.SourceMix.S1M = static_cast<double>(Counts.S1MNativeQualified
        + Counts.S1MNativeOther + Counts.S1MBlend + Counts.S1MBackfill
        + Counts.S1MInterpolated) / Denominator;
    OutReport.SourceMix.Project1m = static_cast<double>(Counts.Project1mQualified
        + Counts.Project1mOther) / Denominator;
    OutReport.SourceMix.ArcSec13 = static_cast<double>(Counts.ArcSec13) / Denominator;
    OutReport.SourceMix.Unknown = false;
    OutReport.SourceMix.Estimated = false;

    if (AtLeastPercent(Qualified, Total, 98)
        && AtMostPercent(Blend, Total, 2)
        && Counts.S1MBackfill == 0 && Counts.ArcSec13 == 0 && Counts.NoData == 0)
        OutReport.Grade = TerrainGrade::A;
    else if (AtLeastPercent(Qualified, Total, 90) && Counts.NoData == 0)
        OutReport.Grade = TerrainGrade::B;
    else if (AtLeastPercent(Qualified, Total, 50))
        OutReport.Grade = TerrainGrade::C;
    else OutReport.Grade = TerrainGrade::D;
    return true;
}

bool SkiDomain::TryValidateTerrainQualitySourceFacts(
    const std::vector<TerrainQualitySourceFacts>& Sources) noexcept
{
    if (Sources.empty() || Sources.size() > MaximumQualitySourceFacts) return false;

    for (std::size_t Index = 0; Index < Sources.size(); ++Index)
    {
        const TerrainQualitySourceFacts& Source = Sources[Index];
        if (!IsValidSourceId(Source.SourceId) || !IsValidProduct(Source.Product)) return false;

        if (Source.QualityLevelUnknown)
        {
            if (Source.QualityLevel != 0 || Source.QualityLevelEstimated) return false;
        }
        else if (Source.QualityLevel == 0 || Source.QualityLevel > 5)
        {
            return false;
        }

        if (Source.AcquisitionDateRangeUnknown)
        {
            if (!Source.AcquisitionStartDate.empty() || !Source.AcquisitionEndDate.empty()
                || Source.AcquisitionDateRangeEstimated)
                return false;
        }
        else if (!IsIsoDate(Source.AcquisitionStartDate)
            || !IsIsoDate(Source.AcquisitionEndDate)
            || Source.AcquisitionStartDate > Source.AcquisitionEndDate)
        {
            return false;
        }

        if (Source.VerticalRmseUnknown)
        {
            if (Source.VerticalRmseMeters != 0.0 || Source.VerticalRmseEstimated) return false;
        }
        else if (!std::isfinite(Source.VerticalRmseMeters) || Source.VerticalRmseMeters < 0.0)
        {
            return false;
        }

        if (!IsValidDatum(Source.VerticalDatum) || !IsValidDatumProofOrigin(Source.DatumProofOrigin))
            return false;
        if (Source.DatumUnknown)
        {
            if (Source.VerticalDatum != TerrainVerticalDatum::Unknown
                || Source.DatumProofOrigin != TerrainDatumProofOrigin::Unknown
                || Source.DatumEstimated || Source.DatumProven)
                return false;
        }
        else if (Source.VerticalDatum == TerrainVerticalDatum::Unknown
            || Source.DatumProofOrigin == TerrainDatumProofOrigin::Unknown
            || Source.DatumEstimated == Source.DatumProven)
        {
            return false;
        }

        if (Source.SampleFractionUnknown)
        {
            if (Source.SampleFraction != 0.0 || Source.SampleFractionEstimated) return false;
        }
        else if (!std::isfinite(Source.SampleFraction)
            || Source.SampleFraction < 0.0 || Source.SampleFraction > 1.0)
        {
            return false;
        }

        for (std::size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
        {
            const TerrainQualitySourceFacts& Other = Sources[OtherIndex];
            if (Source.Product == Other.Product && Source.SourceId == Other.SourceId) return false;
        }
    }
    return true;
}

bool SkiDomain::TrySummarizeTerrainQuality(const TerrainProvenanceCounts& Counts,
    const std::vector<TerrainQualitySourceFacts>& Sources,
    TerrainQualityReport& OutReport)
{
    TerrainQualityReport Summary;
    if (!TrySummarizeTerrainQuality(Counts, Summary)
        || !TryValidateTerrainQualitySourceFacts(Sources))
    {
        OutReport = {};
        return false;
    }

    bool AllSourceFractionsExact = true;
    double S1MFraction = 0.0;
    double Project1mFraction = 0.0;
    double ArcSec13Fraction = 0.0;
    for (const TerrainQualitySourceFacts& Source : Sources)
    {
        if (Source.SampleFractionUnknown || Source.SampleFractionEstimated)
        {
            AllSourceFractionsExact = false;
            continue;
        }
        switch (Source.Product)
        {
        case ElevationProduct::S1M: S1MFraction += Source.SampleFraction; break;
        case ElevationProduct::Project1m: Project1mFraction += Source.SampleFraction; break;
        case ElevationProduct::ArcSec13: ArcSec13Fraction += Source.SampleFraction; break;
        }
    }
    if (AllSourceFractionsExact
        && (!NearlyEqual(S1MFraction, Summary.SourceMix.S1M)
            || !NearlyEqual(Project1mFraction, Summary.SourceMix.Project1m)
            || !NearlyEqual(ArcSec13Fraction, Summary.SourceMix.ArcSec13)))
    {
        OutReport = {};
        return false;
    }

    Summary.Sources = Sources;
    OutReport = std::move(Summary);
    return true;
}
