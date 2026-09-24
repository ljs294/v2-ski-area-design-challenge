#pragma once

#include "SkiPreparation/TerrainAcquisition.h"

namespace SkiPreparation
{
/** The sole policy entry point for acquisition URLs. Validation has no network side effects. */
class SKIPREPARATION_API SkiNetGateway final : public IAcquisitionTransport
{
public:
    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& Cancellation) override;
    static bool ValidateUrl(const FString& Url, FString& OutReason);
    static bool IsAllowedConnectedPort(int32 Port);
#if !UE_BUILD_SHIPPING
    /** Development-only loopback endpoint for a local TLS redirect harness. */
    static void SetTestLoopbackPort(uint16 Port);
    static void SetTestCertificateAuthority(const FString& PemPath);
    static FString GetTestCertificateAuthority();
    static HttpAcquisitionResult RunTestRedirectProbe(const FString& AllowedUrl, const FString& CaPemPath);
    static HttpAcquisitionResult RunTestRangeProbe(const FString& AllowedUrl, const FString& CaPemPath,
        uint64 Offset, uint64 Length);
#endif
};

class SKIPREPARATION_API CurlAcquisitionTransport final : public IAcquisitionTransport
{
public:
    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& Cancellation) override;
};
}
