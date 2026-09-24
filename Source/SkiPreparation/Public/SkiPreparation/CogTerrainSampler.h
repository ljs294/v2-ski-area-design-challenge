#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/CogPreflight.h"
#include "SkiPreparation/TerrainScratchStore.h"

namespace SkiPreparation
{
/**
 * One already-resolved COG candidate in priority order. The mapping callback is supplied by
 * the caller's projection adapter and returns source sample-centre coordinates (pixel 0,0 is
 * the first pixel centre). A false return means this source does not cover the requested point.
 *
 * S1M candidates must provide ResolveS1mProvenance, backed by the source's verified lineage
 * polygons rasterized onto its native pixel lattice. This prevents the sampler from silently
 * labelling every S1M sample as native.
 */
struct SKIPREPARATION_API FCogTerrainSamplerSource
{
    FString Url;
    SkiDomain::ElevationProduct Product = SkiDomain::ElevationProduct::ArcSec13;
    FCogPreflightMetadata Metadata;
    uint8 ScratchSourceIndex = 0;
    TFunction<bool(uint32 TargetColumn, uint32 TargetRow,
        double& OutSourceSampleColumn, double& OutSourceSampleRow)> MapTargetToSourcePixel;
    TFunction<bool(double SourceSampleColumn, double SourceSampleRow,
        TerrainScratchProvenance& OutProvenance)> ResolveS1mProvenance;
};

/** Aggregate limits for one deterministic, serial sampler run. */
struct SKIPREPARATION_API FCogTerrainSamplerLimits
{
    FCogPreflightLimits Preflight;
    uint64 MaxRequests = 131072;
    uint64 MaxTransferredBytes = 1024ULL * 1024ULL * 1024ULL;
    uint64 MaxResidentBytes = 128ULL * 1024ULL * 1024ULL;
    uint64 MaxOutputSamples = SkiDomain::TerrainCoreMaxSamples;
    uint64 MaxEncodedTileBytes = 16ULL * 1024ULL * 1024ULL;
    uint64 MaxDecodedTileBytes = 16ULL * 1024ULL * 1024ULL;
};

struct SKIPREPARATION_API FCogTerrainSamplerReport
{
    bool bPassed = false;
    FString FailureCode;
    FString FailureDetail;
    uint64 Requests = 0;
    uint64 TransferredBytes = 0;
    uint64 TotalSamples = 0;
    uint64 ValidSamples = 0;
    uint64 NoDataSamples = 0;
    TArray<uint64> SamplesBySource;
    TArray<uint64> SamplesByProvenance;
};

/**
 * Runs COG preflight and then samples the same strong-ETag-pinned objects into the canonical
 * 1 m scratch store. Each output sample uses the first source whose four bilinear taps are
 * valid. COG ranges, decoded source tiles, output scratch tiles, requests and transfer bytes
 * are bounded by Limits. Sources are visited serially in the caller-provided resolver order.
 */
SKIPREPARATION_API bool SampleVerifiedElevationCogsToScratch(
    IAcquisitionTransport& Transport,
    const TArray<FCogTerrainSamplerSource>& Sources,
    TerrainScratchStore& Scratch,
    const FCogTerrainSamplerLimits& Limits,
    const TSharedRef<Cancellation>& Cancellation,
    FCogTerrainSamplerReport& OutReport);
}
