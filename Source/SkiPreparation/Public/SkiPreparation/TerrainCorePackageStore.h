#pragma once

#include "CoreMinimal.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <string>
#include <vector>

namespace SkiPreparation
{
using TerrainCoreTileSource = TFunction<bool(
    const SkiDomain::TerrainCoreTileDescriptor& Planned,
    TerrainCoreEncodedTile& OutTile, FString& OutError)>;

/** Supplies the staged per-sample quality and source planes for one LOD0 tile. */
using TerrainCoreProvenanceTileSource = TFunction<bool(
    const SkiDomain::TerrainCoreTileDescriptor& Planned,
    TArrayView<const uint8> Validity, TArray<uint8>& OutProvenance,
    TArray<uint8>& OutSourceIndices, FString& OutError)>;

struct SKIPREPARATION_API TerrainCoreProvenanceSidecarDescriptor
{
    std::string Path;
    std::uint64_t Bytes = 0;
    std::string Sha256;
    /** Hash of the class/source planes, which breaks the TCP1 ContentId cycle. */
    std::string SemanticSha256;
};

struct SKIPREPARATION_API TerrainCorePackageIndex
{
    SkiDomain::TerrainCoreManifest Manifest;
    FString PackageDirectory;
    std::vector<TerrainCoreProvenanceSidecarDescriptor> ProvenanceSidecars;
    /** SHA-256 of the full canonical terraincore.json, including sidecar file hashes. */
    FString PackageManifestSha256;
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
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0,
        TerrainCoreProvenanceTileSource ProvenanceSource = {}) const;

    /** Production streaming path: requests each planned tile exactly once and retains one payload. */
    bool WriteAndActivateFromTiles(SkiDomain::TerrainCoreManifest Manifest,
        const TerrainCoreTileSource& TileSource, FString& OutPackageDirectory,
        SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError,
        const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease = nullptr,
        uint64 SessionGeneration = 0, uint64 OperationGeneration = 0,
        TerrainCoreProvenanceTileSource ProvenanceSource = {}) const;

    /** Opens a bounded index and checks exact files, shard lengths, and path ancestry. */
    bool Open(const FString& ContentId, TerrainCorePackageIndex& OutIndex,
        FString& OutError) const;

    /** Reads, hashes, decompresses, and validates only the selected tile ranges. */
    bool ReadTile(const TerrainCorePackageIndex& Index, uint8 LodIndex,
        uint32 TileX, uint32 TileY, TerrainCoreDecodedTile& OutTile,
        FString& OutError) const;

    /** Reads a declared, hashed TCP1 LOD0 record; legacy packages report unavailable. */
    bool ReadProvenanceSidecar(const TerrainCorePackageIndex& Index, uint8 LodIndex,
        uint32 TileX, uint32 TileY, TArray<uint8>& OutBytes, FString& OutError) const;

    /** Revalidates TCP1 identity, grid, source dictionary, and exact tile validity. */
    bool ReadVerifiedProvenanceSummary(const TerrainCorePackageIndex& Index,
        SkiApplication::TerrainCoreProvenanceSummary& OutSummary, FString& OutError) const;

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
