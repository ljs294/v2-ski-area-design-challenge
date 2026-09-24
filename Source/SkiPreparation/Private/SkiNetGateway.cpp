#include "SkiPreparation/SkiNetGateway.h"

#include "Misc/ScopeLock.h"

namespace
{
FCriticalSection TestPortMutex;
#if !UE_BUILD_SHIPPING
uint16 TestLoopbackPort = 0;
FString TestCertificateAuthority;
#endif

bool IsAsciiHost(const FString& Host)
{
    if (Host.IsEmpty() || Host.StartsWith(TEXT(".")) || Host.EndsWith(TEXT("."))) return false;
    for (TCHAR C : Host)
        if (!((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '.')) return false;
    return !Host.Contains(TEXT("..")) && !Host.StartsWith(TEXT("xn--")) && !Host.Contains(TEXT(".xn--"));
}

bool AllowedPath(const FString& Host, const FString& Path)
{
    if (Host == TEXT("basemap.nationalmap.gov"))
        return Path.StartsWith(TEXT("/arcgis/rest/services/USGSImageryOnly/MapServer/tile/"))
            || Path.StartsWith(TEXT("/arcgis/rest/services/USGSImageryTopo/MapServer/tile/"));
    if (Host == TEXT("elevation-tiles-prod.s3.amazonaws.com"))
        return Path.StartsWith(TEXT("/terrarium/")) && Path.EndsWith(TEXT(".png"));
    if (Host == TEXT("nominatim.openstreetmap.org")) return Path.StartsWith(TEXT("/search?"));
    if (Host == TEXT("tnmaccess.nationalmap.gov")) return Path.StartsWith(TEXT("/api/v1/products?"));
    if (Host == TEXT("prd-tnm.s3.amazonaws.com"))
        return Path.StartsWith(TEXT("/StagedProducts/Elevation/"))
            && (Path.EndsWith(TEXT(".tif")) || Path.EndsWith(TEXT(".gpkg")) || Path.EndsWith(TEXT(".xml")));
    if (Host == TEXT("esa-worldcover.s3.eu-central-1.amazonaws.com"))
        return Path.StartsWith(TEXT("/v200/2021/map/ESA_WorldCover_10m_2021_v200_")) && Path.EndsWith(TEXT("_Map.tif"));
    if (Host == TEXT("overpass-api.de")) return Path.StartsWith(TEXT("/api/interpreter?"));
    return false;
}
}

bool SkiPreparation::SkiNetGateway::ValidateUrl(const FString& Url, FString& OutReason)
{
    OutReason = TEXT("INVALID_URL");
    if (!Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive)) return false;
    const FString Remainder = Url.Mid(8);
    int32 Boundary = INDEX_NONE;
    if (!Remainder.FindChar('/', Boundary)) return false;
    const FString Authority = Remainder.Left(Boundary);
    const FString Path = Remainder.Mid(Boundary);
    if (Authority.Contains(TEXT("@")) || Authority.Contains(TEXT("[")) || Authority.Contains(TEXT("]"))) return false;
    FString Host = Authority;
    uint16 Port = 443;
    int32 Colon = INDEX_NONE;
    if (Authority.FindChar(':', Colon))
    {
        Host = Authority.Left(Colon);
        const FString PortText = Authority.Mid(Colon + 1);
        int32 Parsed = 0;
        if (!LexTryParseString(Parsed, *PortText) || Parsed < 1 || Parsed > 65535) return false;
        Port = static_cast<uint16>(Parsed);
    }
    if (!IsAsciiHost(Host)) return false;
    const FString LowerPath = Path.ToLower();
    if (Path.Contains(TEXT("\\")) || Path.Contains(TEXT("#")) || LowerPath.Contains(TEXT("%2e"))
        || LowerPath.Contains(TEXT("%2f")) || LowerPath.Contains(TEXT("%5c")) || Path.Contains(TEXT("/../"))
        || Path.Contains(TEXT("/./")) || Path.Contains(TEXT("//"))) return false;
    if (Host != TEXT("nominatim.openstreetmap.org") && Host != TEXT("tnmaccess.nationalmap.gov")
        && Host != TEXT("overpass-api.de") && Path.Contains(TEXT("%"))) return false;
#if !UE_BUILD_SHIPPING
    {
        FScopeLock Lock(&TestPortMutex);
        if (Host == TEXT("localhost") && TestLoopbackPort != 0 && Port == TestLoopbackPort)
        { OutReason.Empty(); return true; }
    }
#endif
    if (Port != 443 || !AllowedPath(Host, Path)) return false;
    OutReason.Empty();
    return true;
}

