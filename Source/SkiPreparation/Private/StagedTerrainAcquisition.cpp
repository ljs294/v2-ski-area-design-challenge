#include "SkiPreparation/StagedTerrainAcquisition.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "SkiPreparation/TerrainScratchStore.h"

#include <cmath>
#include <limits>

namespace
{
using namespace SkiPreparation;

constexpr uint32 ReceiptSchemaVersion = 1;

bool IsFiniteBounds(const SkiDomain::GeographicBounds& Bounds)
{
    return std::isfinite(Bounds.WestDeg) && std::isfinite(Bounds.SouthDeg)
        && std::isfinite(Bounds.EastDeg) && std::isfinite(Bounds.NorthDeg)
        && Bounds.WestDeg >= -180.0 && Bounds.WestDeg < Bounds.EastDeg
        && Bounds.EastDeg <= 180.0 && Bounds.SouthDeg >= -90.0
        && Bounds.SouthDeg < Bounds.NorthDeg && Bounds.NorthDeg <= 90.0;
}

bool IsStrongETag(const FString& Value)
{
    if (Value.Len() < 2 || Value.Len() > 128 || Value[0] != TEXT('"')
        || Value[Value.Len() - 1] != TEXT('"')) return false;
    for (int32 Index = 1; Index < Value.Len() - 1; ++Index)
    {
        const TCHAR C = Value[Index];
        if (C < 0x21 || C > 0x7e || C == TEXT('"') || C == TEXT(':') || C == TEXT('\\'))
            return false;
    }
    return true;
}

bool IsSha256(const FString& Value)
{
    if (Value.Len() != 64) return false;
    for (const TCHAR C : Value)
        if (!FChar::IsHexDigit(C)) return false;
    return true;
}

FString ProductCode(const SkiDomain::ElevationProduct Product)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return TEXT("S1M");
    case SkiDomain::ElevationProduct::Project1m: return TEXT("Project1m");
    case SkiDomain::ElevationProduct::ArcSec13: return TEXT("ArcSec13");
    default: return FString();
    }
}

int32 ProductPriority(const SkiDomain::ElevationProduct Product)
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return 0;
    case SkiDomain::ElevationProduct::Project1m: return 1;
    case SkiDomain::ElevationProduct::ArcSec13: return 2;
    default: return INDEX_NONE;
    }
}

FString StageName(const EStagedTerrainStage Stage)
{
    switch (Stage)
    {
    case EStagedTerrainStage::Catalog: return TEXT("Catalog");
    case EStagedTerrainStage::SourcePreflight: return TEXT("SourcePreflight");
    case EStagedTerrainStage::CanonicalLod0: return TEXT("CanonicalLod0");
    case EStagedTerrainStage::CoarseLodCopy: return TEXT("CoarseLodCopy");
    case EStagedTerrainStage::WorldCover: return TEXT("WorldCover");
    case EStagedTerrainStage::TerrainCoreStaging: return TEXT("TerrainCoreStaging");
    case EStagedTerrainStage::TerrainCoreVerification: return TEXT("TerrainCoreVerification");
    case EStagedTerrainStage::Complete: return TEXT("Complete");
    default: return FString();
    }
}

bool ParseStage(const FString& Value, EStagedTerrainStage& OutStage)
{
    for (uint8 Index = static_cast<uint8>(EStagedTerrainStage::Catalog);
         Index <= static_cast<uint8>(EStagedTerrainStage::Complete); ++Index)
    {
        const EStagedTerrainStage Candidate = static_cast<EStagedTerrainStage>(Index);
        if (Value == StageName(Candidate)) { OutStage = Candidate; return true; }
    }
    return false;
}

FString StagedJobStateName(const EStagedTerrainJobState State)
{
    switch (State)
    {
    case EStagedTerrainJobState::Ready: return TEXT("Ready");
    case EStagedTerrainJobState::Running: return TEXT("Running");
    case EStagedTerrainJobState::Paused: return TEXT("Paused");
    case EStagedTerrainJobState::Failed: return TEXT("Failed");
    case EStagedTerrainJobState::Complete: return TEXT("Complete");
    default: return FString();
    }
}

bool ParseState(const FString& Value, EStagedTerrainJobState& OutState)
{
    for (uint8 Index = static_cast<uint8>(EStagedTerrainJobState::Ready);
         Index <= static_cast<uint8>(EStagedTerrainJobState::Complete); ++Index)
    {
        const EStagedTerrainJobState Candidate = static_cast<EStagedTerrainJobState>(Index);
        if (Value == StagedJobStateName(Candidate)) { OutState = Candidate; return true; }
    }
    return false;
}

TSharedRef<FJsonObject> NewObject()
{
    return MakeShared<FJsonObject>();
}

void PutUInt(TSharedRef<FJsonObject> Object, const TCHAR* Name, const uint64 Value)
{
    Object->SetStringField(Name, LexToString(Value));
}

bool ReadUInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, uint64& OutValue)
{
    OutValue = 0;
    FString Text;
    return Object.IsValid() && Object->TryGetStringField(Name, Text)
        && LexTryParseString(OutValue, *Text);
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& OutValue)
{
    return Object.IsValid() && Object->TryGetStringField(Name, OutValue);
}

bool ReadRequiredBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, bool& OutValue)
{
    return Object.IsValid() && Object->TryGetBoolField(Name, OutValue);
}

bool ReadRequiredDouble(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double& OutValue)
{
    return Object.IsValid() && Object->TryGetNumberField(Name, OutValue) && std::isfinite(OutValue);
}

bool IsWholeUnsignedJsonNumber(const double Value, const double Maximum)
{
    return std::isfinite(Value) && Value >= 0.0 && Value <= Maximum
        && std::floor(Value) == Value;
}

void WriteUsage(TSharedRef<FJsonObject> Object, const FStagedTerrainResourceUsage& Usage)
{
    PutUInt(Object, TEXT("requests"), Usage.Requests);
    PutUInt(Object, TEXT("transferredBytes"), Usage.TransferredBytes);
    PutUInt(Object, TEXT("peakResidentBytes"), Usage.PeakResidentBytes);
    PutUInt(Object, TEXT("scratchBytesPresent"), Usage.ScratchBytesPresent);
    PutUInt(Object, TEXT("stagedTerrainBytesPresent"), Usage.StagedTerrainBytesPresent);
}

