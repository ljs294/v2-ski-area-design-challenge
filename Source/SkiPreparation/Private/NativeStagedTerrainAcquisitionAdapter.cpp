#include "SkiPreparation/NativeStagedTerrainAcquisitionAdapter.h"

#include "SkiPreparation/ElevationCatalog.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <limits>

namespace
{
using namespace SkiPreparation;

FString SourceId(const FStagedTerrainSelectedSource& Source)
{
    return UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str());
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

FString ProductCode(const ElevationCatalogSource& Source)
{
    return ProductCode(Source.Candidate.Product);
}

bool IsStrongETag(const FString& Tag)
{
    if (Tag.Len() < 2 || Tag.Len() > 128 || Tag[0] != TEXT('"') || Tag[Tag.Len() - 1] != TEXT('"'))
        return false;
    for (int32 Index = 1; Index < Tag.Len() - 1; ++Index)
    {
        const TCHAR C = Tag[Index];
        if (C < 0x21 || C > 0x7e || C == TEXT('"') || C == TEXT(':') || C == TEXT('\\'))
            return false;
    }
    return true;
}

FString NormalizeToken(const FString& Value)
{
    FString Token;
    Token.Reserve(Value.Len());
    for (const TCHAR Character : Value)
        if (FChar::IsAlnum(Character)) Token.AppendChar(FChar::ToUpper(Character));
    return Token;
}

bool IsNavd88(const FString& Value)
{
    const FString Token = NormalizeToken(Value);
    return Token == TEXT("NAVD88") || Token == TEXT("EPSG5703")
        || Token == TEXT("NORTHAMERICANDATUMOF1988NAVD88")
        || Token == TEXT("NORTHAMERICANDATUM1988NAVD88")
        || Token == TEXT("NORTHAMERICANVERTICALDATUMOF1988NAVD88");
}

bool IsUnprovenCoverageCode(const FString& Value)
{
    return Value.IsEmpty()
        || Value.Contains(TEXT("TNM_BBOX"), ESearchCase::IgnoreCase)
        || Value.Contains(TEXT("UNCHECKED"), ESearchCase::IgnoreCase)
        || Value.Contains(TEXT("NOT_PROVEN"), ESearchCase::IgnoreCase)
        || Value.Contains(TEXT("NOT_VERIFIED"), ESearchCase::IgnoreCase);
}

FString Sha256Text(const FString& Text)
{
    const FTCHARToUTF8 Encoded(*Text);
    return Sha256(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()));
}

FStagedTerrainStageResult Failed(const TCHAR* Code, const FString& Detail,
    const EStagedTerrainStageOutcome Outcome = EStagedTerrainStageOutcome::FatalFailure)
{
    FStagedTerrainStageResult Result;
    Result.Outcome = Outcome;
    Result.FailureCode = Code;
    Result.FailureDetail = Detail;
    return Result;
}

FString DatumFailureDetail(const TerrainAvailabilityReport& Report)
{
    for (const ElevationCatalogSource& Source : Report.Sources)
    {
        if (Source.CogStatusCode.Contains(TEXT("DATUM"), ESearchCase::IgnoreCase)
            || Source.EligibilityReasonCode.Contains(TEXT("DATUM"), ESearchCase::IgnoreCase))
        {
            return FString::Printf(TEXT("Source %s was rejected: %s; resolver reason: %s."),
                UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()),
                *Source.CogStatusCode, *Source.EligibilityReasonCode);
        }
    }
    return TEXT("No catalog candidate has identity-bound COG metadata and an explicit NAVD88 proof.");
}

