#include "SkiPreparation/S1mLineage.h"

#include <cmath>

namespace
{
using SkiPreparation::FS1mLineageBounds;
using SkiPreparation::FS1mLineageEvidence;
using SkiPreparation::FS1mLineageExpectation;
using SkiPreparation::FS1mLineageLimits;
using SkiPreparation::FS1mLineageReport;
using SkiPreparation::FS1mSourceInputEvidence;

constexpr double S1mTileSideMeters = 10000.0;
constexpr double CoordinateGridToleranceMeters = 0.01;
constexpr int32 MaximumSourceRows = 10000;
constexpr int32 MaximumBlendRows = 10000;
constexpr int32 MaximumFieldStringLength = 512;
constexpr uint64 MaximumCogObjectBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64 MaximumGeoPackageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64 MaximumXmlBytes = 8ULL * 1024ULL * 1024ULL;

FString NormalizeToken(const FString& Value)
{
    FString Result = Value;
    Result.TrimStartAndEndInline();
    Result.ToUpperInline();
    FString Compact;
    Compact.Reserve(Result.Len());
    for (const TCHAR Character : Result)
        if (FChar::IsAlnum(Character)) Compact.AppendChar(Character);
    return Compact;
}

FString NormalizeCrs(const FString& Value)
{
    FString Result = Value;
    Result.TrimStartAndEndInline();
    Result.ToUpperInline();
    Result.ReplaceInline(TEXT(" "), TEXT(""));
    return Result;
}

bool IsExplicitAuthorityCrs(const FString& Value)
{
    FString Normalized = NormalizeCrs(Value);
    int32 Separator = INDEX_NONE;
    if (!Normalized.FindChar(TEXT(':'), Separator) || Separator <= 0
        || Separator == Normalized.Len() - 1 || Normalized.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Separator + 1) != INDEX_NONE)
        return false;

    for (int32 Index = 0; Index < Separator; ++Index)
        if (!(FChar::IsAlpha(Normalized[Index]) || Normalized[Index] == TEXT('_'))) return false;
    for (int32 Index = Separator + 1; Index < Normalized.Len(); ++Index)
        if (!FChar::IsDigit(Normalized[Index])) return false;

    uint32 Code = 0;
    return LexTryParseString(Code, *Normalized.Mid(Separator + 1)) && Code > 0;
}

bool IsS1mProduct(const FString& Product)
{
    return Product.Equals(TEXT("S1M"), ESearchCase::CaseSensitive);
}