bool ReadUsage(const TSharedPtr<FJsonObject>& Object, FStagedTerrainResourceUsage& Usage)
{
    return ReadUInt(Object, TEXT("requests"), Usage.Requests)
        && ReadUInt(Object, TEXT("transferredBytes"), Usage.TransferredBytes)
        && ReadUInt(Object, TEXT("peakResidentBytes"), Usage.PeakResidentBytes)
        && ReadUInt(Object, TEXT("scratchBytesPresent"), Usage.ScratchBytesPresent)
        && ReadUInt(Object, TEXT("stagedTerrainBytesPresent"), Usage.StagedTerrainBytesPresent);
}

TSharedRef<FJsonObject> WriteLimits(const FStagedTerrainAcquisitionLimits& Limits)
{
    TSharedRef<FJsonObject> Object = NewObject();
    PutUInt(Object, TEXT("maxRequests"), Limits.MaxRequests);
    PutUInt(Object, TEXT("maxTransferredBytes"), Limits.MaxTransferredBytes);
    PutUInt(Object, TEXT("maxResidentBytes"), Limits.MaxResidentBytes);
    PutUInt(Object, TEXT("maxScratchStorageBytes"), Limits.MaxScratchStorageBytes);
    PutUInt(Object, TEXT("maxStagedTerrainBytes"), Limits.MaxStagedTerrainBytes);

    TSharedRef<FJsonObject> Sampler = NewObject();
    const FCogTerrainSamplerLimits& S = Limits.Sampler;
    PutUInt(Sampler, TEXT("maxRequests"), S.MaxRequests);
    PutUInt(Sampler, TEXT("maxTransferredBytes"), S.MaxTransferredBytes);
    PutUInt(Sampler, TEXT("maxResidentBytes"), S.MaxResidentBytes);
    PutUInt(Sampler, TEXT("maxOutputSamples"), S.MaxOutputSamples);
    PutUInt(Sampler, TEXT("maxEncodedTileBytes"), S.MaxEncodedTileBytes);
    PutUInt(Sampler, TEXT("maxDecodedTileBytes"), S.MaxDecodedTileBytes);
    const FCogPreflightLimits& P = S.Preflight;
    PutUInt(Sampler, TEXT("preflightMaxObjectBytes"), P.MaxObjectBytes);
    PutUInt(Sampler, TEXT("preflightMaxTransferredBytes"), P.MaxTransferredBytes);
    PutUInt(Sampler, TEXT("preflightMaxRequests"), P.MaxRequests);
    PutUInt(Sampler, TEXT("preflightMaxRangeBytes"), P.MaxRangeBytes);
    PutUInt(Sampler, TEXT("preflightMaxCachedBytes"), P.MaxCachedBytes);
    PutUInt(Sampler, TEXT("preflightRangeBlockBytes"), P.RangeBlockBytes);
    PutUInt(Sampler, TEXT("preflightMaxDimension"), P.MaxDimension);
    PutUInt(Sampler, TEXT("preflightMaxDirectories"), P.MaxDirectories);
    PutUInt(Sampler, TEXT("preflightMaxTilesPerDirectory"), P.MaxTilesPerDirectory);
    PutUInt(Sampler, TEXT("preflightMaxTotalTiles"), P.MaxTotalTiles);
    PutUInt(Sampler, TEXT("preflightExpectedTileWidth"), P.ExpectedTileWidth);
    PutUInt(Sampler, TEXT("preflightExpectedTileHeight"), P.ExpectedTileHeight);
    Object->SetObjectField(TEXT("sampler"), Sampler);
    return Object;
}