/** Counts and observes transport responses without changing their proof semantics. */
class FObservedTransport final : public IAcquisitionTransport
{
public:
    FObservedTransport(IAcquisitionTransport& InInner, const FStagedTerrainAcquisitionLimits& Limits)
        : Inner(InInner), MaxRequests(Limits.MaxRequests), MaxBytes(Limits.MaxTransferredBytes) {}

    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& Cancellation) override
    {
        if (Cancellation->IsCancelled())
            return TransportFailure(TransportFailureReason::Cancelled, TEXT("STAGED_TERRAIN_CANCELLED"));
        if (Requests >= MaxRequests || BytesTransferred >= MaxBytes)
            return TransportFailure(TransportFailureReason::ResponseTooLarge, TEXT("STAGED_TERRAIN_NETWORK_BUDGET_EXCEEDED"));

        const uint64 Remaining = MaxBytes - BytesTransferred;
        if (Request.ByteRange.IsSet() && Request.ByteRange->Length > Remaining)
            return TransportFailure(TransportFailureReason::ResponseTooLarge, TEXT("STAGED_TERRAIN_NETWORK_BUDGET_EXCEEDED"));

        HttpAcquisitionRequest BoundedRequest = Request;
        BoundedRequest.MaximumResponseBytes = FMath::Min(Request.MaximumResponseBytes, Remaining);
        if (BoundedRequest.MaximumResponseBytes == 0)
            return TransportFailure(TransportFailureReason::ResponseTooLarge, TEXT("STAGED_TERRAIN_NETWORK_BUDGET_EXCEEDED"));

        ++Requests;
        HttpAcquisitionResult Result = Inner.Get(BoundedRequest, Cancellation);
        const uint64 Received = FMath::Max<uint64>(Result.Bytes.Num(), Result.BytesReceived);
        if (Received > MaxBytes - BytesTransferred)
        {
            BytesTransferred = MaxBytes;
            Result.Bytes.Reset();
            Result.BytesReceived = 0;
            Result.FailureReason = TransportFailureReason::ResponseTooLarge;
            Result.RequestStatus = TEXT("STAGED_TERRAIN_NETWORK_BUDGET_EXCEEDED");
            return Result;
        }
        BytesTransferred += Received;

        if (Request.ByteRange.IsSet())
        {
            FObservedCogVersion& Version = Versions.FindOrAdd(Request.Url);
            ++Version.RangeRequests;
            if (Version.RangeRequests == 1)
            {
                Version.FirstETag = Result.ETag;
            }
            else if (Result.ETag != Version.FirstETag)
            {
                Version.bChanged = true;
            }
            if (!Request.IfMatchETag.IsEmpty() && Request.IfMatchETag != Version.FirstETag)
                Version.bChanged = true;
        }
        return Result;
    }

    uint64 Requests = 0;
    uint64 BytesTransferred = 0;

    bool GetStableStrongETag(const FString& Url, FString& OutETag) const
    {
        OutETag.Reset();
        const FObservedCogVersion* Version = Versions.Find(Url);
        if (!Version || Version->RangeRequests == 0 || Version->bChanged
            || !IsStrongETag(Version->FirstETag)) return false;
        OutETag = Version->FirstETag;
        return true;
    }

private:
    struct FObservedCogVersion
    {
        FString FirstETag;
        uint64 RangeRequests = 0;
        bool bChanged = false;
    };

    static HttpAcquisitionResult TransportFailure(const TransportFailureReason Reason, const TCHAR* Status)
    {
        HttpAcquisitionResult Result;
        Result.FailureReason = Reason;
        Result.RequestStatus = Status;
        return Result;
    }

    IAcquisitionTransport& Inner;
    const uint64 MaxRequests;
    const uint64 MaxBytes;
    TMap<FString, FObservedCogVersion> Versions;
};

