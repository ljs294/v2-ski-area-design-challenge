#include "SkiPreparation/PlaceSearch.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Containers/StringConv.h"

#include <cmath>

namespace
{
constexpr int32 MaximumQueryCharacters = 200;
constexpr int32 MaximumSearchResults = 5;
constexpr int32 MaximumResponseBytes = 256 * 1024;
constexpr int32 MaximumCachedQueries = 64;
constexpr double MinimumRequestSpacingSeconds = 1.0;

FCriticalSection NominatimPacingMutex;
double LastNominatimRequestStart = -1.0;

bool IsControlCharacter(const TCHAR Character)
{
    return Character < 0x20 || Character == 0x7f
        || (Character >= 0x80 && Character <= 0x9f);
}

bool NormalizeQuery(const FString& RawQuery, FString& OutQuery, FString& OutError)
{
    OutQuery.Reset();
    OutError.Reset();
    for (const TCHAR Character : RawQuery)
    {
        if (IsControlCharacter(Character))
        {
            OutError = TEXT("PLACE_SEARCH_INVALID_QUERY");
            return false;
        }
    }

    bool bPendingSpace = false;
    for (const TCHAR Character : RawQuery.TrimStartAndEnd())
    {
        if (FChar::IsWhitespace(Character))
        {
            bPendingSpace = !OutQuery.IsEmpty();
            continue;
        }
        if (bPendingSpace) OutQuery.AppendChar(TEXT(' '));
        bPendingSpace = false;
        OutQuery.AppendChar(Character);
    }

    if (OutQuery.IsEmpty())
    {
        OutError = TEXT("PLACE_SEARCH_EMPTY_QUERY");
        return false;
    }
    if (OutQuery.Len() > MaximumQueryCharacters)
    {
        OutError = TEXT("PLACE_SEARCH_QUERY_TOO_LONG");
        return false;
    }
    return true;
}

FString CacheKeyFor(const FString& Query)
{
    FString Key = Query;
    Key.ToLowerInline();
    return Key;
}

FString EncodeQueryParameter(const FString& Value)
{
    static constexpr TCHAR HexDigits[] = TEXT("0123456789ABCDEF");
    const FTCHARToUTF8 Utf8(*Value);
    FString Encoded;
    Encoded.Reserve(Utf8.Length() * 3);
    for (int32 Index = 0; Index < Utf8.Length(); ++Index)
    {
        const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
        const bool bUnreserved = (Byte >= 'a' && Byte <= 'z')
            || (Byte >= 'A' && Byte <= 'Z')
            || (Byte >= '0' && Byte <= '9')
            || Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';
        // A literal slash is valid inside a query value. The gateway rejects encoded
        // slashes globally as a path traversal defense, even after the '?' delimiter.
        if (bUnreserved || Byte == '/')
        {
            Encoded.AppendChar(static_cast<TCHAR>(Byte));
        }
        else
        {
            Encoded.AppendChar(TEXT('%'));
            Encoded.AppendChar(HexDigits[Byte >> 4]);
            Encoded.AppendChar(HexDigits[Byte & 0x0f]);
        }
    }
    return Encoded;
}

bool ReadNumber(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
{
    if (!Value.IsValid()) return false;
    if (Value->Type == EJson::String)
    {
        const FString Text = Value->AsString();
        return LexTryParseString(OutNumber, *Text) && FMath::IsFinite(OutNumber);
    }
    if (Value->Type == EJson::Number)
    {
        OutNumber = Value->AsNumber();
        return FMath::IsFinite(OutNumber);
    }
    return false;
}

bool ReadAddressString(const TSharedPtr<FJsonObject>& Address,
    const TCHAR* Primary, const TCHAR* Secondary, const TCHAR* Tertiary, FString& OutValue)
{
    if (!Address.IsValid()) return false;
    const TCHAR* Fields[] = {Primary, Secondary, Tertiary};
    for (const TCHAR* Field : Fields)
    {
        if (!Field) continue;
        FString Value;
        if (Address->TryGetStringField(Field, Value))
        {
            Value.TrimStartAndEndInline();
            if (!Value.IsEmpty())
            {
                OutValue = Value.Left(128);
                return true;
            }
        }
    }
    return false;
}

bool ParseCandidate(const TSharedPtr<FJsonObject>& Object,
    SkiPreparation::PlaceSearchResult& OutResult)
{
    if (!Object.IsValid()) return false;

    FString LatitudeText;
    FString LongitudeText;
    const TArray<TSharedPtr<FJsonValue>>* BoundingBoxValues = nullptr;
    if (!Object->TryGetStringField(TEXT("lat"), LatitudeText)
        || !Object->TryGetStringField(TEXT("lon"), LongitudeText)
        || !Object->TryGetArrayField(TEXT("boundingbox"), BoundingBoxValues)
        || !BoundingBoxValues || BoundingBoxValues->Num() != 4)
    {
        return false;
    }

    double Latitude = 0.0;
    double Longitude = 0.0;
    if (!LexTryParseString(Latitude, *LatitudeText)
        || !LexTryParseString(Longitude, *LongitudeText)
        || !FMath::IsFinite(Latitude) || !FMath::IsFinite(Longitude)
        || Latitude < -90.0 || Latitude > 90.0
        || Longitude < -180.0 || Longitude > 180.0)
    {
        return false;
    }

    // Nominatim orders its bbox as south, north, west, east.
    double South = 0.0;
    double North = 0.0;
    double West = 0.0;
    double East = 0.0;
    if (!ReadNumber((*BoundingBoxValues)[0], South)
        || !ReadNumber((*BoundingBoxValues)[1], North)
        || !ReadNumber((*BoundingBoxValues)[2], West)
        || !ReadNumber((*BoundingBoxValues)[3], East)
        || South < -90.0 || North > 90.0 || South > North
        || West < -180.0 || East > 180.0 || West > East
        || Latitude < South - 1.0e-6 || Latitude > North + 1.0e-6
        || Longitude < West - 1.0e-6 || Longitude > East + 1.0e-6)
    {
        return false;
    }

    FString DisplayName;
    Object->TryGetStringField(TEXT("display_name"), DisplayName);
    DisplayName.TrimStartAndEndInline();
    DisplayName = DisplayName.Left(512);

    FString Name;
    Object->TryGetStringField(TEXT("name"), Name);
    Name.TrimStartAndEndInline();
    if (Name.IsEmpty() && !DisplayName.IsEmpty())
    {
        if (!DisplayName.Split(TEXT(","), &Name, nullptr)) Name = DisplayName;
        Name.TrimStartAndEndInline();
    }
    if (Name.IsEmpty()) return false;

    const TSharedPtr<FJsonObject>* AddressObject = nullptr;
    Object->TryGetObjectField(TEXT("address"), AddressObject);
    const TSharedPtr<FJsonObject> Address = AddressObject ? *AddressObject : nullptr;

    OutResult.Name = Name.Left(256);
    OutResult.DisplayName = DisplayName;
    OutResult.Point = {Latitude, Longitude};
    OutResult.BoundingBox = {West, South, East, North};
    ReadAddressString(Address, TEXT("state"), TEXT("region"), TEXT("province"), OutResult.Region);
    if (OutResult.Region.IsEmpty())
        ReadAddressString(Address, TEXT("county"), nullptr, nullptr, OutResult.Region);
    ReadAddressString(Address, TEXT("country"), nullptr, nullptr, OutResult.Country);
    return true;
}

bool WaitForNominatimSlot(const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
    bool& bOutLocked)
{
    bOutLocked = false;
    while (!Cancellation->IsCancelled())
    {
        if (NominatimPacingMutex.TryLock())
        {
            bOutLocked = true;
            break;
        }
        FPlatformProcess::SleepNoStats(0.01F);
    }
    if (!bOutLocked) return false;

    if (LastNominatimRequestStart >= 0.0)
    {
        const double EarliestStart = LastNominatimRequestStart + MinimumRequestSpacingSeconds;
        while (!Cancellation->IsCancelled())
        {
            const double Remaining = EarliestStart - FPlatformTime::Seconds();
            if (Remaining <= 0.0) return true;
            FPlatformProcess::SleepNoStats(static_cast<float>(FMath::Min(Remaining, 0.01)));
        }
        NominatimPacingMutex.Unlock();
        bOutLocked = false;
        return false;
    }
    return !Cancellation->IsCancelled();
}

class FScopedNominatimPacingSlot final
{
public:
    explicit FScopedNominatimPacingSlot(
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation)
        : bAcquired(WaitForNominatimSlot(Cancellation, bLocked)) {}

    ~FScopedNominatimPacingSlot()
    {
        if (bLocked) NominatimPacingMutex.Unlock();
    }

    bool IsAcquired() const noexcept { return bAcquired; }

    void MarkRequestStarted() noexcept
    {
        LastNominatimRequestStart = FPlatformTime::Seconds();
    }

private:
    bool bLocked = false;
    bool bAcquired = false;
};
}

SkiPreparation::NominatimSearchProvider::NominatimSearchProvider(
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport)
    : Transport(MoveTemp(InTransport))
{
    if (!Transport.IsValid())
        Transport = MakeShared<SkiNetGateway, ESPMode::ThreadSafe>();
}

bool SkiPreparation::NominatimSearchProvider::Search(const FString& RawQuery,
    const TSharedRef<Cancellation>& Cancellation, TArray<PlaceSearchResult>& OutResults,
    FString& OutError)
{
    OutResults.Reset();
    OutError.Reset();
    if (Cancellation->IsCancelled())
    {
        OutError = TEXT("PLACE_SEARCH_CANCELLED");
        return false;
    }

    FString Query;
    if (!NormalizeQuery(RawQuery, Query, OutError)) return false;
    const FString CacheKey = CacheKeyFor(Query);
    {
        FScopeLock Lock(&CacheMutex);
        if (const TArray<PlaceSearchResult>* Cached = Cache.Find(CacheKey))
        {
            if (Cancellation->IsCancelled())
            {
                OutError = TEXT("PLACE_SEARCH_CANCELLED");
                return false;
            }
            OutResults = *Cached;
            return true;
        }
    }

    const FString Url = FString::Printf(
        TEXT("https://nominatim.openstreetmap.org/search?q=%s&format=jsonv2&limit=%d&addressdetails=1"),
        *EncodeQueryParameter(Query), MaximumSearchResults);
    FString UrlReason;
    if (!SkiNetGateway::ValidateUrl(Url, UrlReason))
    {
        OutError = TEXT("PLACE_SEARCH_INVALID_QUERY");
        return false;
    }
    HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.Product = ProviderProduct::None;
    Request.Attempt = 1;
    Request.ActivityTimeoutSeconds = 15.0F;
    Request.TotalTimeoutSeconds = 30.0F;
    Request.MaximumResponseBytes = MaximumResponseBytes;

    FScopedNominatimPacingSlot PacingSlot(Cancellation);
    if (!PacingSlot.IsAcquired())
    {
        OutError = TEXT("PLACE_SEARCH_CANCELLED");
        return false;
    }
    if (Cancellation->IsCancelled())
    {
        OutError = TEXT("PLACE_SEARCH_CANCELLED");
        return false;
    }
    PacingSlot.MarkRequestStarted();
    const HttpAcquisitionResult Response = Transport->Get(Request, Cancellation);
    if (Cancellation->IsCancelled())
    {
        OutError = TEXT("PLACE_SEARCH_CANCELLED");
        return false;
    }
    if (!Response.Ok())
    {
        OutError = FString::Printf(TEXT("PLACE_SEARCH_REQUEST_FAILED:%s:%d"),
            *Response.RequestStatus.Left(96), Response.HttpStatus);
        return false;
    }
    if (Response.Bytes.Num() > MaximumResponseBytes)
    {
        OutError = TEXT("PLACE_SEARCH_RESPONSE_TOO_LARGE");
        return false;
    }
    if (!Response.ContentType.IsEmpty()
        && !Response.ContentType.Contains(TEXT("json"), ESearchCase::IgnoreCase))
    {
        OutError = TEXT("PLACE_SEARCH_UNEXPECTED_CONTENT_TYPE");
        return false;
    }

    const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Response.Bytes.GetData()),
        Response.Bytes.Num());
    const FString JsonText(Utf8.Length(), Utf8.Get());
    TArray<TSharedPtr<FJsonValue>> RootArray;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), RootArray))
    {
        OutError = TEXT("PLACE_SEARCH_INVALID_JSON");
        return false;
    }

    TArray<PlaceSearchResult> ParsedResults;
    ParsedResults.Reserve(FMath::Min(RootArray.Num(), MaximumSearchResults));
    for (const TSharedPtr<FJsonValue>& Value : RootArray)
    {
        if (Cancellation->IsCancelled())
        {
            OutError = TEXT("PLACE_SEARCH_CANCELLED");
            return false;
        }
        if (ParsedResults.Num() >= MaximumSearchResults) break;
        if (!Value.IsValid() || Value->Type != EJson::Object) continue;
        PlaceSearchResult Candidate;
        if (ParseCandidate(Value->AsObject(), Candidate))
            ParsedResults.Add(MoveTemp(Candidate));
    }

    if (Cancellation->IsCancelled())
    {
        OutError = TEXT("PLACE_SEARCH_CANCELLED");
        return false;
    }
    {
        FScopeLock Lock(&CacheMutex);
        if (!Cache.Contains(CacheKey))
        {
            while (CacheOrder.Num() >= MaximumCachedQueries)
            {
                Cache.Remove(CacheOrder[0]);
                CacheOrder.RemoveAt(0);
            }
        }
        else
        {
            CacheOrder.Remove(CacheKey);
        }
        Cache.Add(CacheKey, ParsedResults);
        CacheOrder.Add(CacheKey);
    }
    OutResults = MoveTemp(ParsedResults);
    return true;
}
