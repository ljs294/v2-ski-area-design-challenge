#include "SkiPreparation/SkiNetGateway.h"

#include "HAL/PlatformTime.h"
#include "Containers/StringConv.h"
#include "Ssl.h"

#include <curl/curl.h>

namespace
{
struct FTransfer
{
    SkiPreparation::HttpAcquisitionResult Result;
    const SkiPreparation::HttpAcquisitionRequest* Request = nullptr;
    const SkiPreparation::Cancellation* Cancellation = nullptr;
    bool bTooLarge = false;
    bool bWrongConnection = false;
    double Began = 0.0;
};

CURLcode ConfigureTrustStore(CURL*, void* Context, void*)
{
    if (!Context) return CURLE_SSL_CERTPROBLEM;
    // Engine libcurl has no Windows CA bundle by default. Match HTTP's trusted
    // root setup while retaining curl's peer and hostname verification.
    FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(
        static_cast<SSL_CTX*>(Context));
    return CURLE_OK;
}

size_t WriteBody(char* Data, size_t Size, size_t Count, void* User)
{
    FTransfer& T = *static_cast<FTransfer*>(User);
    const size_t Length = Size * Count;
    if (Size != 0 && Length / Size != Count) return 0;
    if (Length > T.Request->MaximumResponseBytes ||
        static_cast<uint64>(T.Result.Bytes.Num()) > T.Request->MaximumResponseBytes - Length ||
        Length > static_cast<size_t>(MAX_int32 - T.Result.Bytes.Num()))
    { T.bTooLarge = true; return 0; }
    if (T.Result.TimeToFirstByteSeconds < 0.0) T.Result.TimeToFirstByteSeconds = FPlatformTime::Seconds() - T.Began;
    T.Result.Bytes.Append(reinterpret_cast<const uint8*>(Data), static_cast<int32>(Length));
    T.Result.BytesReceived = T.Result.Bytes.Num();
    return Length;
}

size_t ReadHeader(char* Data, size_t Size, size_t Count, void* User)
{
    FTransfer& T = *static_cast<FTransfer*>(User);
    const size_t Length = Size * Count;
    if (Size != 0 && Length / Size != Count) return 0;
    const FUTF8ToTCHAR Converted(Data, static_cast<int32>(FMath::Min<size_t>(Length, 1024)));
    const FString Header(Converted.Length(), Converted.Get());
    auto Value = [&Header](const TCHAR* Prefix) -> FString
    { return Header.Mid(FCString::Strlen(Prefix)).TrimStartAndEnd(); };
    if (Header.StartsWith(TEXT("Content-Type:"), ESearchCase::IgnoreCase)) T.Result.ContentType = Value(TEXT("Content-Type:")).Left(128);
    else if (Header.StartsWith(TEXT("Content-Range:"), ESearchCase::IgnoreCase)) T.Result.ContentRange = Value(TEXT("Content-Range:")).Left(128);
    else if (Header.StartsWith(TEXT("Retry-After:"), ESearchCase::IgnoreCase)) T.Result.RetryAfter = Value(TEXT("Retry-After:")).Left(64);
    else if (Header.StartsWith(TEXT("ETag:"), ESearchCase::IgnoreCase))
    {
        const FString Tag = Value(TEXT("ETag:"));
        if (Tag.Len() <= 256 && !Tag.Contains(TEXT("\r")) && !Tag.Contains(TEXT("\n")))
            T.Result.ETag = Tag;
    }
    else if (Header.StartsWith(TEXT("Content-Length:"), ESearchCase::IgnoreCase))
    {
        uint64 Declared = 0;
        if (!LexTryParseString(Declared, *Value(TEXT("Content-Length:"))) || Declared > T.Request->MaximumResponseBytes)
        { T.bTooLarge = true; return 0; }
    }
    return Length;
}

int OnCurlProgress(void* User, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const FTransfer& T = *static_cast<FTransfer*>(User);
    return T.Cancellation->IsCancelled() ||
        (T.Request->AbsoluteOperationDeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= T.Request->AbsoluteOperationDeadlineSeconds) ? 1 : 0;
}

int BeforeConnect(void* User, char*, char*, int RemotePort, int)
{
    FTransfer& T = *static_cast<FTransfer*>(User);
    if (!SkiPreparation::SkiNetGateway::IsAllowedConnectedPort(RemotePort))
    { T.bWrongConnection = true; return 1; }
    return 0;
}
}