FStagedTerrainStageResult RunCatalogStage(IAcquisitionTransport& Transport,
    const FStagedTerrainAcquisitionRequest& Request,
    const FStagedTerrainAcquisitionReceipt& Receipt,
    const TSharedRef<Cancellation>& Cancellation)
{
    FObservedTransport Observed(Transport, Request.Limits);
    ElevationCatalog Catalog(Observed);
    TerrainAvailabilityReport Report;
    if (!Catalog.Preflight(Request.Bounds, Cancellation, Report))
    {
        FStagedTerrainStageResult Result = Failed(
            Cancellation->IsCancelled() ? TEXT("STAGED_TERRAIN_CANCELLED") : TEXT("STAGED_TERRAIN_CATALOG_PREFLIGHT_FAILED"),
            Report.FailureDetail.IsEmpty() ? Report.FailureCode : Report.FailureDetail,
            Cancellation->IsCancelled() ? EStagedTerrainStageOutcome::Cancelled : EStagedTerrainStageOutcome::FatalFailure);
        Result.Usage.Requests = Observed.Requests;
        Result.Usage.TransferredBytes = Observed.BytesTransferred;
        return Result;
    }

    FStagedTerrainStageResult Result;
    Result.Outcome = EStagedTerrainStageOutcome::FatalFailure;
    Result.Proof.bSupportedGeography = Report.IsSupportedGeography;
    // Deliberately not copied from HasElevationCoverage: that bit only means a TNM bbox
    // query returned a candidate, not that a selected raster covers the complete site.
    Result.Proof.bSiteCoverageVerified = Report.HasVerifiedSiteCoverage;
    Result.CompletedUnits = static_cast<uint64>(Report.ResolvedSources.Num());
    Result.TotalUnits = Result.CompletedUnits;
    Result.Usage.Requests = Observed.Requests;
    Result.Usage.TransferredBytes = Observed.BytesTransferred;

    if (!Report.IsSupportedGeography)
    {
        Result.FailureCode = TEXT("STAGED_TERRAIN_UNSUPPORTED_GEOGRAPHY");
        Result.FailureDetail = Report.FailureDetail.IsEmpty()
            ? TEXT("The requested site is outside the supported USGS 3DEP geography envelope.")
            : Report.FailureDetail;
        return Result;
    }

    if (Report.ResolvedSources.IsEmpty())
    {
        bool bDatumRejected = false;
        for (const ElevationCatalogSource& Source : Report.Sources)
        {
            bDatumRejected |= Source.CogStatusCode.Contains(TEXT("DATUM"), ESearchCase::IgnoreCase)
                || Source.EligibilityReasonCode.Contains(TEXT("DATUM"), ESearchCase::IgnoreCase);
        }
        Result.FailureCode = bDatumRejected
            ? TEXT("STAGED_TERRAIN_NAVD88_DATUM_NOT_PROVEN")
            : TEXT("STAGED_TERRAIN_NO_PROVEN_ELEVATION_SOURCE");
        Result.FailureDetail = DatumFailureDetail(Report);
        return Result;
    }

    TArray<FString> MissingProofs;
    if (!Report.HasVerifiedCogHeaders)
        MissingProofs.Add(FString::Printf(TEXT("cog=%s"), *Report.CogStatusCode));
    if (!Report.HasVerifiedSiteCoverage || IsUnprovenCoverageCode(Report.CoverageStatusCode))
        MissingProofs.Add(FString::Printf(TEXT("full-site-raster-coverage=%s"), *Report.CoverageStatusCode));
    if (!Report.HasCompleteSourceLineage || Report.LineageStatusCode.IsEmpty()
        || Report.LineageStatusCode == TEXT("LINEAGE_SIDECARS_NOT_READ"))
        MissingProofs.Add(FString::Printf(TEXT("raw-gpkg-xml-lineage=%s"),
            Report.LineageStatusCode.IsEmpty() ? TEXT("LINEAGE_NOT_PROVEN") : *Report.LineageStatusCode));
    if (!Report.HasQualityReport || Report.QualityReport.TotalSamples == 0)
        MissingProofs.Add(FString::Printf(TEXT("sampled-quality=%s"), *Report.QualityStatusCode));
    else
        MissingProofs.Add(TEXT("lineage-quality-details=STAGED_RECEIPT_SCHEMA_CANNOT_RETAIN_EVIDENCE"));

    // ElevationCatalog currently has no per-source coverage-evidence identifier and the
    // staged receipt has no durable lineage/quality fields. Do not convert status booleans
    // into opaque proof IDs or advance a receipt that cannot retain those underlying facts.
    for (const ElevationCatalogSource& Source : Report.ResolvedSources)
    {
        // A status string is not a source-specific evidence identity. ElevationCatalog has
        // not yet exposed the raster/site intersection receipt needed by this field.
        MissingProofs.Add(FString::Printf(TEXT("source-coverage-evidence-id=NOT_EXPOSED_BY_ELEVATION_CATALOG(%s)"),
            Source.CoverageStatusCode.IsEmpty() ? TEXT("empty-status") : *Source.CoverageStatusCode));
        FString StrongETag;
        if (!Source.HasVerifiedCogHeader || !Source.CogPreflight.bPassed
            || Source.CogProofSourceId != UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str())
            || Source.CogProofDownloadUrl != Source.DownloadUrl
            || !Source.HasExactObjectBytes || Source.ExactObjectBytes == 0
            || Source.ExactObjectBytes != Source.CogPreflight.ObjectBytes
            || Source.Proofs.HorizontalCrsOrigin == ElevationProofOrigin::None
            || Source.Proofs.VerticalDatumOrigin == ElevationProofOrigin::None
            || Source.Proofs.EncodingOrigin == ElevationProofOrigin::None
            || !Source.Candidate.SupportedHorizontalCrs || !Source.Candidate.Navd88Proven
            || !Source.Candidate.SupportedEncoding || !IsNavd88(Source.Proofs.VerticalDatum))
        {
            MissingProofs.Add(FString::Printf(TEXT("source-proof=%s:%s"),
                UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()), *Source.EligibilityReasonCode));
            continue;
        }
        if (!Observed.GetStableStrongETag(Source.DownloadUrl, StrongETag)
            || !Source.CogPreflight.bStrongETagPinned)
        {
            MissingProofs.Add(FString::Printf(TEXT("etag=%s:NOT_STRONGLY_PINNED"), *Source.DownloadUrl));
        }
        else
        {
            FStagedTerrainObjectPin Pin;
            Pin.ProductCode = ProductCode(Source);
            Pin.SourceId = UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str());
            Pin.Url = Source.DownloadUrl;
            Pin.ETag = StrongETag;
            Pin.ObjectBytes = Source.CogPreflight.ObjectBytes;
            const FStagedTerrainObjectPin* Existing = Receipt.PinnedObjects.FindByPredicate(
                [&Pin](const FStagedTerrainObjectPin& Other) { return Other.SourceId == Pin.SourceId; });
            if (Existing && (Existing->Url != Pin.Url || Existing->ETag != Pin.ETag
                || Existing->ObjectBytes != Pin.ObjectBytes))
            {
                Result.FailureCode = TEXT("STAGED_TERRAIN_ETAG_OR_OBJECT_IDENTITY_CHANGED");
                Result.FailureDetail = FString::Printf(TEXT("Catalog retry changed the pinned COG identity for %s."),
                    *Pin.SourceId);
                return Result;
            }
            Result.ObjectPins.Add(Pin);
        }
    }

    Result.Proof.bSiteCoverageVerified = Report.HasVerifiedSiteCoverage
        && MissingProofs.FindByPredicate([](const FString& Proof)
            { return Proof.StartsWith(TEXT("full-site-raster-coverage="))
                || Proof.StartsWith(TEXT("source-coverage-evidence-id=")); }) == nullptr;

    if (!Report.DownloadEnabled || Report.Status != ElevationCatalogStatus::Ready || !MissingProofs.IsEmpty())
    {
        Result.FailureCode = MissingProofs.ContainsByPredicate([](const FString& Proof)
                { return Proof.StartsWith(TEXT("raw-gpkg-xml-lineage=")); })
            ? TEXT("STAGED_TERRAIN_S1M_XML_GPKG_NOT_PROVEN")
            : MissingProofs.ContainsByPredicate([](const FString& Proof)
                { return Proof.StartsWith(TEXT("full-site-raster-coverage="))
                    || Proof.StartsWith(TEXT("source-coverage-evidence-id=")); })
                ? TEXT("STAGED_TERRAIN_SITE_RASTER_COVERAGE_NOT_PROVEN")
                : MissingProofs.ContainsByPredicate([](const FString& Proof)
                    { return Proof.StartsWith(TEXT("sampled-quality=")); })
                    ? TEXT("STAGED_TERRAIN_SAMPLED_QUALITY_NOT_PROVEN")
                    : TEXT("STAGED_TERRAIN_CATALOG_EVIDENCE_INCOMPLETE");
        Result.FailureDetail = FString::Printf(
            TEXT("Catalog status=%d; report=%s; bboxCoverage=%s; cog=%s; missing proofs: %s. "
                "The TNM bbox and COG header are not full-site coverage, raw .gpkg/.xml lineage, or sampled quality."),
            static_cast<int32>(Report.Status), *Report.FailureCode, *Report.CoverageStatusCode,
            *Report.CogStatusCode, *FString::Join(MissingProofs, TEXT(", ")));
        return Result;
    }

    for (const ElevationCatalogSource& Source : Report.ResolvedSources)
    {
        if (Source.CoverageStatusCode.IsEmpty() || IsUnprovenCoverageCode(Source.CoverageStatusCode))
        {
            Result.FailureCode = TEXT("STAGED_TERRAIN_SITE_COVERAGE_EVIDENCE_ID_MISSING");
            Result.FailureDetail = TEXT("ElevationCatalog did not expose a verified per-source raster coverage evidence ID.");
            return Result;
        }
        FStagedTerrainSelectedSource Selected;
        Selected.Candidate = Source.Candidate;
        Selected.DownloadUrl = Source.DownloadUrl;
        Selected.HorizontalCrs = Source.Proofs.HorizontalCrs;
        Selected.VerticalDatum = Source.Proofs.VerticalDatum;
        Selected.bSiteCoverageVerified = true;
        Selected.CoverageEvidenceId = Source.CoverageStatusCode;
        Result.SelectedSources.Add(MoveTemp(Selected));
    }

    FString DigestText = FString::Printf(TEXT("Catalog|%s|%s|%s|%d|%s|%s|%s"),
        *Report.GeographyRegion, *Report.CoverageStatusCode, *Report.LineageStatusCode,
        static_cast<int32>(Report.QualityReport.Grade), *Report.QualityStatusCode,
        *Report.CogStatusCode, *Report.FailureCode);
    for (const FStagedTerrainSelectedSource& Source : Result.SelectedSources)
        DigestText += FString::Printf(TEXT("|%s|%s|%s|%s|%s|%s"), *SourceId(Source),
            *ProductCode(Source.Candidate.Product), *Source.DownloadUrl, *Source.HorizontalCrs,
            *Source.VerticalDatum, *Source.CoverageEvidenceId);
    Result.OutputSha256 = Sha256Text(DigestText);
    if (Result.OutputSha256.IsEmpty())
    {
        Result.FailureCode = TEXT("STAGED_TERRAIN_CATALOG_PROOF_DIGEST_FAILED");
        Result.FailureDetail = TEXT("Could not fingerprint the catalog proof snapshot.");
        return Result;
    }
    Result.Outcome = EStagedTerrainStageOutcome::Succeeded;
    return Result;
}

