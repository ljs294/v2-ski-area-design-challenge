#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
/** Axis-aligned bounds in the explicitly named horizontal CRS. */
struct SKIPREPARATION_API FS1mLineageBounds
{
    double MinX = 0.0;
    double MinY = 0.0;
    double MaxX = 0.0;
    double MaxY = 0.0;
};

/** Normalized identity and exact byte count for one acquired S1M object. */
struct SKIPREPARATION_API FS1mLineageArtifactEvidence
{
    FString Product;
    FString TileId;
    FString PublicationDate;
    uint64 ObjectBytes = 0;
    bool bExactSizeProven = false;
};

/** One source polygon row after a bounded GeoPackage adapter has decoded it. */
struct SKIPREPARATION_API FS1mSourceInputEvidence
{
    /** Stable, unique feature/record key from the source_inputs table. */
    FString FeatureId;
    /** Stable source project identity; multiple polygons may refer to one project. */
    FString SourceId;
    FString ProjectName;
    FString HorizontalCrs;
    FString VerticalDatum;
    FString CollectionStartDate;
    FString CollectionEndDate;
    int32 QualityLevel = -1;
    double ResolutionMeters = 0.0;
    FS1mLineageBounds Footprint;
    bool bGeometryValidated = false;
    bool bHorizontalCrsExplicit = false;
    bool bVerticalDatumExplicit = false;
    bool bCollectionDatesExplicit = false;
    bool bQualityLevelExplicit = false;
    bool bResolutionExplicit = false;
};

enum class ES1mLineageAreaKind : uint8
{
    Blend,
    Backfill,
    Interpolated,
};

/** One blend/backfill/interpolated polygon row after bounded GPKG decoding. */
struct SKIPREPARATION_API FS1mLineageAreaEvidence
{
    FString FeatureId;
    ES1mLineageAreaKind Kind = ES1mLineageAreaKind::Blend;
    FS1mLineageBounds Footprint;
    bool bGeometryValidated = false;
    bool bKindExplicit = false;
};

/** Facts supplied by the SQLiteCore adapter. No raw GeoPackage is accepted here. */
struct SKIPREPARATION_API FS1mGeoPackageEvidence
{
    FS1mLineageArtifactEvidence Artifact;
    bool bRequiredTablesPresent = false;
    bool bFeatureMetadataValidated = false;
    int32 SourceInputsContentsSrsId = 0;
    int32 SourceInputsGeometrySrsId = 0;
    int32 BlendContentsSrsId = 0;
    int32 BlendGeometrySrsId = 0;
    bool bTileFootprintExplicit = false;
    FS1mLineageBounds TileFootprint;
    TArray<FS1mSourceInputEvidence> SourceInputs;
    TArray<FS1mLineageAreaEvidence> BlendAreas;
};

/** Facts supplied by an XML adapter. Explicit flags must only reflect present source elements. */
struct SKIPREPARATION_API FS1mXmlSidecarEvidence
{
    FS1mLineageArtifactEvidence Artifact;
    bool bWellFormed = false;
    bool bHorizontalCrsExplicit = false;
    FString HorizontalCrs;
    bool bVerticalDatumExplicit = false;
    FString VerticalDatum;
    bool bVerticalRmseExplicit = false;
    double VerticalRmseMeters = 0.0;
    bool bTileFootprintExplicit = false;
    FS1mLineageBounds TileFootprint;
};

/** COG facts are passed from COG preflight; this verifier does not read TIFF bytes. */
struct SKIPREPARATION_API FS1mCogLineageEvidence
{
    FS1mLineageArtifactEvidence Artifact;
    bool bHeaderValidated = false;
    FString HorizontalCrs;
    bool bHorizontalCrsExplicit = false;
    FString VerticalDatum;
    bool bVerticalDatumExplicit = false;
    FS1mLineageBounds TileFootprint;
};

/** Bounded normalized evidence from the three sidecars associated with one S1M tile. */
struct SKIPREPARATION_API FS1mLineageEvidence
{
    FS1mCogLineageEvidence Cog;
    FS1mGeoPackageEvidence GeoPackage;
    FS1mXmlSidecarEvidence Xml;
};

/** Independent identity and optional exact byte counts from the catalog/acquisition plan. */
struct SKIPREPARATION_API FS1mLineageExpectation
{
    FString Product = TEXT("S1M");
    FString TileId;
    FString PublicationDate;
    FString HorizontalCrs = TEXT("EPSG:6350");
    FString VerticalDatum = TEXT("NAVD88");
    uint64 CogObjectBytes = 0;
    uint64 GeoPackageObjectBytes = 0;
    uint64 XmlObjectBytes = 0;
};

/** Hard bounds applied before evidence can be considered complete. */
struct SKIPREPARATION_API FS1mLineageLimits
{
    uint64 MaxCogObjectBytes = 512ULL * 1024ULL * 1024ULL;
    uint64 MaxGeoPackageBytes = 64ULL * 1024ULL * 1024ULL;
    uint64 MaxXmlBytes = 8ULL * 1024ULL * 1024ULL;
    int32 MaxSourceInputRows = 10000;
    int32 MaxBlendAreaRows = 10000;
    int32 MaxStringLength = 512;
    double FootprintToleranceMeters = 0.01;
};

struct SKIPREPARATION_API FS1mLineageReport
{
    bool bPassed = false;
    FString FailureCode = TEXT("S1M_LINEAGE_NOT_VERIFIED");
    FString FailureDetail;
    FString Product;
    FString TileId;
    FString PublicationDate;
    FString HorizontalCrs;
    FString VerticalDatum;
    double VerticalRmseMeters = 0.0;
    uint64 CogObjectBytes = 0;
    uint64 GeoPackageObjectBytes = 0;
    uint64 XmlObjectBytes = 0;
    int32 SourceInputRows = 0;
    int32 BlendAreaRows = 0;
};

/**
 * Validates already parsed sidecar facts. This is intentionally not a raw GPKG/XML parser:
 * the checked-in repository has SQLiteCore but no XML parser dependency, and has no pinned
 * real S1M schema/fixture. Adapter flags therefore must only be set from explicit source data.
 * Missing adapters/evidence fail closed.
 */
SKIPREPARATION_API bool VerifyS1mLineageEvidence(const FS1mLineageEvidence& Evidence,
    const FS1mLineageExpectation& Expected, const FS1mLineageLimits& Limits,
    FS1mLineageReport& OutReport);
}