SkiPreparation::HttpAcquisitionResult SkiPreparation::CurlAcquisitionTransport::Get(
    const HttpAcquisitionRequest& Request, const TSharedRef<Cancellation>& Cancellation)
{
    FTransfer T;
    T.Request = &Request;
    T.Cancellation = &Cancellation.Get();
    T.Began = FPlatformTime::Seconds();
    T.Result.Attempt = Request.Attempt;
    if (!PermitAcquisitionBeforeHandle())
    { T.Result.FailureReason = TransportFailureReason::Cancelled; T.Result.RequestStatus = TEXT("BlockedByOfflineGuard"); return T.Result; }
    FString Reason;
    if (!SkiNetGateway::ValidateUrl(Request.Url, Reason))
    { T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = Reason; return T.Result; }
    CURL* Curl = curl_easy_init();
    if (!Curl)
    { T.Result.FailureReason = TransportFailureReason::QueueFailure; T.Result.RequestStatus = TEXT("CurlInitFailed"); return T.Result; }
    const FTCHARToUTF8 Url(*Request.Url);
    curl_easy_setopt(Curl, CURLOPT_URL, Url.Get());
    curl_easy_setopt(Curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(Curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(Curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(Curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(Curl, CURLOPT_PROXY, "");
    curl_easy_setopt(Curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(Curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    curl_easy_setopt(Curl, CURLOPT_COOKIEFILE, nullptr);
    curl_easy_setopt(Curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(Curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(Curl, CURLOPT_SSL_CTX_FUNCTION, ConfigureTrustStore);
    curl_easy_setopt(Curl, CURLOPT_USERAGENT, "MountainPlanner-Unreal-P1/1");
#if !UE_BUILD_SHIPPING
    const FString TestCa = SkiNetGateway::GetTestCertificateAuthority();
    const FTCHARToUTF8 TestCaUtf8(*TestCa);
    if (!TestCa.IsEmpty()) curl_easy_setopt(Curl, CURLOPT_CAINFO, TestCaUtf8.Get());
#endif
    curl_easy_setopt(Curl, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(Curl, CURLOPT_WRITEDATA, &T);
    curl_easy_setopt(Curl, CURLOPT_HEADERFUNCTION, ReadHeader);
    curl_easy_setopt(Curl, CURLOPT_HEADERDATA, &T);
    curl_easy_setopt(Curl, CURLOPT_XFERINFOFUNCTION, OnCurlProgress);
    curl_easy_setopt(Curl, CURLOPT_XFERINFODATA, &T);
    curl_easy_setopt(Curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(Curl, CURLOPT_PREREQFUNCTION, BeforeConnect);
    curl_easy_setopt(Curl, CURLOPT_PREREQDATA, &T);
    curl_easy_setopt(Curl, CURLOPT_TIMEOUT_MS, static_cast<long>(FMath::Max(1.0F, Request.TotalTimeoutSeconds) * 1000.0F));
    curl_easy_setopt(Curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(FMath::Max(1.0F, Request.ActivityTimeoutSeconds) * 1000.0F));
    struct curl_slist* Headers = nullptr;
    if (!Request.IfMatchETag.IsEmpty())
    {
        const FString& Tag = Request.IfMatchETag;
        if (Tag.Len() > 256 || Tag.Contains(TEXT("\r")) || Tag.Contains(TEXT("\n"))
            || Tag.Contains(TEXT(":")) || Tag.Contains(TEXT("\\")))
        { curl_easy_cleanup(Curl); T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = TEXT("InvalidETag"); return T.Result; }
        const FString Header = TEXT("If-Match: ") + Tag;
        const FTCHARToUTF8 HeaderUtf8(*Header);
        Headers = curl_slist_append(Headers, HeaderUtf8.Get());
    }
    if (Request.ByteRange.IsSet())
    {
        const HttpByteRange Range = Request.ByteRange.GetValue();
        if (Range.Length == 0 || Range.Offset > MAX_uint64 - (Range.Length - 1))
        { curl_easy_cleanup(Curl); T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = TEXT("InvalidByteRange"); return T.Result; }
        const FString Header = FString::Printf(TEXT("Range: bytes=%llu-%llu"), Range.Offset, Range.Offset + Range.Length - 1);
        const FTCHARToUTF8 Encoded(*Header);
        Headers = curl_slist_append(Headers, Encoded.Get());
    }
    if (Headers) curl_easy_setopt(Curl, CURLOPT_HTTPHEADER, Headers);
    const CURLcode Status = curl_easy_perform(Curl);
    long HttpStatus = 0;
    curl_easy_getinfo(Curl, CURLINFO_RESPONSE_CODE, &HttpStatus);
    T.Result.HttpStatus = static_cast<int32>(HttpStatus);
    T.Result.ElapsedSeconds = FPlatformTime::Seconds() - T.Began;
    if (Headers) curl_slist_free_all(Headers);
    curl_easy_cleanup(Curl);
    if (T.bWrongConnection) { T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = TEXT("CONNECTION_DENIED"); }
    else if (HttpStatus >= 300 && HttpStatus < 400) { T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = TEXT("REDIRECT_DENIED"); T.Result.Bytes.Reset(); }
    else if (T.bTooLarge) { T.Result.FailureReason = TransportFailureReason::ResponseTooLarge; T.Result.RequestStatus = TEXT("ResponseTooLarge"); }
    else if (Cancellation->IsCancelled()) { T.Result.FailureReason = TransportFailureReason::Cancelled; T.Result.RequestStatus = TEXT("Cancelled"); }
    else if (Status == CURLE_OPERATION_TIMEDOUT) { T.Result.FailureReason = TransportFailureReason::TimedOut; T.Result.RequestStatus = TEXT("TimedOut"); }
    else if (Status != CURLE_OK) { T.Result.FailureReason = TransportFailureReason::ConnectionError; T.Result.RequestStatus = UTF8_TO_TCHAR(curl_easy_strerror(Status)); }
    else if (HttpStatus < 200 || HttpStatus >= 300) { T.Result.FailureReason = TransportFailureReason::HttpStatus; T.Result.RequestStatus = TEXT("HttpStatus"); }
    else if (T.Result.Bytes.IsEmpty()) { T.Result.FailureReason = TransportFailureReason::EmptyResponse; T.Result.RequestStatus = TEXT("EmptyResponse"); }
    else if (Request.ByteRange.IsSet())
    {
        uint64 Total = 0;
        if (HttpStatus != 206 || !ValidateContentRange(T.Result.ContentRange, Request.ByteRange.GetValue(), T.Result.Bytes.Num(), Total))
        { T.Result.Bytes.Reset(); T.Result.FailureReason = TransportFailureReason::Other; T.Result.RequestStatus = TEXT("InvalidContentRange"); }
    }
    return T.Result;
}