bool ReadLimits(const TSharedPtr<FJsonObject>& Object, FStagedTerrainAcquisitionLimits& Limits)
{
    if (!ReadUInt(Object, TEXT("maxRequests"), Limits.MaxRequests)
        || !ReadUInt(Object, TEXT("maxTransferredBytes"), Limits.MaxTransferredBytes)
        || !ReadUInt(Object, TEXT("maxResidentBytes"), Limits.MaxResidentBytes)
        || !ReadUInt(Object, TEXT("maxScratchStorageBytes"), Limits.MaxScratchStorageBytes)
        || !ReadUInt(Object, TEXT("maxStagedTerrainBytes"), Limits.MaxStagedTerrainBytes)) return false;

    const TSharedPtr<FJsonObject>* SamplerPtr = nullptr;
    if (!Object->TryGetObjectField(TEXT("sampler"), SamplerPtr) || !SamplerPtr || !SamplerPtr->IsValid())
        return false;
    const TSharedPtr<FJsonObject> Sampler = *SamplerPtr;
    FCogTerrainSamplerLimits& S = Limits.Sampler;
    uint64 Value = 0;
#define READ_SAMPLER_UINT(Field) \
    do { if (!ReadUInt(Sampler, TEXT(#Field), Value)) return false; S.Field = Value; } while (false)
    READ_SAMPLER_UINT(MaxRequests);
    READ_SAMPLER_UINT(MaxTransferredBytes);
    READ_SAMPLER_UINT(MaxResidentBytes);
    READ_SAMPLER_UINT(MaxOutputSamples);
    READ_SAMPLER_UINT(MaxEncodedTileBytes);
    READ_SAMPLER_UINT(MaxDecodedTileBytes);
#undef READ_SAMPLER_UINT
    FCogPreflightLimits& P = S.Preflight;
#define READ_PREFLIGHT_UINT(JsonName, Field) \
    do { if (!ReadUInt(Sampler, TEXT(JsonName), Value)) return false; P.Field = static_cast<decltype(P.Field)>(Value); } while (false)
    READ_PREFLIGHT_UINT("preflightMaxObjectBytes", MaxObjectBytes);
    READ_PREFLIGHT_UINT("preflightMaxTransferredBytes", MaxTransferredBytes);
    READ_PREFLIGHT_UINT("preflightMaxRequests", MaxRequests);
    READ_PREFLIGHT_UINT("preflightMaxRangeBytes", MaxRangeBytes);
    READ_PREFLIGHT_UINT("preflightMaxCachedBytes", MaxCachedBytes);
    READ_PREFLIGHT_UINT("preflightRangeBlockBytes", RangeBlockBytes);
    READ_PREFLIGHT_UINT("preflightMaxDimension", MaxDimension);
    READ_PREFLIGHT_UINT("preflightMaxDirectories", MaxDirectories);
    READ_PREFLIGHT_UINT("preflightMaxTilesPerDirectory", MaxTilesPerDirectory);
    READ_PREFLIGHT_UINT("preflightMaxTotalTiles", MaxTotalTiles);
    READ_PREFLIGHT_UINT("preflightExpectedTileWidth", ExpectedTileWidth);
    READ_PREFLIGHT_UINT("preflightExpectedTileHeight", ExpectedTileHeight);
#undef READ_PREFLIGHT_UINT
    return true;
}

TSharedRef<FJsonObject> WriteSelectedSource(const FStagedTerrainSelectedSource& Source)
{
    TSharedRef<FJsonObject> Object = NewObject();
    Object->SetNumberField(TEXT("product"), static_cast<int32>(Source.Candidate.Product));
    Object->SetStringField(TEXT("sourceId"), UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()));
    Object->SetBoolField(TEXT("supportedHorizontalCrs"), Source.Candidate.SupportedHorizontalCrs);
    Object->SetBoolField(TEXT("navd88Proven"), Source.Candidate.Navd88Proven);
    Object->SetBoolField(TEXT("supportedEncoding"), Source.Candidate.SupportedEncoding);
    Object->SetNumberField(TEXT("qualityLevel"), Source.Candidate.QualityLevel);
    Object->SetStringField(TEXT("collectionEndDate"), UTF8_TO_TCHAR(Source.Candidate.CollectionEndDate.c_str()));
    Object->SetStringField(TEXT("publicationDate"), UTF8_TO_TCHAR(Source.Candidate.PublicationDate.c_str()));
    Object->SetStringField(TEXT("downloadUrl"), Source.DownloadUrl);
    Object->SetStringField(TEXT("horizontalCrs"), Source.HorizontalCrs);
    Object->SetStringField(TEXT("verticalDatum"), Source.VerticalDatum);
    Object->SetBoolField(TEXT("siteCoverageVerified"), Source.bSiteCoverageVerified);
    Object->SetStringField(TEXT("coverageEvidenceId"), Source.CoverageEvidenceId);
    return Object;
}

bool ReadSelectedSource(const TSharedPtr<FJsonObject>& Object, FStagedTerrainSelectedSource& Source)
{
    double Product = -1.0;
    double Quality = -1.0;
    if (!Object.IsValid() || !Object->TryGetNumberField(TEXT("product"), Product)
        || !IsWholeUnsignedJsonNumber(Product, static_cast<double>(SkiDomain::ElevationProduct::ArcSec13))
        || Product < static_cast<double>(SkiDomain::ElevationProduct::S1M)
        || !Object->TryGetNumberField(TEXT("qualityLevel"), Quality)
        || !IsWholeUnsignedJsonNumber(Quality, 255.0))
        return false;
    Source.Candidate.Product = static_cast<SkiDomain::ElevationProduct>(static_cast<uint8>(Product));
    Source.Candidate.QualityLevel = static_cast<uint8>(Quality);
    FString Text;
    if (!ReadRequiredString(Object, TEXT("sourceId"), Text)) return false;
    Source.Candidate.SourceId = TCHAR_TO_UTF8(*Text);
    if (!ReadRequiredBool(Object, TEXT("supportedHorizontalCrs"), Source.Candidate.SupportedHorizontalCrs)
        || !ReadRequiredBool(Object, TEXT("navd88Proven"), Source.Candidate.Navd88Proven)
        || !ReadRequiredBool(Object, TEXT("supportedEncoding"), Source.Candidate.SupportedEncoding)
        || !ReadRequiredString(Object, TEXT("collectionEndDate"), Text)) return false;
    Source.Candidate.CollectionEndDate = TCHAR_TO_UTF8(*Text);
    if (!ReadRequiredString(Object, TEXT("publicationDate"), Text)) return false;
    Source.Candidate.PublicationDate = TCHAR_TO_UTF8(*Text);
    return ReadRequiredString(Object, TEXT("downloadUrl"), Source.DownloadUrl)
        && ReadRequiredString(Object, TEXT("horizontalCrs"), Source.HorizontalCrs)
        && ReadRequiredString(Object, TEXT("verticalDatum"), Source.VerticalDatum)
        && ReadRequiredBool(Object, TEXT("siteCoverageVerified"), Source.bSiteCoverageVerified)
        && ReadRequiredString(Object, TEXT("coverageEvidenceId"), Source.CoverageEvidenceId);
}

TSharedRef<FJsonObject> WritePin(const FStagedTerrainObjectPin& Pin)
{
    TSharedRef<FJsonObject> Object = NewObject();
    Object->SetStringField(TEXT("product"), Pin.ProductCode);
    Object->SetStringField(TEXT("sourceId"), Pin.SourceId);
    Object->SetStringField(TEXT("url"), Pin.Url);
    Object->SetStringField(TEXT("etag"), Pin.ETag);
    PutUInt(Object, TEXT("objectBytes"), Pin.ObjectBytes);
    return Object;
}

bool ReadPin(const TSharedPtr<FJsonObject>& Object, FStagedTerrainObjectPin& Pin)
{
    return ReadRequiredString(Object, TEXT("product"), Pin.ProductCode)
        && ReadRequiredString(Object, TEXT("sourceId"), Pin.SourceId)
        && ReadRequiredString(Object, TEXT("url"), Pin.Url)
        && ReadRequiredString(Object, TEXT("etag"), Pin.ETag)
        && ReadUInt(Object, TEXT("objectBytes"), Pin.ObjectBytes);
}

bool ReadObjectArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key,
    TArray<TSharedPtr<FJsonValue>>& OutValues)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(Key, Values) || !Values) return false;
    OutValues = *Values;
    return true;
}

}

