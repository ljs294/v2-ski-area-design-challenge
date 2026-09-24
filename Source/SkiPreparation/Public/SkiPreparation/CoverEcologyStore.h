#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/CoverEcology.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
struct SKIPREPARATION_API CoverEcologyPackageIndex
{
    SkiDomain::CoverEcologyManifest Manifest;
    FString PackageDirectory;
};

struct SKIPREPARATION_API InstalledTerrainIndex
{
    std::uint32_t SchemaVersion = 0;
    SkiDomain::InstalledTerrainReceipt Receipt;
    CompositeInstallReceipt CompositeReceipt;
    FString ReceiptDirectory;
};

struct SKIPREPARATION_API InstalledTerrainLibraryEntry
{
    FString ContentId;
    FString SourceId;
    FString AcquisitionEpoch;
    FString GeneratorVersion;
};

class SKIPREPARATION_API CoverEcologyStore
{
public:
    explicit CoverEcologyStore(FString InDataRoot);

    bool WriteAndActivate(SkiDomain::CoverEcologyManifest Manifest,
        const TArrayView<const uint8> Classes, const TArrayView<const uint8> Validity,
        FString& OutPackageDirectory, SkiDomain::CoverEcologyManifest& OutManifest,
        FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;
    bool Open(const FString& ContentId, CoverEcologyPackageIndex& OutIndex,
        FString& OutError) const;
    bool Verify(const CoverEcologyPackageIndex& Index, FString& OutError) const;
    bool ReadChannels(const CoverEcologyPackageIndex& Index, TArray<uint8>& OutClasses,
        TArray<uint8>& OutValidity, FString& OutError) const;

private:
    FString Root;
};

/** Activates only references to already reopened and verified component stores. */
class SKIPREPARATION_API InstalledTerrainStore
{
public:
    explicit InstalledTerrainStore(FString InDataRoot);

    bool WriteAndActivate(SkiDomain::InstalledTerrainReceipt Receipt,
        FString& OutReceiptDirectory, SkiDomain::InstalledTerrainReceipt& OutReceipt,
        FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;
    bool WriteAndActivate(CompositeInstallReceipt Receipt,
        FString& OutReceiptDirectory, CompositeInstallReceipt& OutReceipt,
        FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;
    bool Open(const FString& ContentId, InstalledTerrainIndex& OutIndex,
        FString& OutError) const;
    /** Lists only canonical receipt directories whose complete composite passes Open. */
    bool ListVerified(TArray<InstalledTerrainLibraryEntry>& OutEntries,
        FString& OutError) const;

private:
    FString Root;
};

SKIPREPARATION_API FString SerializeCoverEcologyManifest(
    const SkiDomain::CoverEcologyManifest& Manifest, bool IncludeContentId);
SKIPREPARATION_API bool ParseCoverEcologyManifest(const FString& Json,
    SkiDomain::CoverEcologyManifest& OutManifest, FString& OutError);
SKIPREPARATION_API FString SerializeInstalledTerrainReceipt(
    const SkiDomain::InstalledTerrainReceipt& Receipt, bool IncludeContentId);
SKIPREPARATION_API bool ParseInstalledTerrainReceipt(const FString& Json,
    SkiDomain::InstalledTerrainReceipt& OutReceipt, FString& OutError);
}
