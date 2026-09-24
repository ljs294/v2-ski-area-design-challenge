#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "SkiPreparation/SkiNetGateway.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkiNetGatewayPolicyTest,
    "Ski.P1.Preparation.Gateway.Policy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkiNetGatewayPolicyTest::RunTest(const FString&)
{
    FString Reason;
    const TCHAR* Allowed[] = {
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/n26e19/a.tif"),
        TEXT("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_N46W122_Map.tif"),
        TEXT("https://nominatim.openstreetmap.org/search?q=mountain")
    };
    for (const TCHAR* Url : Allowed) TestTrue(Url, SkiPreparation::SkiNetGateway::ValidateUrl(Url, Reason));
    const TCHAR* Denied[] = {
        TEXT("http://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/a.tif"),
        TEXT("https://user@prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/a.tif"),
        TEXT("https://127.0.0.1/StagedProducts/Elevation/a.tif"),
        TEXT("https://prd-tnm.s3.amazonaws.com:8443/StagedProducts/Elevation/a.tif"),
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/%2e%2e/a.tif"),
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/%2f/a.tif"),
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/..\\a.tif"),
        TEXT("https://evil.example/StagedProducts/Elevation/a.tif"),
        TEXT("https://xn--e1afmkfd.example/StagedProducts/Elevation/a.tif"),
        TEXT("https://elevation-tiles-prod.s3.amazonaws.com/private/object"),
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/secret.txt"),
        TEXT("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/private.tif")
    };
    for (const TCHAR* Url : Denied) TestFalse(Url, SkiPreparation::SkiNetGateway::ValidateUrl(Url, Reason));
    TestTrue(TEXT("Port 443"), SkiPreparation::SkiNetGateway::IsAllowedConnectedPort(443));
    TestFalse(TEXT("Alternate port"), SkiPreparation::SkiNetGateway::IsAllowedConnectedPort(8443));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkiNetGatewayRedirectChainTest,
    "Ski.P1.Preparation.Gateway.RedirectChain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkiNetGatewayRedirectChainTest::RunTest(const FString&)
{
    int32 AllowedPort = 0;
    FString CaPath;
    FString ForbiddenCountPath;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestPort="), AllowedPort)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestCA="), CaPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayForbiddenCount="), ForbiddenCountPath))
    {
        AddWarning(TEXT("Redirect-chain harness arguments absent; external M0-D gate must invoke this test."));
        return true;
    }
    if (AllowedPort < 1 || AllowedPort > 65535) return false;
    SkiPreparation::SkiNetGateway::SetTestLoopbackPort(static_cast<uint16>(AllowedPort));
    SkiPreparation::SkiNetGateway::SetTestCertificateAuthority(CaPath);
    const TCHAR* Cases[] = {
        TEXT("absolute"), TEXT("relative"), TEXT("http-to-https"), TEXT("https-to-http"),
        TEXT("alternate-port"), TEXT("userinfo"), TEXT("encoded-traversal"), TEXT("odd-location"), TEXT("loop")
    };
    for (const TCHAR* Case : Cases)
    {
        SkiPreparation::HttpAcquisitionRequest Request;
        Request.Url = FString::Printf(TEXT("https://localhost:%d/redirect/%s"), AllowedPort, Case);
        Request.TotalTimeoutSeconds = 5.0F;
        Request.MaximumResponseBytes = 4096;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::SkiNetGateway Gateway;
        const SkiPreparation::HttpAcquisitionResult Result = Gateway.Get(Request, Cancellation);
        TestEqual(Case, Result.RequestStatus, FString(TEXT("REDIRECT_DENIED")));
        TestEqual(TEXT("One response only"), Result.HttpStatus, 302);
    }
    FString CountText;
    if (!FFileHelper::LoadFileToString(CountText, *ForbiddenCountPath))
    { AddError(TEXT("Forbidden-listener count receipt missing")); return false; }
    TestEqual(TEXT("Forbidden listener accepted no connections"), CountText.TrimStartAndEnd(), FString(TEXT("0")));
    SkiPreparation::SkiNetGateway::SetTestCertificateAuthority(FString());
    SkiPreparation::SkiNetGateway::SetTestLoopbackPort(0);
    return true;
}

#endif
