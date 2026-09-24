#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/WorldCoverCogDecoder.h"
#include "SkiPreparation/SkiNetGateway.h"

namespace SkiPreparation
{
/** M0-only bounded range source. Initialize performs a 206 request to discover exact object size. */
class SKIPREPARATION_API FM0GatewayCogByteSource final : public ICogByteSource
{
public:
    explicit FM0GatewayCogByteSource(FString InUrl, uint64 InMaxTransferredBytes = MAX_uint64,
        uint64 InMaxRequests = MAX_uint64, IAcquisitionTransport* InTransport = nullptr);
    bool Initialize(FString& OutError);
    uint64 Size() const noexcept override { return ObjectSize; }
    bool Read(uint64 Offset, uint64 Length, TArray<uint8>& OutBytes, FString& OutError) override;
    uint64 RequestCount() const noexcept { return Requests; }
    uint64 TransferredBytes() const noexcept { return TransferredBytesCount; }
private:
    FString Url;
    SkiNetGateway Gateway;
    IAcquisitionTransport* Transport = nullptr;
    uint64 ObjectSize = 0;
    uint64 Requests = 0;
    uint64 TransferredBytesCount = 0;
    uint64 MaxTransferredBytes = MAX_uint64;
    uint64 MaxRequests = MAX_uint64;
    FString PinnedETag;
    TSharedRef<Cancellation> Cancel = MakeShared<Cancellation>();
};

struct SKIPREPARATION_API FM0RasterProjectionReceipt
{
    bool bCogPassed = false;
    bool bGeoPackagePassed = false;
    bool bProjPassed = false;
    bool bShippingProjPassed = false;
    bool bFallbackProjectionPassed = false;
    bool bRealS1MProbe = false;
    bool bBaseTileDecoded = false;
    bool bOverviewTileDecoded = false;
    FString CogError;
    FString GeoPackageError;
    FString ProjError;
    FString FallbackProjectionError;
    uint32 Width = 0;
    uint32 Height = 0;
    uint32 OverviewCount = 0;
    uint64 GatewayRangeRequests = 0;
    uint64 GatewayRangeBytes = 0;
    uint64 ValidSamples = 0;
    uint64 NoDataSamples = 0;
    int64 SourceInputRows = 0;
    int64 BlendingRows = 0;
    int32 QualityLevel = -1;
    uint32 SourceGeometryBytes = 0;
    uint32 BlendingGeometryBytes = 0;
    double ProjRoundTripMeters = 0;
    double ProjControlErrorMeters = 0;
    double GnAlbersErrorMeters = 0;
    double GnTransverseMercatorErrorMeters = 0;
    double NgsDatumErrorMeters = 0;
    double FallbackGnAlbersErrorMeters = 0;
    double FallbackGnTransverseMercatorErrorMeters = 0;
    double FallbackS1MRoundTripMeters = 0;
    double FallbackProjectRoundTripMeters = 0;
    double FallbackDatumApproximationMeters = 0;
    double ProjLatticeMaxErrorMeters = 0;
    double ProjSamplesPerSecond = 0;
    FString ToJson() const;
};

/** Callable in a packaged build. Paths are caller-selected packaged/local fixtures; no network is opened. */
SKIPREPARATION_API FM0RasterProjectionReceipt RunM0RasterProjectionProbe(
    ICogByteSource& Cog, const FString& GeoPackagePath);
/** Convenience entry for packaged command-line smoke. Returns a receipt even on file-open failure. */
SKIPREPARATION_API FM0RasterProjectionReceipt RunM0RasterProjectionProbe(
    const FString& CogPath, const FString& GeoPackagePath);
/** Live S1M proof: metadata plus one base tile and one overview tile, no whole-image loop. */
SKIPREPARATION_API FM0RasterProjectionReceipt RunM0RealS1MCogProbe(const FString& CogUrl);
/** Development TLS loopback integration: typed 206 ranges pass through SkiNetGateway into LibTiff. */
#if !UE_BUILD_SHIPPING
SKIPREPARATION_API FM0RasterProjectionReceipt RunM0GatewayRasterProbe(
    const FString& CogUrl, const FString& CertificateAuthorityPemPath, const FString& GeoPackagePath);
#endif
}
