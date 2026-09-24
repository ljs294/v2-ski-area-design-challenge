#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/ElevationSources.h"
#include "SkiPreparation/TerrainAcquisition.h"

namespace SkiPreparation
{
/**
 * Hard limits applied while reading an elevation COG header. The reader only fetches
 * metadata and tile indexes; it never downloads or decodes tile payloads.
 */
struct SKIPREPARATION_API FCogPreflightLimits
{
    uint64 MaxObjectBytes = 512ULL * 1024ULL * 1024ULL;
    uint64 MaxTransferredBytes = 32ULL * 1024ULL * 1024ULL;
    uint64 MaxRequests = 512;
    uint64 MaxRangeBytes = 1024ULL * 1024ULL;
    uint64 MaxCachedBytes = 1024ULL * 1024ULL;
    uint32 RangeBlockBytes = 64U * 1024U;
    uint32 MaxDimension = 10020;
    uint32 MaxDirectories = 16;
    uint32 MaxTilesPerDirectory = 4096;
    uint32 MaxTotalTiles = 8192;
    /** Zero accepts any safe TIFF tile size; production defaults enforce the 512² USGS layout. */
    uint32 ExpectedTileWidth = 512;
    uint32 ExpectedTileHeight = 512;
};

/** Catalog facts are cross-checked against GeoTIFF keys, never treated as header proof. */
struct SKIPREPARATION_API FCogPreflightMetadata
{
    FString CatalogHorizontalCrs;
    /** Project 1 m files have no vertical CRS key; NAVD88 must be proven outside the TIFF. */
    FString CatalogVerticalDatum;
};

struct SKIPREPARATION_API FCogDirectoryPreflight
{
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 TileWidth = 0;
    uint32 TileHeight = 0;
    uint32 TileCount = 0;
    uint16 Compression = 0;
    uint16 Predictor = 0;
    bool bOverview = false;
};

struct SKIPREPARATION_API FCogPreflightReport
{
    bool bPassed = false;
    FString FailureCode;
    FString FailureDetail;
    FString HorizontalCrs;
    FString VerticalDatum;
    FString VerticalDatumOrigin;
    FString NoDataText;
    FString SampleFormat;
    FString Compression;
    uint64 ObjectBytes = 0;
    uint64 Requests = 0;
    uint64 TransferredBytes = 0;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 TileWidth = 0;
    uint32 TileHeight = 0;
    uint16 BitsPerSample = 0;
    uint16 SamplesPerPixel = 0;
    uint16 Predictor = 0;
    bool bPixelIsArea = false;
    bool bStrongETagPinned = false;
    TArray<FCogDirectoryPreflight> Directories;
};

/**
 * Validates a classic tiled elevation GeoTIFF/COG using bounded exact HTTP ranges.
 * The injected transport is expected to be SkiNetGateway in production. The preflight
 * independently checks each 206 Content-Range, body length, strong ETag and object size.
 */
SKIPREPARATION_API bool PreflightElevationCog(IAcquisitionTransport& Transport,
    const FString& Url, SkiDomain::ElevationProduct Product, const FCogPreflightMetadata& Metadata,
    const FCogPreflightLimits& Limits, const TSharedRef<Cancellation>& Cancellation,
    FCogPreflightReport& OutReport);
}
