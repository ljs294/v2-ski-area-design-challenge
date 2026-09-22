#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
using TerrainCoreTileSource = TFunction<bool(
    const SkiDomain::TerrainCoreTileDescriptor& Planned,
    TerrainCoreEncodedTile& OutTile, FString& OutError)>;

struct SKIPREPARATION_API TerrainCorePackageIndex
{
    SkiDomain::TerrainCoreManifest Manifest;
    FString PackageDirectory;
};

/** Separate schema-2 store. The schema-1 PackageStore remains unchanged. */
class SKIPREPARATION_API TerrainCorePackageStore
{
public:
    explicit TerrainCorePackageStore(FString InDataRoot);

    bool WriteAndActivate(SkiDomain::TerrainCoreManifest Manifest,
        const SkiDomain::Heightfield& Finest, FString& OutPackageDirectory,
        SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;

    /** Production streaming path: requests each planned tile exactly once and retains one payload. */
    bool WriteAndActivateFromTiles(SkiDomain::TerrainCoreManifest Manifest,
        const TerrainCoreTileSource& TileSource, FString& OutPackageDirectory,
        SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;

    /** Opens a bounded index and checks exact files, shard lengths, and path ancestry. */
    bool Open(const FString& ContentId, TerrainCorePackageIndex& OutIndex,
        FString& OutError) const;

    /** Reads, hashes, decompresses, and validates only the selected tile ranges. */
    bool ReadTile(const TerrainCorePackageIndex& Index, uint8 LodIndex,
        uint32 TileX, uint32 TileY, TerrainCoreDecodedTile& OutTile,
        FString& OutError) const;

    /** Incrementally verifies hashes plus shared-edge, halo, and LOD semantics. */
    bool Verify(const TerrainCorePackageIndex& Index, FString& OutError) const;

    bool WriteEditSetAndActivate(const SkiDomain::TerrainEditSet& Edits,
        uint32 Width, uint32 Height, FString& OutEditSetId, FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0) const;
    bool LoadEditSet(const FString& TerrainCoreId, const FString& EditSetId,
        uint32 Width, uint32 Height, SkiDomain::TerrainEditSet& OutEdits,
        FString& OutError) const;

    const FString& DataRoot() const noexcept { return Root; }

private:
    FString Root;
};

SKIPREPARATION_API FString SerializeTerrainCoreManifest(
    const SkiDomain::TerrainCoreManifest& Manifest, bool IncludeContentId);
SKIPREPARATION_API bool ParseTerrainCoreManifest(const FString& Json,
    SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError);
}