bool SkiPreparation::FStagedTerrainAcquisitionJob::Create(
    const FStagedTerrainAcquisitionRequest& Request,
    FStagedTerrainAcquisitionJob& OutJob, FString& OutError)
{
    OutError.Reset();
    FStagedTerrainAcquisitionReceipt Receipt;
    Receipt.Request = Request;
    if (Receipt.Request.JobId.IsEmpty()) Receipt.Request.JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FStagedTerrainAcquisitionJob Candidate(MoveTemp(Receipt));
    if (!Candidate.ValidateReceipt(OutError)) return false;
    OutJob = MoveTemp(Candidate);
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::Restore(
    const FStagedTerrainAcquisitionReceipt& Receipt,
    FStagedTerrainAcquisitionJob& OutJob, FString& OutError)
{
    OutError.Reset();
    FStagedTerrainAcquisitionJob Candidate{FStagedTerrainAcquisitionReceipt(Receipt)};
    if (!Candidate.ValidateReceipt(OutError)) return false;
    OutJob = MoveTemp(Candidate);
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::ValidateReceipt(FString& OutError) const
{
    OutError.Reset();
    const FStagedTerrainAcquisitionRequest& Request = Receipt.Request;
    if (Receipt.SchemaVersion != ReceiptSchemaVersion || Request.JobId.IsEmpty()
        || Request.JobId.Len() > 128 || !IsFiniteBounds(Request.Bounds))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_INVALID");
        return false;
    }
    uint64 RequiredScratch = 0;
    if (!TerrainScratchStore::TryCalculateRequiredStorageBytes(Request.Width, Request.Height,
            RequiredScratch)
        || static_cast<uint64>(Request.Width) * Request.Height > Request.Limits.Sampler.MaxOutputSamples
        || RequiredScratch > Request.Limits.MaxScratchStorageBytes
        || Request.Limits.MaxRequests == 0 || Request.Limits.MaxTransferredBytes == 0
        || Request.Limits.MaxResidentBytes == 0 || Request.Limits.MaxStagedTerrainBytes == 0
        || Request.Limits.Sampler.MaxRequests == 0
        || Request.Limits.Sampler.MaxRequests > Request.Limits.MaxRequests
        || Request.Limits.Sampler.MaxTransferredBytes == 0
        || Request.Limits.Sampler.MaxTransferredBytes > Request.Limits.MaxTransferredBytes
        || Request.Limits.Sampler.MaxResidentBytes == 0
        || Request.Limits.Sampler.MaxResidentBytes > Request.Limits.MaxResidentBytes
        || Request.Limits.Sampler.MaxResidentBytes <= 258ULL * 258ULL * TerrainScratchBytesPerSample
        || Request.Limits.Sampler.Preflight.MaxCachedBytes > Request.Limits.Sampler.MaxResidentBytes
        || Request.Limits.Sampler.Preflight.RangeBlockBytes == 0
        || Request.Limits.Sampler.Preflight.MaxRangeBytes < Request.Limits.Sampler.Preflight.RangeBlockBytes)
    {
        OutError = TEXT("STAGED_TERRAIN_RESOURCE_LIMIT_INVALID");
        return false;
    }
    if (Receipt.NextStage > EStagedTerrainStage::Complete
        || Receipt.State > EStagedTerrainJobState::Complete
        || Receipt.bLibraryEntryWritten || Receipt.bMountainTransitioned)
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_STATE_INVALID");
        return false;
    }
    const FStagedTerrainAcquisitionLimits& Limits = Request.Limits;
    if (Receipt.Usage.Requests > Limits.MaxRequests
        || Receipt.Usage.TransferredBytes > Limits.MaxTransferredBytes
        || Receipt.Usage.PeakResidentBytes > Limits.MaxResidentBytes
        || Receipt.Usage.ScratchBytesPresent > Limits.MaxScratchStorageBytes
        || Receipt.Usage.StagedTerrainBytesPresent > Limits.MaxStagedTerrainBytes)
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_USAGE_INVALID");
        return false;
    }
    if (Receipt.CompletedStages.Num() > static_cast<int32>(EStagedTerrainStage::TerrainCoreVerification) + 1)
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_STAGE_COUNT_INVALID");
        return false;
    }
    for (int32 Index = 0; Index < Receipt.CompletedStages.Num(); ++Index)
    {
        const FStagedTerrainCompletedStage& Completed = Receipt.CompletedStages[Index];
        if (static_cast<uint8>(Completed.Stage) != static_cast<uint8>(Index)
            || Completed.Stage == EStagedTerrainStage::Complete || !IsSha256(Completed.OutputSha256)
            || (Completed.TotalUnits != 0 && Completed.CompletedUnits > Completed.TotalUnits))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_STAGE_ORDER_INVALID");
            return false;
        }
    }
    const bool bCatalogComplete = HasCompleted(EStagedTerrainStage::Catalog);
    if (bCatalogComplete != !Receipt.SelectedSources.IsEmpty())
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_SELECTION_INCONSISTENT");
        return false;
    }
    int32 PreviousPriority = -1;
    TSet<FString> SeenSourceIds;
    for (const FStagedTerrainSelectedSource& Source : Receipt.SelectedSources)
    {
        const FString SourceId = UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str());
        const int32 Priority = ProductPriority(Source.Candidate.Product);
        FString UrlReason;
        if (SourceId.IsEmpty() || ProductCode(Source.Candidate.Product).IsEmpty()
            || !Source.Candidate.SupportedHorizontalCrs || !Source.Candidate.Navd88Proven
            || !Source.Candidate.SupportedEncoding || Source.HorizontalCrs.IsEmpty()
            || Source.VerticalDatum.IsEmpty() || !Source.bSiteCoverageVerified
            || Source.CoverageEvidenceId.IsEmpty() || Priority < PreviousPriority
            || SeenSourceIds.Contains(SourceId) || !SkiNetGateway::ValidateUrl(Source.DownloadUrl, UrlReason))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_SOURCE_INVALID");
            return false;
        }
        SeenSourceIds.Add(SourceId);
        PreviousPriority = Priority;
    }
    TSet<FString> SeenPins;
    for (const FStagedTerrainObjectPin& Pin : Receipt.PinnedObjects)
    {
        FString UrlReason;
        const FString PinKey = Pin.ProductCode + TEXT("\n") + Pin.SourceId;
        if (Pin.ProductCode.IsEmpty() || Pin.SourceId.IsEmpty() || Pin.ObjectBytes == 0
            || !IsStrongETag(Pin.ETag) || !SkiNetGateway::ValidateUrl(Pin.Url, UrlReason)
            || SeenPins.Contains(PinKey))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_PIN_INVALID");
            return false;
        }
        SeenPins.Add(PinKey);
    }
    if (HasCompleted(EStagedTerrainStage::SourcePreflight))
    {
        for (const FStagedTerrainSelectedSource& Source : Receipt.SelectedSources)
        {
            const FString SourceId = UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str());
            if (!Receipt.PinnedObjects.ContainsByPredicate([&SourceId, &Source](const FStagedTerrainObjectPin& Pin)
                { return Pin.SourceId == SourceId && Pin.ProductCode == ProductCode(Source.Candidate.Product)
                    && Pin.Url == Source.DownloadUrl; }))
            {
                OutError = TEXT("STAGED_TERRAIN_RECEIPT_ELEVATION_PIN_MISSING");
                return false;
            }
        }
    }
    const EStagedTerrainStage ExpectedStage = static_cast<EStagedTerrainStage>(Receipt.CompletedStages.Num());
    if (Receipt.NextStage != ExpectedStage
        || Receipt.bTerrainCoreStaged != HasCompleted(EStagedTerrainStage::TerrainCoreStaging)
        || Receipt.bTerrainCoreVerified != HasCompleted(EStagedTerrainStage::TerrainCoreVerification)
        || (Receipt.State == EStagedTerrainJobState::Complete
            && Receipt.NextStage != EStagedTerrainStage::Complete)
        || (Receipt.State != EStagedTerrainJobState::Complete
            && Receipt.NextStage == EStagedTerrainStage::Complete))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_CHECKPOINT_INCONSISTENT");
        return false;
    }
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::HasCompleted(
    const EStagedTerrainStage Stage) const noexcept
{
    for (const FStagedTerrainCompletedStage& Completed : Receipt.CompletedStages)
        if (Completed.Stage == Stage) return true;
    return false;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::RememberPins(
    const TArray<FStagedTerrainObjectPin>& Pins, FString& OutError)
{
    OutError.Reset();
    for (const FStagedTerrainObjectPin& Pin : Pins)
    {
        FString UrlReason;
        if (Pin.ProductCode.IsEmpty() || Pin.SourceId.IsEmpty() || Pin.ObjectBytes == 0
            || !IsStrongETag(Pin.ETag) || !SkiNetGateway::ValidateUrl(Pin.Url, UrlReason))
        {
            OutError = TEXT("STAGED_TERRAIN_OBJECT_PIN_INVALID");
            return false;
        }
        int32 FoundIndex = INDEX_NONE;
        for (int32 Index = 0; Index < Receipt.PinnedObjects.Num(); ++Index)
        {
            const FStagedTerrainObjectPin& Existing = Receipt.PinnedObjects[Index];
            if (Existing.ProductCode == Pin.ProductCode && Existing.SourceId == Pin.SourceId)
            {
                FoundIndex = Index;
                break;
            }
        }
        if (FoundIndex != INDEX_NONE)
        {
            const FStagedTerrainObjectPin& Existing = Receipt.PinnedObjects[FoundIndex];
            if (Existing.Url != Pin.Url || Existing.ETag != Pin.ETag
                || Existing.ObjectBytes != Pin.ObjectBytes)
            {
                OutError = TEXT("STAGED_TERRAIN_ETAG_OR_OBJECT_IDENTITY_CHANGED");
                return false;
            }
        }
        else Receipt.PinnedObjects.Add(Pin);
    }
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::CheckResourceUsage(
    const FStagedTerrainResourceUsage& Added, FString& OutError)
{
    OutError.Reset();
    const FStagedTerrainAcquisitionLimits& Limits = Receipt.Request.Limits;
    if (Added.Requests > Limits.MaxRequests - FMath::Min(Receipt.Usage.Requests, Limits.MaxRequests)
        || Added.TransferredBytes > Limits.MaxTransferredBytes
            - FMath::Min(Receipt.Usage.TransferredBytes, Limits.MaxTransferredBytes)
        || Added.PeakResidentBytes > Limits.MaxResidentBytes
        || Added.ScratchBytesPresent > Limits.MaxScratchStorageBytes
        || Added.StagedTerrainBytesPresent > Limits.MaxStagedTerrainBytes)
    {
        OutError = TEXT("STAGED_TERRAIN_RESOURCE_BUDGET_EXCEEDED");
        return false;
    }
    Receipt.Usage.Requests += Added.Requests;
    Receipt.Usage.TransferredBytes += Added.TransferredBytes;
    Receipt.Usage.PeakResidentBytes = FMath::Max(Receipt.Usage.PeakResidentBytes, Added.PeakResidentBytes);
    Receipt.Usage.ScratchBytesPresent = FMath::Max(Receipt.Usage.ScratchBytesPresent, Added.ScratchBytesPresent);
    Receipt.Usage.StagedTerrainBytesPresent = FMath::Max(Receipt.Usage.StagedTerrainBytesPresent,
        Added.StagedTerrainBytesPresent);
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::ValidateStageResult(
    const EStagedTerrainStage Stage, const FStagedTerrainStageResult& Result,
    FString& OutError)
{
    OutError.Reset();
    if (Result.Outcome != EStagedTerrainStageOutcome::Succeeded)
    {
        OutError = Result.FailureCode.IsEmpty() ? TEXT("STAGED_TERRAIN_STAGE_DID_NOT_SUCCEED") : Result.FailureCode;
        return false;
    }
    if (Stage == EStagedTerrainStage::Complete || !IsSha256(Result.OutputSha256)
        || (Result.TotalUnits != 0 && Result.CompletedUnits > Result.TotalUnits)
        || Result.Proof.bLibraryEntryWritten || Result.Proof.bMountainTransitioned)
    {
        OutError = TEXT("STAGED_TERRAIN_STAGE_RECEIPT_INVALID");
        return false;
    }

    if (!RememberPins(Result.ObjectPins, OutError)) return false;

    switch (Stage)
    {
    case EStagedTerrainStage::Catalog:
    {
        if (!Result.Proof.bSupportedGeography || !Result.Proof.bSiteCoverageVerified
            || Result.SelectedSources.IsEmpty())
        {
            OutError = TEXT("STAGED_TERRAIN_CATALOG_COVERAGE_NOT_PROVEN");
            return false;
        }
        int32 PreviousPriority = -1;
        TSet<FString> SeenIds;
        for (const FStagedTerrainSelectedSource& Source : Result.SelectedSources)
        {
            const int32 Priority = ProductPriority(Source.Candidate.Product);
            FString UrlReason;
            const FString Product = ProductCode(Source.Candidate.Product);
            if (Product.IsEmpty() || Source.Candidate.SourceId.empty()
                || !Source.Candidate.SupportedHorizontalCrs || !Source.Candidate.Navd88Proven
                || !Source.Candidate.SupportedEncoding || Source.HorizontalCrs.IsEmpty()
                || Source.VerticalDatum.IsEmpty() || !Source.bSiteCoverageVerified
                || Source.CoverageEvidenceId.IsEmpty() || !SkiNetGateway::ValidateUrl(Source.DownloadUrl, UrlReason)
                || SeenIds.Contains(UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()))
                || Priority < PreviousPriority)
            {
                OutError = TEXT("STAGED_TERRAIN_SELECTED_SOURCE_PROOF_INVALID");
                return false;
            }
            SeenIds.Add(UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()));
            PreviousPriority = Priority;
        }
        Receipt.SelectedSources = Result.SelectedSources;
        break;
    }
    case EStagedTerrainStage::SourcePreflight:
    {
        if (Receipt.SelectedSources.IsEmpty()
            || Result.CogObservations.Num() != Receipt.SelectedSources.Num())
        {
            OutError = TEXT("STAGED_TERRAIN_SOURCE_PREFLIGHT_INCOMPLETE");
            return false;
        }
        for (const FStagedTerrainSelectedSource& Selected : Receipt.SelectedSources)
        {
            const FString SourceId = UTF8_TO_TCHAR(Selected.Candidate.SourceId.c_str());
            const FStagedTerrainCogObservation* Observation = Result.CogObservations.FindByPredicate(
                [&SourceId](const FStagedTerrainCogObservation& Item) { return Item.SourceId == SourceId; });
            if (!Observation || Observation->Url != Selected.DownloadUrl
                || !Observation->Preflight.bPassed || !Observation->Preflight.bStrongETagPinned
                || Observation->Preflight.ObjectBytes == 0 || !IsStrongETag(Observation->StrongETag)
                || Observation->Preflight.HorizontalCrs != Selected.HorizontalCrs
                || !Observation->Preflight.VerticalDatum.Contains(TEXT("NAVD"), ESearchCase::IgnoreCase))
            {
                OutError = TEXT("STAGED_TERRAIN_SOURCE_PREFLIGHT_PROOF_INVALID");
                return false;
            }
            const FStagedTerrainObjectPin* Pin = Result.ObjectPins.FindByPredicate(
                [&SourceId](const FStagedTerrainObjectPin& Item) { return Item.SourceId == SourceId; });
            if (!Pin || Pin->ProductCode != ProductCode(Selected.Candidate.Product)
                || Pin->Url != Observation->Url || Pin->ETag != Observation->StrongETag
                || Pin->ObjectBytes != Observation->Preflight.ObjectBytes)
            {
                OutError = TEXT("STAGED_TERRAIN_SOURCE_ETAG_PIN_MISSING");
                return false;
            }
        }
        break;
    }
    case EStagedTerrainStage::CanonicalLod0:
        if (!HasCompleted(EStagedTerrainStage::SourcePreflight)
            || !Result.Proof.bCanonicalLod0IsOneMetre || !Result.Proof.bCanonicalLod0StoreSealed
            || Result.Proof.CanonicalLod0Samples
                != static_cast<uint64>(Receipt.Request.Width) * Receipt.Request.Height)
        {
            OutError = TEXT("STAGED_TERRAIN_CANONICAL_LOD0_PROOF_INVALID");
            return false;
        }
        for (const FStagedTerrainSelectedSource& Selected : Receipt.SelectedSources)
        {
            const FString SourceId = UTF8_TO_TCHAR(Selected.Candidate.SourceId.c_str());
            if (!Result.ObjectPins.ContainsByPredicate([&SourceId](const FStagedTerrainObjectPin& Item)
                { return Item.SourceId == SourceId; }))
            {
                OutError = TEXT("STAGED_TERRAIN_SAMPLER_ETAG_NOT_REPORTED");
                return false;
            }
        }
        break;
    case EStagedTerrainStage::CoarseLodCopy:
        if (!HasCompleted(EStagedTerrainStage::CanonicalLod0)
            || !Result.Proof.bCoarseLodsAreIndexedCopies || !Result.Proof.bCoarseLodsBitExactVerified)
        {
            OutError = TEXT("STAGED_TERRAIN_COARSE_LOD_COPY_PROOF_INVALID");
            return false;
        }
        break;
    case EStagedTerrainStage::WorldCover:
        if (!Result.Proof.bWorldCoverWindowVerified
            || !Result.ObjectPins.ContainsByPredicate([](const FStagedTerrainObjectPin& Pin)
                { return Pin.ProductCode == TEXT("WorldCover"); }))
        {
            OutError = TEXT("STAGED_TERRAIN_WORLDCOVER_PROOF_INVALID");
            return false;
        }
        break;
    case EStagedTerrainStage::TerrainCoreStaging:
        if (!Result.Proof.bTerrainCoreStagedOnly)
        {
            OutError = TEXT("STAGED_TERRAIN_CORE_WAS_NOT_STAGED_ONLY");
            return false;
        }
        Receipt.bTerrainCoreStaged = true;
        break;
    case EStagedTerrainStage::TerrainCoreVerification:
        if (!Receipt.bTerrainCoreStaged || !Result.Proof.bTerrainCoreVerificationPassed)
        {
            OutError = TEXT("STAGED_TERRAIN_CORE_VERIFICATION_FAILED");
            return false;
        }
        Receipt.bTerrainCoreVerified = true;
        break;
    default:
        OutError = TEXT("STAGED_TERRAIN_STAGE_UNSUPPORTED");
        return false;
    }

    if (Receipt.bLibraryEntryWritten || Receipt.bMountainTransitioned)
    {
        OutError = TEXT("STAGED_TERRAIN_INSTALLATION_FORBIDDEN_IN_M4");
        return false;
    }
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::Run(
    IStagedTerrainAcquisitionAdapter& Adapter,
    const TSharedRef<Cancellation>& Cancellation, const uint32 MaxStages,
    FString& OutError)
{
    OutError.Reset();
    if (MaxStages == 0 || Receipt.State == EStagedTerrainJobState::Failed
        || Receipt.State == EStagedTerrainJobState::Complete
        || !ValidateReceipt(OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("STAGED_TERRAIN_JOB_NOT_RUNNABLE");
        return false;
    }
    Receipt.State = EStagedTerrainJobState::Running;
    uint32 Calls = 0;
    while (Receipt.NextStage != EStagedTerrainStage::Complete && Calls < MaxStages)
    {
        if (Cancellation->IsCancelled())
        {
            Receipt.State = EStagedTerrainJobState::Paused;
            OutError = TEXT("STAGED_TERRAIN_CANCELLED");
            return false;
        }
        const EStagedTerrainStage Stage = Receipt.NextStage;
        SkiNetGateway Gateway;
        FStagedTerrainStageResult Result = Adapter.RunStage(Stage, Receipt.Request, Receipt,
            Gateway, Cancellation, Receipt.CheckpointToken);
        ++Calls;
        if (!RememberPins(Result.ObjectPins, OutError))
        {
            Receipt.State = EStagedTerrainJobState::Failed;
            Receipt.FailureCode = OutError;
            Receipt.CheckpointToken = Result.CheckpointToken;
            return false;
        }
        if (!CheckResourceUsage(Result.Usage, OutError))
        {
            Receipt.State = EStagedTerrainJobState::Failed;
            Receipt.FailureCode = OutError;
            Receipt.FailureDetail = TEXT("The adapter reported stage usage above the job resource caps.");
            Receipt.CheckpointToken = Result.CheckpointToken;
            return false;
        }
        if (Result.Outcome == EStagedTerrainStageOutcome::Cancelled
            || Result.Outcome == EStagedTerrainStageOutcome::RetryableFailure)
        {
            Receipt.State = EStagedTerrainJobState::Paused;
            Receipt.CheckpointToken = Result.CheckpointToken;
            Receipt.FailureCode = Result.FailureCode;
            Receipt.FailureDetail = Result.FailureDetail;
            OutError = Result.FailureCode.IsEmpty() ? TEXT("STAGED_TERRAIN_STAGE_PAUSED") : Result.FailureCode;
            return false;
        }
        if (Result.Outcome == EStagedTerrainStageOutcome::FatalFailure)
        {
            Receipt.State = EStagedTerrainJobState::Failed;
            Receipt.CheckpointToken = Result.CheckpointToken;
            Receipt.FailureCode = Result.FailureCode.IsEmpty()
                ? TEXT("STAGED_TERRAIN_STAGE_FAILED") : Result.FailureCode;
            Receipt.FailureDetail = Result.FailureDetail;
            OutError = Receipt.FailureCode;
            return false;
        }
        if (!ValidateStageResult(Stage, Result, OutError))
        {
            Receipt.State = EStagedTerrainJobState::Failed;
            Receipt.FailureCode = OutError;
            Receipt.FailureDetail = Result.FailureDetail;
            Receipt.CheckpointToken = Result.CheckpointToken;
            return false;
        }

        FStagedTerrainCompletedStage Completed;
        Completed.Stage = Stage;
        Completed.OutputSha256 = Result.OutputSha256;
        Completed.CompletedUnits = Result.CompletedUnits;
        Completed.TotalUnits = Result.TotalUnits;
        Receipt.CompletedStages.Add(MoveTemp(Completed));
        Receipt.CheckpointToken.Reset();
        Receipt.FailureCode.Reset();
        Receipt.FailureDetail.Reset();
        Receipt.NextStage = static_cast<EStagedTerrainStage>(static_cast<uint8>(Stage) + 1U);
        if (Cancellation->IsCancelled())
        {
            Receipt.State = EStagedTerrainJobState::Paused;
            OutError = TEXT("STAGED_TERRAIN_CANCELLED");
            return false;
        }
    }

    if (Receipt.NextStage == EStagedTerrainStage::Complete)
    {
        if (!Receipt.bTerrainCoreStaged || !Receipt.bTerrainCoreVerified
            || Receipt.bLibraryEntryWritten || Receipt.bMountainTransitioned)
        {
            Receipt.State = EStagedTerrainJobState::Failed;
            OutError = TEXT("STAGED_TERRAIN_COMPLETION_INVARIANT_FAILED");
            Receipt.FailureCode = OutError;
            return false;
        }
        Receipt.State = EStagedTerrainJobState::Complete;
        return true;
    }

    Receipt.State = EStagedTerrainJobState::Paused;
    OutError = TEXT("STAGED_TERRAIN_STAGE_CALL_BUDGET_REACHED");
    return false;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::SerializeReceipt(
    const FStagedTerrainAcquisitionReceipt& Receipt, FString& OutJson, FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();
    FStagedTerrainAcquisitionJob Validated;
    if (!Restore(Receipt, Validated, OutError)) return false;

    TSharedRef<FJsonObject> Root = NewObject();
    Root->SetNumberField(TEXT("schemaVersion"), Receipt.SchemaVersion);
    Root->SetStringField(TEXT("state"), StagedJobStateName(Receipt.State));
    Root->SetStringField(TEXT("nextStage"), StageName(Receipt.NextStage));
    Root->SetStringField(TEXT("checkpointToken"), Receipt.CheckpointToken);
    Root->SetStringField(TEXT("failureCode"), Receipt.FailureCode);
    Root->SetStringField(TEXT("failureDetail"), Receipt.FailureDetail);
    Root->SetBoolField(TEXT("terrainCoreStaged"), Receipt.bTerrainCoreStaged);
    Root->SetBoolField(TEXT("terrainCoreVerified"), Receipt.bTerrainCoreVerified);
    Root->SetBoolField(TEXT("libraryEntryWritten"), Receipt.bLibraryEntryWritten);
    Root->SetBoolField(TEXT("mountainTransitioned"), Receipt.bMountainTransitioned);
    WriteUsage(Root, Receipt.Usage);

    TSharedRef<FJsonObject> Request = NewObject();
    Request->SetStringField(TEXT("jobId"), Receipt.Request.JobId);
    Request->SetNumberField(TEXT("west"), Receipt.Request.Bounds.WestDeg);
    Request->SetNumberField(TEXT("south"), Receipt.Request.Bounds.SouthDeg);
    Request->SetNumberField(TEXT("east"), Receipt.Request.Bounds.EastDeg);
    Request->SetNumberField(TEXT("north"), Receipt.Request.Bounds.NorthDeg);
    PutUInt(Request, TEXT("width"), Receipt.Request.Width);
    PutUInt(Request, TEXT("height"), Receipt.Request.Height);
    Request->SetObjectField(TEXT("limits"), WriteLimits(Receipt.Request.Limits));
    Root->SetObjectField(TEXT("request"), Request);

    TArray<TSharedPtr<FJsonValue>> Sources;
    for (const FStagedTerrainSelectedSource& Source : Receipt.SelectedSources)
        Sources.Add(MakeShared<FJsonValueObject>(WriteSelectedSource(Source)));
    Root->SetArrayField(TEXT("selectedSources"), Sources);
    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const FStagedTerrainObjectPin& Pin : Receipt.PinnedObjects)
        Pins.Add(MakeShared<FJsonValueObject>(WritePin(Pin)));
    Root->SetArrayField(TEXT("pinnedObjects"), Pins);

    TArray<TSharedPtr<FJsonValue>> Completed;
    for (const FStagedTerrainCompletedStage& Item : Receipt.CompletedStages)
    {
        TSharedRef<FJsonObject> Object = NewObject();
        Object->SetStringField(TEXT("stage"), StageName(Item.Stage));
        Object->SetStringField(TEXT("sha256"), Item.OutputSha256);
        PutUInt(Object, TEXT("completedUnits"), Item.CompletedUnits);
        PutUInt(Object, TEXT("totalUnits"), Item.TotalUnits);
        Completed.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("completedStages"), Completed);

    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        OutJson.Reset();
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_SERIALIZE_FAILED");
        return false;
    }
    return true;
}

bool SkiPreparation::FStagedTerrainAcquisitionJob::DeserializeReceipt(
    const FString& Json, FStagedTerrainAcquisitionReceipt& OutReceipt, FString& OutError)
{
    OutError.Reset();
    OutReceipt = {};
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_JSON_INVALID");
        return false;
    }
    double Schema = 0.0;
    FString StateText, StageText;
    if (!Root->TryGetNumberField(TEXT("schemaVersion"), Schema)
        || !IsWholeUnsignedJsonNumber(Schema, static_cast<double>(MAX_uint32))
        || !ReadRequiredString(Root, TEXT("state"), StateText)
        || !ReadRequiredString(Root, TEXT("nextStage"), StageText)
        || !ParseState(StateText, OutReceipt.State) || !ParseStage(StageText, OutReceipt.NextStage))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_HEADER_INVALID");
        return false;
    }
    OutReceipt.SchemaVersion = static_cast<uint32>(Schema);
    if (!ReadRequiredString(Root, TEXT("checkpointToken"), OutReceipt.CheckpointToken)
        || !ReadRequiredString(Root, TEXT("failureCode"), OutReceipt.FailureCode)
        || !ReadRequiredString(Root, TEXT("failureDetail"), OutReceipt.FailureDetail)
        || !ReadRequiredBool(Root, TEXT("terrainCoreStaged"), OutReceipt.bTerrainCoreStaged)
        || !ReadRequiredBool(Root, TEXT("terrainCoreVerified"), OutReceipt.bTerrainCoreVerified)
        || !ReadRequiredBool(Root, TEXT("libraryEntryWritten"), OutReceipt.bLibraryEntryWritten)
        || !ReadRequiredBool(Root, TEXT("mountainTransitioned"), OutReceipt.bMountainTransitioned)
        || !ReadUsage(Root, OutReceipt.Usage))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_FIELDS_INVALID");
        return false;
    }

    const TSharedPtr<FJsonObject>* RequestPtr = nullptr;
    if (!Root->TryGetObjectField(TEXT("request"), RequestPtr) || !RequestPtr || !RequestPtr->IsValid())
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_REQUEST_MISSING");
        return false;
    }
    const TSharedPtr<FJsonObject> Request = *RequestPtr;
    FStagedTerrainAcquisitionRequest& R = OutReceipt.Request;
    uint64 Width = 0, Height = 0;
    const TSharedPtr<FJsonObject>* LimitsPtr = nullptr;
    if (!ReadRequiredString(Request, TEXT("jobId"), R.JobId)
        || !ReadRequiredDouble(Request, TEXT("west"), R.Bounds.WestDeg)
        || !ReadRequiredDouble(Request, TEXT("south"), R.Bounds.SouthDeg)
        || !ReadRequiredDouble(Request, TEXT("east"), R.Bounds.EastDeg)
        || !ReadRequiredDouble(Request, TEXT("north"), R.Bounds.NorthDeg)
        || !ReadUInt(Request, TEXT("width"), Width) || Width > MAX_uint32
        || !ReadUInt(Request, TEXT("height"), Height) || Height > MAX_uint32
        || !Request->TryGetObjectField(TEXT("limits"), LimitsPtr) || !LimitsPtr || !LimitsPtr->IsValid()
        || !ReadLimits(*LimitsPtr, R.Limits))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_REQUEST_INVALID");
        return false;
    }
    R.Width = static_cast<uint32>(Width);
    R.Height = static_cast<uint32>(Height);

    TArray<TSharedPtr<FJsonValue>> Values;
    if (!ReadObjectArray(Root, TEXT("selectedSources"), Values))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_SOURCES_INVALID");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : Values)
    {
        const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
        FStagedTerrainSelectedSource Source;
        if (!ReadSelectedSource(Object, Source))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_SOURCE_INVALID");
            return false;
        }
        OutReceipt.SelectedSources.Add(MoveTemp(Source));
    }
    if (!ReadObjectArray(Root, TEXT("pinnedObjects"), Values))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_PINS_INVALID");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : Values)
    {
        FStagedTerrainObjectPin Pin;
        if (!ReadPin(Value.IsValid() ? Value->AsObject() : nullptr, Pin))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_PIN_INVALID");
            return false;
        }
        OutReceipt.PinnedObjects.Add(MoveTemp(Pin));
    }
    if (!ReadObjectArray(Root, TEXT("completedStages"), Values))
    {
        OutError = TEXT("STAGED_TERRAIN_RECEIPT_COMPLETED_STAGES_INVALID");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : Values)
    {
        const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
        FStagedTerrainCompletedStage Completed;
        FString CompletedName;
        if (!ReadRequiredString(Object, TEXT("stage"), CompletedName)
            || !ParseStage(CompletedName, Completed.Stage)
            || !ReadRequiredString(Object, TEXT("sha256"), Completed.OutputSha256)
            || !ReadUInt(Object, TEXT("completedUnits"), Completed.CompletedUnits)
            || !ReadUInt(Object, TEXT("totalUnits"), Completed.TotalUnits))
        {
            OutError = TEXT("STAGED_TERRAIN_RECEIPT_COMPLETED_STAGE_INVALID");
            return false;
        }
        OutReceipt.CompletedStages.Add(MoveTemp(Completed));
    }

    FStagedTerrainAcquisitionJob Validated;
    if (!Restore(OutReceipt, Validated, OutError))
    {
        OutReceipt = {};
        return false;
    }
    return true;
}
