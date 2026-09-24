#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiPreparation/ImageryPyramid.h"

namespace SkiPreparation
{
/** Hard limits for the bounded site-imagery acquisition slice. The
 * working-memory figure bounds application-owned tile/cache/scratch buffers. */
struct SKIPREPARATION_API ImageryAcquisitionBudget
{
    std::uint32_t MaximumPyramidTiles = 512;
    std::uint32_t MaximumSourceRequests = 512;
    std::uint32_t MaximumSourceTilesPerOutputTile = 16;
    std::uint64_t MaximumSourceTileBytes = 2ULL * 1024ULL * 1024ULL;
    std::uint64_t MaximumSourceTransferBytes = 128ULL * 1024ULL * 1024ULL;
    std::uint64_t MaximumDecodedSourceBytes = 8ULL * 1024ULL * 1024ULL;
    std::uint64_t MaximumWorkingMemoryBytes = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t MaximumOutputTileBytes = 1ULL * 1024ULL * 1024ULL;
    std::uint64_t MaximumOutputBytes = 256ULL * 1024ULL * 1024ULL;
    double MaximumElapsedSeconds = 900.0;
};

enum class ImageryAcquisitionError : std::uint8_t
{
    None,
    Cancelled,
    InvalidTerrainCore,
    InvalidBudget,
    TooManyPyramidTiles,
    RequestBudgetExceeded,
    TransferBudgetExceeded,
    DecodedMemoryBudgetExceeded,
    OutputBudgetExceeded,
    DeadlineExceeded,
    UnsupportedCoordinate,
    InvalidGatewayUrl,
    MissingSourceTile,
    SourceRequestFailed,
    UnsupportedSourceEncoding,
    InvalidSourceImage,
    NoDataInOutputTile,
    OutputEncodingFailed,
    AssetWriteFailed,
    InvalidOutputManifest,
};

/** One-at-a-time acquisition evidence; encoded tile bytes remain at the writer seam. */
struct SKIPREPARATION_API ImageryAcquisitionReport
{
    ImageryAcquisitionError Error = ImageryAcquisitionError::None;
    std::int32_t TileIndex = INDEX_NONE;
    std::uint32_t SourceTileX = 0;
    std::uint32_t SourceTileY = 0;
    std::int32_t HttpStatus = 0;
    std::uint32_t Requests = 0;
    std::uint64_t SourceBytes = 0;
    std::uint64_t OutputBytes = 0;
    std::uint64_t BoundedWorkingMemoryBytes = 0;
    std::uint64_t NoDataSamples = 0;
    double ElapsedSeconds = 0.0;
    FString FailureDetail;
    ImageryPyramidManifest Manifest;
    bool Ok() const noexcept { return Error == ImageryAcquisitionError::None; }
};

/**
 * Synchronous staging writer seam. It must consume the view before returning and
 * must not activate or publish a package on its own.
 */
class SKIPREPARATION_API IImageryPyramidAssetWriter
{
public:
    virtual ~IImageryPyramidAssetWriter() = default;
    virtual bool Write(const std::string& RelativePath, TArrayView<const uint8> Bytes,
        const Cancellation& CancellationValue, FString& OutError) = 0;
};

/**
 * Retrieves USGSImageryOnly z17 source tiles through SkiNetGateway, reprojects them
 * into the exact TerrainCore tile grid, and writes 256x256 JPEG assets one at a time.
 * The caller owns the staging and atomic-activation path. This production entrypoint
 * is gateway-backed in every configuration, including Shipping.
 */
SKIPREPARATION_API bool AcquireUsgsImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    IImageryPyramidAssetWriter& AssetWriter,
    const TSharedRef<Cancellation>& CancellationValue,
    ImageryAcquisitionReport& OutReport,
    const ImageryAcquisitionBudget& Budget = {});

#if !UE_BUILD_SHIPPING && WITH_DEV_AUTOMATION_TESTS
class IAcquisitionTransport;

/** Offline scripted-transport entrypoint; never present in Shipping. */
SKIPREPARATION_API bool AcquireUsgsImageryPyramidForTest(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    IAcquisitionTransport& ScriptedTransport,
    IImageryPyramidAssetWriter& AssetWriter,
    const TSharedRef<Cancellation>& CancellationValue,
    ImageryAcquisitionReport& OutReport,
    const ImageryAcquisitionBudget& Budget = {});
#endif

SKIPREPARATION_API ImagerySourceMetadata MakeUsgsImageryOnlyMetadata();
}
