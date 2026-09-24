#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformFile.h"
#include "Misc/ScopeLock.h"
#include "SkiDomain/TerrainCore.h"

namespace SkiPreparation
{
class Cancellation;

static_assert(sizeof(float) == 4, "Terrain scratch stores raw float32 heights.");

/** Per-sample quality class retained in canonical LOD0 scratch. */
enum class TerrainScratchProvenance : uint8
{
    S1MNative = 0,
    S1MBlend = 1,
    S1MBackfill = 2,
    S1MInterpolated = 3,
    Project1m = 4,
    ArcSec13 = 5,
    NoData = 255,
};

inline constexpr uint8 TerrainScratchNoSourceIndex = 0xffU;
inline constexpr uint32 TerrainScratchTileSamples = 256U;
inline constexpr uint64 TerrainScratchBytesPerSample = sizeof(float) + 3ULL;
inline constexpr uint64 TerrainScratchMaximumBytes =
    SkiDomain::TerrainCoreMaxSamples * TerrainScratchBytesPerSample;

/** One copied TerrainCore LOD tile, including any declared halo. */
struct SKIPREPARATION_API TerrainScratchLodTile
{
    SkiDomain::TerrainCoreTileDescriptor Descriptor;
    uint16 Width = 0;
    uint16 Height = 0;
    TArray<float> Heights;
    TArray<uint8> Validity;
    TArray<uint8> Provenance;
    TArray<uint8> SourceIndices;
};

/**
 * Bounded, temporary 1 m canonical sample store.
 *
 * WriteLod0Tile accepts disjoint 256x256 (partial at east/south edges) sample
 * blocks. The store is row-major on disk as four raw planes: IEEE float32
 * heights, one byte validity, one byte provenance class, and one byte source
 * table index. NoData samples use NoData provenance and TerrainScratchNoSourceIndex.
 * Coarser LOD reads only copy selected LOD0 sample bytes at the TerrainCore
 * factor and edge-clamp locations; no averaging or raster resampling occurs.
 */
class SKIPREPARATION_API TerrainScratchStore
{
public:
    TerrainScratchStore() = default;
    ~TerrainScratchStore();

    TerrainScratchStore(const TerrainScratchStore&) = delete;
    TerrainScratchStore& operator=(const TerrainScratchStore&) = delete;

    /** Exact raw storage required by the four canonical planes. */
    static bool TryCalculateRequiredStorageBytes(uint32 Width, uint32 Height,
        uint64& OutBytes, uint32* OutTilesX = nullptr, uint32* OutTilesY = nullptr) noexcept;

    /**
     * Creates a new scratch directory. Spacing must be exactly 1 m in both axes.
     * MaxStorageBytes is the caller's preflight allowance for the raw files.
     */
    bool Create(const FString& ScratchDirectory, uint32 Width, uint32 Height,
        double EastSpacingM, double NorthSpacingM,
        const TArray<SkiDomain::TerrainCoreSource>& Sources, uint64 MaxStorageBytes,
        FString& OutError);

    /** Writes one disjoint canonical tile using explicit row strides. */
    bool WriteLod0Tile(uint32 TileX, uint32 TileY,
        TArrayView<const float> Heights, uint64 HeightStrideSamples,
        TArrayView<const uint8> Validity, uint64 ValidityStrideBytes,
        TArrayView<const uint8> Provenance, uint64 ProvenanceStrideBytes,
        TArrayView<const uint8> SourceIndices, uint64 SourceStrideBytes,
        FString& OutError, const Cancellation* CancellationValue = nullptr);

    /** Seals the store after every 256x256 LOD0 tile has been written once. */
    bool Finalize(FString& OutError, const Cancellation* CancellationValue = nullptr);

    /** Copies one planned TerrainCore tile from indexed canonical LOD0 samples. */
    bool ReadLodTile(const SkiDomain::TerrainCoreTileDescriptor& Planned,
        TerrainScratchLodTile& OutTile, FString& OutError,
        const Cancellation* CancellationValue = nullptr) const;

    /** Removes the exact scratch directory created by this instance. */
    bool Cleanup(FString& OutError);

    uint32 Width() const noexcept { return RasterWidth; }
    uint32 Height() const noexcept { return RasterHeight; }
    double EastSpacingM() const noexcept { return RasterEastSpacingM; }
    double NorthSpacingM() const noexcept { return RasterNorthSpacingM; }
    uint64 RequiredStorageBytes() const noexcept { return StorageBytes; }
    const TArray<SkiDomain::TerrainCoreSource>& Sources() const noexcept { return SourceTable; }
    const FString& Directory() const noexcept { return RootDirectory; }
    bool IsFinalized() const noexcept { return CurrentState == EState::Finalized; }

private:
    enum class EState : uint8 { Empty, Writing, Finalized, Failed, Cleaned };

    bool Fail(FString& OutError, const TCHAR* Message);
    bool CheckCancellation(FString& OutError, const Cancellation* CancellationValue) const;

    mutable FCriticalSection Mutex;
    EState CurrentState = EState::Empty;
    FString RootDirectory;
    FString HeightPath;
    FString ValidityPath;
    FString ProvenancePath;
    FString SourceIndexPath;
    TUniquePtr<IFileHandle> HeightFile;
    TUniquePtr<IFileHandle> ValidityFile;
    TUniquePtr<IFileHandle> ProvenanceFile;
    TUniquePtr<IFileHandle> SourceIndexFile;
    TArray<SkiDomain::TerrainCoreSource> SourceTable;
    TArray<uint8> WrittenTiles;
    uint32 RasterWidth = 0;
    uint32 RasterHeight = 0;
    uint32 TilesX = 0;
    uint32 TilesY = 0;
    double RasterEastSpacingM = 0.0;
    double RasterNorthSpacingM = 0.0;
    uint64 SampleCount = 0;
    uint64 StorageBytes = 0;
    bool bOwnsDirectory = false;
};
}