bool IsSafeTileId(const FString& Value, const int32 MaximumLength)
{
    if (Value.IsEmpty() || Value.Len() > MaximumLength) return false;
    for (const TCHAR Character : Value)
        if (!(FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-'))) return false;
    return true;
}

bool IsIsoDate(const FString& Value)
{
    if (Value.Len() != 10 || Value[4] != TEXT('-') || Value[7] != TEXT('-')) return false;
    for (int32 Index = 0; Index < Value.Len(); ++Index)
        if (Index != 4 && Index != 7 && !FChar::IsDigit(Value[Index])) return false;

    const int32 Year = FCString::Atoi(*Value.Left(4));
    const int32 Month = FCString::Atoi(*Value.Mid(5, 2));
    const int32 Day = FCString::Atoi(*Value.Right(2));
    if (Year < 1 || Month < 1 || Month > 12 || Day < 1) return false;
    constexpr int32 DaysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool bLeapYear = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    return Day <= DaysInMonth[Month] + (Month == 2 && bLeapYear ? 1 : 0);
}

bool IsNavd88(const FString& Value)
{
    const FString Token = NormalizeToken(Value);
    return Token == TEXT("NAVD88")
        || Token == TEXT("NORTHAMERICANVERTICALDATUMOF1988")
        || Token == TEXT("NORTHAMERICANDATUMOF1988NAVD88")
        || Token == TEXT("NORTHAMERICANDATUM1988NAVD88");
}

bool IsValidBounds(const FS1mLineageBounds& Bounds)
{
    return FMath::IsFinite(Bounds.MinX) && FMath::IsFinite(Bounds.MinY)
        && FMath::IsFinite(Bounds.MaxX) && FMath::IsFinite(Bounds.MaxY)
        && Bounds.MinX < Bounds.MaxX && Bounds.MinY < Bounds.MaxY;
}

bool BoundsEqual(const FS1mLineageBounds& A, const FS1mLineageBounds& B, const double Tolerance)
{
    return FMath::Abs(A.MinX - B.MinX) <= Tolerance
        && FMath::Abs(A.MinY - B.MinY) <= Tolerance
        && FMath::Abs(A.MaxX - B.MaxX) <= Tolerance
        && FMath::Abs(A.MaxY - B.MaxY) <= Tolerance;
}

bool IsInside(const FS1mLineageBounds& Bounds, const FS1mLineageBounds& Tile, const double Tolerance)
{
    return Bounds.MinX >= Tile.MinX - Tolerance && Bounds.MinY >= Tile.MinY - Tolerance
        && Bounds.MaxX <= Tile.MaxX + Tolerance && Bounds.MaxY <= Tile.MaxY + Tolerance;
}

bool IsCanonicalS1mFootprint(const FS1mLineageBounds& Bounds, const double Tolerance)
{
    if (!IsValidBounds(Bounds)
        || FMath::Abs((Bounds.MaxX - Bounds.MinX) - S1mTileSideMeters) > Tolerance
        || FMath::Abs((Bounds.MaxY - Bounds.MinY) - S1mTileSideMeters) > Tolerance)
        return false;

    const double GridX = std::round(Bounds.MinX / S1mTileSideMeters) * S1mTileSideMeters;
    const double GridY = std::round(Bounds.MinY / S1mTileSideMeters) * S1mTileSideMeters;
    return FMath::Abs(Bounds.MinX - GridX) <= Tolerance
        && FMath::Abs(Bounds.MinY - GridY) <= Tolerance;
}

bool IsShortText(const FString& Value, const FS1mLineageLimits& Limits)
{
    return !Value.IsEmpty() && Value.Len() <= Limits.MaxStringLength;
}

bool IsMetadataEquivalent(const FS1mSourceInputEvidence& A, const FS1mSourceInputEvidence& B)
{
    return A.ProjectName == B.ProjectName
        && NormalizeCrs(A.HorizontalCrs) == NormalizeCrs(B.HorizontalCrs)
        && NormalizeToken(A.VerticalDatum) == NormalizeToken(B.VerticalDatum)
        && A.CollectionStartDate == B.CollectionStartDate
        && A.CollectionEndDate == B.CollectionEndDate
        && A.QualityLevel == B.QualityLevel
        && FMath::IsNearlyEqual(A.ResolutionMeters, B.ResolutionMeters, 1.e-9);
}

bool ValidateArtifact(const SkiPreparation::FS1mLineageArtifactEvidence& Artifact,
    const FS1mLineageExpectation& Expected, const uint64 ExpectedBytes,
    const uint64 MaximumBytes, const FS1mLineageLimits& Limits, const TCHAR* Label,
    FString& OutCode, FString& OutDetail)
{
    if (!IsS1mProduct(Artifact.Product) || !Artifact.Product.Equals(Expected.Product, ESearchCase::CaseSensitive)
        || Artifact.TileId != Expected.TileId || Artifact.PublicationDate != Expected.PublicationDate)
    {
        OutCode = TEXT("S1M_LINEAGE_IDENTITY_MISMATCH");
        OutDetail = FString::Printf(TEXT("%s product, tile identity, or publication date differs from the expected S1M object."), Label);
        return false;
    }
    if (!Artifact.bExactSizeProven || Artifact.ObjectBytes == 0 || Artifact.ObjectBytes > MaximumBytes
        || (ExpectedBytes > 0 && Artifact.ObjectBytes != ExpectedBytes))
    {
        OutCode = TEXT("S1M_LINEAGE_SIZE_UNPROVEN");
        OutDetail = FString::Printf(TEXT("%s exact object size is missing, over the cap, or differs from the pinned byte count."), Label);
        return false;
    }
    if (!IsSafeTileId(Artifact.TileId, Limits.MaxStringLength) || !IsIsoDate(Artifact.PublicationDate))
    {
        OutCode = TEXT("S1M_LINEAGE_IDENTITY_INVALID");
        OutDetail = FString::Printf(TEXT("%s tile identity or normalized publication date is malformed."), Label);
        return false;
    }
    return true;
}

bool ValidateSourceInputs(const FS1mLineageEvidence& Evidence, const FS1mLineageLimits& Limits,
    FString& OutCode, FString& OutDetail)
{
    const SkiPreparation::FS1mGeoPackageEvidence& Gpkg = Evidence.GeoPackage;
    if (Gpkg.SourceInputs.IsEmpty() || Gpkg.SourceInputs.Num() > Limits.MaxSourceInputRows)
    {
        OutCode = TEXT("S1M_LINEAGE_SOURCE_ROWS_INVALID");
        OutDetail = TEXT("GeoPackage source_inputs rows are empty or exceed the configured bound.");
        return false;
    }

    TMap<FString, int32> FeatureIds;
    TMap<FString, int32> SourceIds;
    for (int32 Index = 0; Index < Gpkg.SourceInputs.Num(); ++Index)
    {
        const FS1mSourceInputEvidence& Source = Gpkg.SourceInputs[Index];
        const bool bTextValid = IsShortText(Source.FeatureId, Limits)
            && IsShortText(Source.SourceId, Limits) && IsShortText(Source.ProjectName, Limits)
            && IsShortText(Source.VerticalDatum, Limits);
        const bool bCrsValid = Source.bHorizontalCrsExplicit && IsShortText(Source.HorizontalCrs, Limits)
            && IsExplicitAuthorityCrs(Source.HorizontalCrs);
        const bool bDatesValid = Source.bCollectionDatesExplicit
            && IsIsoDate(Source.CollectionStartDate) && IsIsoDate(Source.CollectionEndDate)
            && Source.CollectionStartDate <= Source.CollectionEndDate;
        const bool bQualityValid = Source.bQualityLevelExplicit
            && (Source.QualityLevel == 0 || Source.QualityLevel == 1 || Source.QualityLevel == 2
                || Source.QualityLevel == 3 || Source.QualityLevel == 5);
        const bool bResolutionValid = Source.bResolutionExplicit && FMath::IsFinite(Source.ResolutionMeters)
            && Source.ResolutionMeters > 0.0;
        const bool bGeometryValid = Source.bGeometryValidated && IsValidBounds(Source.Footprint)
            && IsInside(Source.Footprint, Evidence.Cog.TileFootprint, Limits.FootprintToleranceMeters);
        if (!bTextValid || !Source.bVerticalDatumExplicit || !bCrsValid || !bDatesValid
            || !bQualityValid || !bResolutionValid || !bGeometryValid)
        {
            OutCode = TEXT("S1M_LINEAGE_SOURCE_RECORD_INVALID");
            OutDetail = FString::Printf(TEXT("source_inputs row %d lacks explicit bounded identity, CRS, datum, acquisition, quality, resolution, or in-tile geometry facts."), Index);
            return false;
        }

        if (FeatureIds.Contains(Source.FeatureId))
        {
            OutCode = TEXT("S1M_LINEAGE_SOURCE_RECORD_DUPLICATE");
            OutDetail = FString::Printf(TEXT("source_inputs repeats feature identity '%s'."), *Source.FeatureId);
            return false;
        }
        FeatureIds.Add(Source.FeatureId, Index);

        if (const int32* PreviousIndex = SourceIds.Find(Source.SourceId))
        {
            if (!IsMetadataEquivalent(Gpkg.SourceInputs[*PreviousIndex], Source))
            {
                OutCode = TEXT("S1M_LINEAGE_SOURCE_CONTRADICTORY");
                OutDetail = FString::Printf(TEXT("source_inputs rows for source '%s' disagree on source metadata."), *Source.SourceId);
                return false;
            }
        }
        else
        {
            SourceIds.Add(Source.SourceId, Index);
        }
    }
    return true;
}

bool ValidateBlendAreas(const FS1mLineageEvidence& Evidence, const FS1mLineageLimits& Limits,
    FString& OutCode, FString& OutDetail)
{
    const TArray<SkiPreparation::FS1mLineageAreaEvidence>& Areas = Evidence.GeoPackage.BlendAreas;
    if (Areas.Num() > Limits.MaxBlendAreaRows)
    {
        OutCode = TEXT("S1M_LINEAGE_BLEND_ROWS_INVALID");
        OutDetail = TEXT("GeoPackage blend/backfill rows exceed the configured bound.");
        return false;
    }

    TSet<FString> FeatureIds;
    for (int32 Index = 0; Index < Areas.Num(); ++Index)
    {
        const SkiPreparation::FS1mLineageAreaEvidence& Area = Areas[Index];
        const bool bKindValid = Area.Kind == SkiPreparation::ES1mLineageAreaKind::Blend
            || Area.Kind == SkiPreparation::ES1mLineageAreaKind::Backfill
            || Area.Kind == SkiPreparation::ES1mLineageAreaKind::Interpolated;
        if (!IsShortText(Area.FeatureId, Limits) || !Area.bKindExplicit || !bKindValid
            || !Area.bGeometryValidated || !IsValidBounds(Area.Footprint)
            || !IsInside(Area.Footprint, Evidence.Cog.TileFootprint, Limits.FootprintToleranceMeters))
        {
            OutCode = TEXT("S1M_LINEAGE_BLEND_RECORD_INVALID");
            OutDetail = FString::Printf(TEXT("blend-area row %d lacks an explicit recognized class or valid in-tile geometry."), Index);
            return false;
        }
        if (FeatureIds.Contains(Area.FeatureId))
        {
            OutCode = TEXT("S1M_LINEAGE_BLEND_RECORD_DUPLICATE");
            OutDetail = FString::Printf(TEXT("blend-area table repeats feature identity '%s'."), *Area.FeatureId);
            return false;
        }
        FeatureIds.Add(Area.FeatureId);
    }
    return true;
}
}

namespace SkiPreparation
{
bool VerifyS1mLineageEvidence(const FS1mLineageEvidence& Evidence,
    const FS1mLineageExpectation& Expected, const FS1mLineageLimits& Limits,
    FS1mLineageReport& OutReport)
{
    OutReport = FS1mLineageReport();
    const auto Reject = [&OutReport](const FString& Code, const FString& Detail)
    {
        OutReport.FailureCode = Code;
        OutReport.FailureDetail = Detail;
        return false;
    };

    if (!Expected.Product.Equals(TEXT("S1M"), ESearchCase::CaseSensitive)
        || !IsSafeTileId(Expected.TileId, FMath::Clamp(Limits.MaxStringLength, 1, MaximumFieldStringLength))
        || !IsIsoDate(Expected.PublicationDate))
        return Reject(TEXT("S1M_LINEAGE_EXPECTATION_INVALID"),
            TEXT("Expected product, tile identity, or normalized publication date is invalid."));

    if (Limits.MaxCogObjectBytes == 0 || Limits.MaxCogObjectBytes > MaximumCogObjectBytes
        || Limits.MaxGeoPackageBytes == 0 || Limits.MaxGeoPackageBytes > MaximumGeoPackageBytes
        || Limits.MaxXmlBytes == 0 || Limits.MaxXmlBytes > MaximumXmlBytes
        || Limits.MaxSourceInputRows <= 0 || Limits.MaxSourceInputRows > MaximumSourceRows
        || Limits.MaxBlendAreaRows < 0 || Limits.MaxBlendAreaRows > MaximumBlendRows
        || Limits.MaxStringLength <= 0 || Limits.MaxStringLength > MaximumFieldStringLength
        || !FMath::IsFinite(Limits.FootprintToleranceMeters)
        || Limits.FootprintToleranceMeters < 0.0 || Limits.FootprintToleranceMeters > CoordinateGridToleranceMeters)
        return Reject(TEXT("S1M_LINEAGE_LIMITS_INVALID"), TEXT("Lineage limits exceed fixed parser bounds."));

    if (NormalizeCrs(Expected.HorizontalCrs) != TEXT("EPSG:6350") || !IsNavd88(Expected.VerticalDatum))
        return Reject(TEXT("S1M_LINEAGE_EXPECTATION_INVALID"),
            TEXT("The S1M output CRS and vertical datum expectation must be explicitly EPSG:6350 and NAVD88."));

    FString FailureCode;
    FString FailureDetail;
    if (!ValidateArtifact(Evidence.Cog.Artifact, Expected, Expected.CogObjectBytes,
            Limits.MaxCogObjectBytes, Limits, TEXT("COG"), FailureCode, FailureDetail)
        || !ValidateArtifact(Evidence.GeoPackage.Artifact, Expected, Expected.GeoPackageObjectBytes,
            Limits.MaxGeoPackageBytes, Limits, TEXT("GeoPackage"), FailureCode, FailureDetail)
        || !ValidateArtifact(Evidence.Xml.Artifact, Expected, Expected.XmlObjectBytes,
            Limits.MaxXmlBytes, Limits, TEXT("XML sidecar"), FailureCode, FailureDetail))
        return Reject(FailureCode, FailureDetail);

    if (!Evidence.Cog.bHeaderValidated || !Evidence.Cog.bHorizontalCrsExplicit
        || NormalizeCrs(Evidence.Cog.HorizontalCrs) != TEXT("EPSG:6350")
        || !Evidence.Xml.bWellFormed || !Evidence.Xml.bHorizontalCrsExplicit
        || NormalizeCrs(Evidence.Xml.HorizontalCrs) != TEXT("EPSG:6350"))
        return Reject(TEXT("S1M_LINEAGE_HORIZONTAL_CRS_UNPROVEN"),
            TEXT("COG and XML must explicitly agree on the S1M EPSG:6350 horizontal CRS."));

    if (!Evidence.Cog.bVerticalDatumExplicit || !Evidence.Xml.bVerticalDatumExplicit
        || !IsNavd88(Evidence.Cog.VerticalDatum) || !IsNavd88(Evidence.Xml.VerticalDatum)
        || !Evidence.Xml.bVerticalRmseExplicit || !FMath::IsFinite(Evidence.Xml.VerticalRmseMeters)
        || Evidence.Xml.VerticalRmseMeters < 0.0)
        return Reject(TEXT("S1M_LINEAGE_DATUM_UNPROVEN"),
            TEXT("COG and XML must explicitly prove NAVD88, and XML must carry a finite nonnegative vertical RMSE."));

    const FS1mLineageBounds& Footprint = Evidence.Cog.TileFootprint;
    if (!IsCanonicalS1mFootprint(Footprint, Limits.FootprintToleranceMeters)
        || !Evidence.GeoPackage.bTileFootprintExplicit || !Evidence.Xml.bTileFootprintExplicit
        || !IsValidBounds(Evidence.GeoPackage.TileFootprint) || !IsValidBounds(Evidence.Xml.TileFootprint)
        || !BoundsEqual(Footprint, Evidence.GeoPackage.TileFootprint, Limits.FootprintToleranceMeters)
        || !BoundsEqual(Footprint, Evidence.Xml.TileFootprint, Limits.FootprintToleranceMeters))
        return Reject(TEXT("S1M_LINEAGE_TILE_FOOTPRINT_MISMATCH"),
            TEXT("COG, GeoPackage, and XML must explicitly identify the same aligned 10 km S1M tile footprint."));

    const FS1mGeoPackageEvidence& Gpkg = Evidence.GeoPackage;
    if (!Gpkg.bRequiredTablesPresent || !Gpkg.bFeatureMetadataValidated
        || Gpkg.SourceInputsContentsSrsId != 6350 || Gpkg.SourceInputsGeometrySrsId != 6350
        || Gpkg.BlendContentsSrsId != 6350 || Gpkg.BlendGeometrySrsId != 6350)
        return Reject(TEXT("S1M_LINEAGE_GPKG_METADATA_INVALID"),
            TEXT("GeoPackage must expose both required feature tables with EPSG:6350 content and geometry metadata."));

    if (!ValidateSourceInputs(Evidence, Limits, FailureCode, FailureDetail))
        return Reject(FailureCode, FailureDetail);
    if (!ValidateBlendAreas(Evidence, Limits, FailureCode, FailureDetail))
        return Reject(FailureCode, FailureDetail);

    OutReport.bPassed = true;
    OutReport.FailureCode = TEXT("S1M_LINEAGE_VERIFIED");
    OutReport.FailureDetail = TEXT("Normalized S1M identity, footprint, CRS, datum, lineage, and exact-size facts agree.");
    OutReport.Product = Expected.Product;
    OutReport.TileId = Expected.TileId;
    OutReport.PublicationDate = Expected.PublicationDate;
    OutReport.HorizontalCrs = Expected.HorizontalCrs;
    OutReport.VerticalDatum = Expected.VerticalDatum;
    OutReport.VerticalRmseMeters = Evidence.Xml.VerticalRmseMeters;
    OutReport.CogObjectBytes = Evidence.Cog.Artifact.ObjectBytes;
    OutReport.GeoPackageObjectBytes = Evidence.GeoPackage.Artifact.ObjectBytes;
    OutReport.XmlObjectBytes = Evidence.Xml.Artifact.ObjectBytes;
    OutReport.SourceInputRows = Gpkg.SourceInputs.Num();
    OutReport.BlendAreaRows = Gpkg.BlendAreas.Num();
    return true;
}
}