bool SkiPreparation::SkiNetGateway::IsAllowedConnectedPort(int32 Port)
{
    if (Port == 443) return true;
#if !UE_BUILD_SHIPPING
    FScopeLock Lock(&TestPortMutex);
    return TestLoopbackPort != 0 && Port == TestLoopbackPort;
#else
    return false;
#endif
}

#if !UE_BUILD_SHIPPING
void SkiPreparation::SkiNetGateway::SetTestLoopbackPort(uint16 Port)
{
    FScopeLock Lock(&TestPortMutex);
    TestLoopbackPort = Port;
}

void SkiPreparation::SkiNetGateway::SetTestCertificateAuthority(const FString& PemPath)
{
    FScopeLock Lock(&TestPortMutex);
    TestCertificateAuthority = PemPath;
}

FString SkiPreparation::SkiNetGateway::GetTestCertificateAuthority()
{
    FScopeLock Lock(&TestPortMutex);
    return TestCertificateAuthority;
}

SkiPreparation::HttpAcquisitionResult SkiPreparation::SkiNetGateway::RunTestRedirectProbe(
    const FString& AllowedUrl, const FString& CaPemPath)
{
    HttpAcquisitionResult Invalid;
    Invalid.FailureReason = TransportFailureReason::Other;
    Invalid.RequestStatus = TEXT("INVALID_TEST_URL");
    if (!AllowedUrl.StartsWith(TEXT("https://localhost:"))) return Invalid;
    const FString AfterAuthority = AllowedUrl.Mid(18);
    int32 Slash = INDEX_NONE;
    if (!AfterAuthority.FindChar('/', Slash)) return Invalid;
    const FString PortText = AfterAuthority.Left(Slash);
    int32 Port = 0;
    if (!LexTryParseString(Port, *PortText) || Port < 1 || Port > 65535) return Invalid;
    SetTestLoopbackPort(static_cast<uint16>(Port));
    SetTestCertificateAuthority(CaPemPath);
    HttpAcquisitionRequest Request;
    Request.Url = AllowedUrl;
    Request.MaximumResponseBytes = 4096;
    Request.TotalTimeoutSeconds = 5.0F;
    const TSharedRef<SkiPreparation::Cancellation> CancellationRef = MakeShared<SkiPreparation::Cancellation>();
    SkiNetGateway Gateway;
    HttpAcquisitionResult Result = Gateway.Get(Request, CancellationRef);
    SetTestCertificateAuthority(FString());
    SetTestLoopbackPort(0);
    return Result;
}

SkiPreparation::HttpAcquisitionResult SkiPreparation::SkiNetGateway::RunTestRangeProbe(
    const FString& AllowedUrl, const FString& CaPemPath, uint64 Offset, uint64 Length)
{
    HttpAcquisitionResult Invalid;
    Invalid.FailureReason = TransportFailureReason::Other;
    Invalid.RequestStatus = TEXT("INVALID_TEST_URL");
    if (!AllowedUrl.StartsWith(TEXT("https://localhost:"))) return Invalid;
    const FString AfterAuthority = AllowedUrl.Mid(18);
    int32 Slash = INDEX_NONE;
    if (!AfterAuthority.FindChar('/', Slash)) return Invalid;
    int32 Port = 0;
    if (!LexTryParseString(Port, *AfterAuthority.Left(Slash)) || Port < 1 || Port > 65535) return Invalid;
    SetTestLoopbackPort(static_cast<uint16>(Port));
    SetTestCertificateAuthority(CaPemPath);
    HttpAcquisitionRequest Request;
    Request.Url = AllowedUrl;
    Request.ByteRange = HttpByteRange{Offset, Length};
    Request.MaximumResponseBytes = 4096;
    Request.TotalTimeoutSeconds = 5.0F;
    const TSharedRef<SkiPreparation::Cancellation> CancellationRef = MakeShared<SkiPreparation::Cancellation>();
    SkiNetGateway Gateway;
    HttpAcquisitionResult Result = Gateway.Get(Request, CancellationRef);
    SetTestCertificateAuthority(FString());
    SetTestLoopbackPort(0);
    return Result;
}
#endif

SkiPreparation::HttpAcquisitionResult SkiPreparation::SkiNetGateway::Get(
    const HttpAcquisitionRequest& Request, const TSharedRef<Cancellation>& Cancellation)
{
    FString Reason;
    if (!ValidateUrl(Request.Url, Reason))
    {
        HttpAcquisitionResult Rejected;
        Rejected.Attempt = Request.Attempt;
        Rejected.FailureReason = TransportFailureReason::Other;
        Rejected.RequestStatus = Reason;
        return Rejected;
    }
    CurlAcquisitionTransport Transport;
    return Transport.Get(Request, Cancellation);
}