FStagedTerrainStageResult RunSourcePreflightStage(IAcquisitionTransport& Transport,
    const FStagedTerrainAcquisitionRequest& Request,
    const FStagedTerrainAcquisitionReceipt& Receipt,
    const TSharedRef<Cancellation>& Cancellation)
{
    if (Cancellation->IsCancelled())
        return Failed(TEXT("STAGED_TERRAIN_CANCELLED"), TEXT("Source preflight was cancelled."),
            EStagedTerrainStageOutcome::Cancelled);
    if (!Receipt.CompletedStages.ContainsByPredicate([](const FStagedTerrainCompletedStage& Stage)
            { return Stage.Stage == EStagedTerrainStage::Catalog; }))
        return Failed(TEXT("STAGED_TERRAIN_CATALOG_STAGE_REQUIRED"),
            TEXT("Source preflight requires a completed catalog stage."));
    if (Receipt.SelectedSources.IsEmpty())
        return Failed(TEXT("STAGED_TERRAIN_SOURCE_PREFLIGHT_INCOMPLETE"),
            TEXT("The completed catalog stage has no selected source proofs."));

    FObservedTransport Observed(Transport, Request.Limits);
    FStagedTerrainStageResult Result;
    Result.Outcome = EStagedTerrainStageOutcome::FatalFailure;
    FString DigestText(TEXT("SourcePreflight"));
    TSet<FString> SeenSourceIds;

    for (const FStagedTerrainSelectedSource& Selected : Receipt.SelectedSources)
    {
        const FString Id = SourceId(Selected);
        const FString Product = ProductCode(Selected.Candidate.Product);
        FString UrlReason;
        if (Id.IsEmpty() || Product.IsEmpty() || !Selected.Candidate.SupportedHorizontalCrs
            || !Selected.Candidate.Navd88Proven || !Selected.Candidate.SupportedEncoding
            || Selected.HorizontalCrs.IsEmpty() || !IsNavd88(Selected.VerticalDatum)
            || !Selected.bSiteCoverageVerified || IsUnprovenCoverageCode(Selected.CoverageEvidenceId)
            || !SkiNetGateway::ValidateUrl(Selected.DownloadUrl, UrlReason)
            || SeenSourceIds.Contains(Id))
        {
            Result.FailureCode = TEXT("STAGED_TERRAIN_SOURCE_CATALOG_PROOF_INVALID");
            Result.FailureDetail = FString::Printf(TEXT("Catalog proof for %s is incomplete or invalid: %s."),
                *Id, UrlReason.IsEmpty() ? TEXT("identity, datum, encoding, or full-site coverage missing") : *UrlReason);
            return Result;
        }
        SeenSourceIds.Add(Id);

        const FStagedTerrainObjectPin* CatalogPin = Receipt.PinnedObjects.FindByPredicate(
            [&Id](const FStagedTerrainObjectPin& Pin) { return Pin.SourceId == Id; });
        if (!CatalogPin || CatalogPin->ProductCode != Product || CatalogPin->Url != Selected.DownloadUrl
            || !IsStrongETag(CatalogPin->ETag) || CatalogPin->ObjectBytes == 0)
        {
            Result.FailureCode = TEXT("STAGED_TERRAIN_SOURCE_ETAG_PIN_MISSING");
            Result.FailureDetail = FString::Printf(TEXT("Catalog has no strong ETag and exact-size pin for %s."), *Id);
            return Result;
        }

        FCogPreflightMetadata Metadata;
        Metadata.CatalogHorizontalCrs = Selected.HorizontalCrs;
        Metadata.CatalogVerticalDatum = Selected.VerticalDatum;
        FCogPreflightReport Preflight;
        if (!PreflightElevationCog(Observed, Selected.DownloadUrl, Selected.Candidate.Product,
                Metadata, Request.Limits.Sampler.Preflight, Cancellation, Preflight))
        {
            Result.FailureCode = Preflight.FailureCode.IsEmpty()
                ? TEXT("COG_PREFLIGHT_FAILED") : Preflight.FailureCode;
            Result.FailureDetail = Preflight.FailureDetail;
            Result.Usage.Requests = Observed.Requests;
            Result.Usage.TransferredBytes = Observed.BytesTransferred;
            if (Cancellation->IsCancelled()) Result.Outcome = EStagedTerrainStageOutcome::Cancelled;
            return Result;
        }

        FString StrongETag;
        if (!Observed.GetStableStrongETag(Selected.DownloadUrl, StrongETag)
            || !Preflight.bStrongETagPinned)
        {
            Result.FailureCode = TEXT("COG_PREFLIGHT_STRONG_ETAG_REQUIRED");
            Result.FailureDetail = TEXT("The source preflight did not observe one stable strong ETag across all ranges.");
            Result.Usage.Requests = Observed.Requests;
            Result.Usage.TransferredBytes = Observed.BytesTransferred;
            return Result;
        }
        if (StrongETag != CatalogPin->ETag || Preflight.ObjectBytes != CatalogPin->ObjectBytes)
        {
            Result.FailureCode = TEXT("STAGED_TERRAIN_ETAG_OR_OBJECT_IDENTITY_CHANGED");
            Result.FailureDetail = FString::Printf(
                TEXT("Pinned object changed between Catalog and SourcePreflight for %s (ETag or exact byte total differs)."), *Id);
            Result.Usage.Requests = Observed.Requests;
            Result.Usage.TransferredBytes = Observed.BytesTransferred;
            return Result;
        }
        if (Preflight.HorizontalCrs != Selected.HorizontalCrs || !IsNavd88(Preflight.VerticalDatum))
        {
            Result.FailureCode = TEXT("STAGED_TERRAIN_SOURCE_PREFLIGHT_PROOF_MISMATCH");
            Result.FailureDetail = FString::Printf(TEXT("COG proof no longer matches the catalog CRS or NAVD88 facts for %s."), *Id);
            Result.Usage.Requests = Observed.Requests;
            Result.Usage.TransferredBytes = Observed.BytesTransferred;
            return Result;
        }

        FStagedTerrainCogObservation Observation;
        Observation.SourceId = Id;
        Observation.Url = Selected.DownloadUrl;
        Observation.StrongETag = StrongETag;
        Observation.Preflight = MoveTemp(Preflight);
        Result.CogObservations.Add(MoveTemp(Observation));
        Result.ObjectPins.Add(*CatalogPin);
        DigestText += FString::Printf(TEXT("|%s|%s|%s|%llu"), *Id, *Selected.DownloadUrl,
            *StrongETag, CatalogPin->ObjectBytes);
    }

    Result.Usage.Requests = Observed.Requests;
    Result.Usage.TransferredBytes = Observed.BytesTransferred;
    Result.CompletedUnits = Result.CogObservations.Num();
    Result.TotalUnits = Receipt.SelectedSources.Num();
    Result.OutputSha256 = Sha256Text(DigestText);
    if (Result.OutputSha256.IsEmpty())
    {
        Result.FailureCode = TEXT("STAGED_TERRAIN_SOURCE_PREFLIGHT_DIGEST_FAILED");
        Result.FailureDetail = TEXT("Could not fingerprint the source-preflight observations.");
        return Result;
    }
    Result.Outcome = EStagedTerrainStageOutcome::Succeeded;
    return Result;
}
}

SkiPreparation::FStagedTerrainStageResult SkiPreparation::FNativeStagedTerrainAcquisitionAdapter::RunStage(
    const EStagedTerrainStage Stage,
    const FStagedTerrainAcquisitionRequest& Request,
    const FStagedTerrainAcquisitionReceipt& Receipt,
    SkiNetGateway& Gateway,
    const TSharedRef<Cancellation>& Cancellation,
    const FString& CheckpointToken)
{
    static_cast<void>(CheckpointToken);
    if (Cancellation->IsCancelled())
        return Failed(TEXT("STAGED_TERRAIN_CANCELLED"), TEXT("The acquisition stage was cancelled."),
            EStagedTerrainStageOutcome::Cancelled);

    IAcquisitionTransport& Transport = TestTransportOverride ? *TestTransportOverride : static_cast<IAcquisitionTransport&>(Gateway);
    switch (Stage)
    {
    case EStagedTerrainStage::Catalog:
        return RunCatalogStage(Transport, Request, Receipt, Cancellation);
    case EStagedTerrainStage::SourcePreflight:
        return RunSourcePreflightStage(Transport, Request, Receipt, Cancellation);
    case EStagedTerrainStage::CanonicalLod0:
        return Failed(TEXT("STAGED_TERRAIN_LOD0_SAMPLER_UNSUPPORTED"),
            TEXT("The production adapter seam for canonical LOD0 sampling and raw provenance is not implemented."));
    case EStagedTerrainStage::CoarseLodCopy:
        return Failed(TEXT("STAGED_TERRAIN_COARSE_LOD_COPY_UNSUPPORTED"),
            TEXT("The production adapter seam for indexed bit-exact LOD copies is not implemented."));
    case EStagedTerrainStage::WorldCover:
        return Failed(TEXT("STAGED_TERRAIN_WORLDCOVER_UNSUPPORTED"),
            TEXT("The production adapter seam for bounded WorldCover acquisition and classification is not implemented."));
    case EStagedTerrainStage::TerrainCoreStaging:
        return Failed(TEXT("STAGED_TERRAIN_CORE_STAGING_UNSUPPORTED"),
            TEXT("The production adapter seam for TerrainCore staging is not implemented."));
    case EStagedTerrainStage::TerrainCoreVerification:
        return Failed(TEXT("STAGED_TERRAIN_CORE_VERIFICATION_UNSUPPORTED"),
            TEXT("The production adapter seam for TerrainCore verification is not implemented."));
    case EStagedTerrainStage::Complete:
    default:
        return Failed(TEXT("STAGED_TERRAIN_STAGE_UNSUPPORTED"),
            TEXT("Complete is a coordinator state and is not executable by an adapter."));
    }
}
