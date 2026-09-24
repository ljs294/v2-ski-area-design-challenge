#include "SkiPreparation/TerrainScratchStore.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
bool CheckedArrayLength(const int32 Available, const uint32 Rows,
    const uint64 Stride, const uint64 RowWidth)
{
    if (Rows == 0 || Stride < RowWidth)
    {
        return false;
    }
    const uint64 Max = (std::numeric_limits<uint64>::max)();
    const uint64 LastRow = static_cast<uint64>(Rows - 1U);
    if (LastRow != 0 && Stride > (Max - RowWidth) / LastRow)
    {
        return false;
    }
    const uint64 Required = LastRow * Stride + RowWidth;
    return Required <= static_cast<uint64>(FMath::Max(Available, 0));
}

bool CheckedFileOffset(const uint64 SampleOffset, const uint64 BytesPerSample,
    int64& OutOffset)
{
    OutOffset = 0;
    const uint64 Max = static_cast<uint64>((std::numeric_limits<int64>::max)());
    if (BytesPerSample == 0 || SampleOffset > Max / BytesPerSample)
    {
        return false;
    }
    OutOffset = static_cast<int64>(SampleOffset * BytesPerSample);
    return true;
}

bool IsKnownProvenance(const uint8 Value)
{
    return Value <= static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::ArcSec13)
        || Value == static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData);
}

bool IsValidSourceTable(const TArray<SkiDomain::TerrainCoreSource>& Sources)
{
    if (Sources.IsEmpty()
        || Sources.Num() > static_cast<int32>(SkiDomain::TerrainCoreMaxAdditionalSources + 1U))
    {
        return false;
    }
    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        if (Sources[Index].SourceId.empty() || Sources[Index].Product.empty())
        {
            return false;
        }
        for (int32 Earlier = 0; Earlier < Index; ++Earlier)
        {
            if (Sources[Earlier].SourceId == Sources[Index].SourceId)
            {
                return false;
            }
        }
    }
    return true;
}

bool WriteAt(IFileHandle* File, const int64 Offset, const uint8* Bytes, const int64 Count)
{
    return File != nullptr && Bytes != nullptr && Count > 0
        && File->Seek(Offset) && File->Write(Bytes, Count);
}

bool ReadAt(IFileHandle* File, const int64 Offset, uint8* Bytes, const int64 Count)
{
    return File != nullptr && Bytes != nullptr && Count > 0
        && File->Seek(Offset) && File->Read(Bytes, Count);
}

bool SameTileGeometry(const SkiDomain::TerrainCoreTileDescriptor& A,
    const SkiDomain::TerrainCoreTileDescriptor& B)
{
    return A.LodIndex == B.LodIndex && A.LodFactor == B.LodFactor
        && A.TileX == B.TileX && A.TileY == B.TileY
        && A.StartColumn == B.StartColumn && A.StartRow == B.StartRow
        && A.CoreWidth == B.CoreWidth && A.CoreHeight == B.CoreHeight
        && A.HaloWest == B.HaloWest && A.HaloNorth == B.HaloNorth
        && A.HaloEast == B.HaloEast && A.HaloSouth == B.HaloSouth;
}

bool IsSampleRecordValid(const float Height, const uint8 Validity,
    const uint8 Provenance, const uint8 SourceIndex, const int32 SourceCount)
{
    if (Validity == 0)
    {
        return Provenance == static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData)
            && SourceIndex == SkiPreparation::TerrainScratchNoSourceIndex;
    }
    return Validity == 1 && FMath::IsFinite(Height)
        && Provenance != static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData)
        && IsKnownProvenance(Provenance) && SourceIndex < SourceCount;
}
}

SkiPreparation::TerrainScratchStore::~TerrainScratchStore()
{
    FString Ignored;
    Cleanup(Ignored);
}

bool SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(
    const uint32 WidthValue, const uint32 HeightValue, uint64& OutBytes,
    uint32* OutTilesX, uint32* OutTilesY) noexcept
{
    OutBytes = 0;
    if (OutTilesX) *OutTilesX = 0;
    if (OutTilesY) *OutTilesY = 0;
    if (WidthValue < 2 || HeightValue < 2)
    {
        return false;
    }
    const uint64 Width64 = WidthValue;
    const uint64 Height64 = HeightValue;
    if (Width64 > SkiDomain::TerrainCoreMaxSamples / Height64)
    {
        return false;
    }
    const uint64 Samples = Width64 * Height64;
    if (Samples > SkiDomain::TerrainCoreMaxSamples
        || Samples > (std::numeric_limits<uint64>::max)() / TerrainScratchBytesPerSample)
    {
        return false;
    }
    const uint64 XCount = (Width64 + TerrainScratchTileSamples - 1ULL)
        / TerrainScratchTileSamples;
    const uint64 YCount = (Height64 + TerrainScratchTileSamples - 1ULL)
        / TerrainScratchTileSamples;
    if (XCount == 0 || YCount == 0
        || XCount > (std::numeric_limits<uint32>::max)()
        || YCount > (std::numeric_limits<uint32>::max)()
        || XCount > SkiDomain::TerrainCoreMaxTiles / YCount)
    {
        return false;
    }
    const uint64 Bytes = Samples * TerrainScratchBytesPerSample;
    if (Bytes > TerrainScratchMaximumBytes
        || Bytes > static_cast<uint64>((std::numeric_limits<int64>::max)()))
    {
        return false;
    }
    OutBytes = Bytes;
    if (OutTilesX) *OutTilesX = static_cast<uint32>(XCount);
    if (OutTilesY) *OutTilesY = static_cast<uint32>(YCount);
    return true;
}

bool SkiPreparation::TerrainScratchStore::Create(const FString& ScratchDirectory,
    const uint32 WidthValue, const uint32 HeightValue, const double EastSpacing,
    const double NorthSpacing, const TArray<SkiDomain::TerrainCoreSource>& SourcesValue,
    const uint64 MaxStorageBytes, FString& OutError)
{
    FScopeLock Lock(&Mutex);
    OutError.Reset();
    if (CurrentState != EState::Empty)
    {
        OutError = TEXT("Terrain scratch store can only be created once.");
        return false;
    }
    uint64 Required = 0;
    uint32 CountX = 0;
    uint32 CountY = 0;
    if (!std::isfinite(EastSpacing) || !std::isfinite(NorthSpacing)
        || EastSpacing != 1.0 || NorthSpacing != 1.0)
    {
        OutError = TEXT("Canonical terrain scratch requires exactly 1 m sample spacing.");
        return false;
    }
    if (!TryCalculateRequiredStorageBytes(WidthValue, HeightValue, Required,
            &CountX, &CountY))
    {
        OutError = TEXT("Canonical terrain scratch dimensions or storage arithmetic are invalid.");
        return false;
    }
    if (Required > MaxStorageBytes)
    {
        OutError = FString::Printf(TEXT("Canonical terrain scratch requires %llu bytes, above the %llu-byte budget."),
            static_cast<unsigned long long>(Required),
            static_cast<unsigned long long>(MaxStorageBytes));
        return false;
    }
    if (!IsValidSourceTable(SourcesValue))
    {
        OutError = TEXT("Canonical terrain scratch needs 1-65 unique, named source records.");
        return false;
    }
    if (ScratchDirectory.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("Canonical terrain scratch directory is empty.");
        return false;
    }

    FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(ScratchDirectory);
    FPaths::NormalizeDirectoryName(AbsoluteRoot);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (PlatformFile.DirectoryExists(*AbsoluteRoot))
    {
        OutError = TEXT("Canonical terrain scratch directory already exists.");
        return false;
    }
    if (!PlatformFile.CreateDirectoryTree(*AbsoluteRoot)
        || !PlatformFile.DirectoryExists(*AbsoluteRoot))
    {
        OutError = TEXT("Cannot create canonical terrain scratch directory.");
        return false;
    }

    RootDirectory = MoveTemp(AbsoluteRoot);
    bOwnsDirectory = true;
    HeightPath = FPaths::Combine(RootDirectory, TEXT("lod0-height-f32.raw"));
    ValidityPath = FPaths::Combine(RootDirectory, TEXT("lod0-validity-u8.raw"));
    ProvenancePath = FPaths::Combine(RootDirectory, TEXT("lod0-provenance-u8.raw"));
    SourceIndexPath = FPaths::Combine(RootDirectory, TEXT("lod0-source-index-u8.raw"));
    HeightFile.Reset(PlatformFile.OpenWrite(*HeightPath, false, false));
    ValidityFile.Reset(PlatformFile.OpenWrite(*ValidityPath, false, false));
    ProvenanceFile.Reset(PlatformFile.OpenWrite(*ProvenancePath, false, false));
    SourceIndexFile.Reset(PlatformFile.OpenWrite(*SourceIndexPath, false, false));
    if (!HeightFile || !ValidityFile || !ProvenanceFile || !SourceIndexFile)
    {
        OutError = TEXT("Cannot create canonical terrain scratch planes.");
        HeightFile.Reset();
        ValidityFile.Reset();
        ProvenanceFile.Reset();
        SourceIndexFile.Reset();
        if (IFileManager::Get().DeleteDirectory(*RootDirectory, false, true))
        {
            bOwnsDirectory = false;
            RootDirectory.Reset();
        }
        else
        {
            OutError += TEXT(" Scratch cleanup also failed.");
            CurrentState = EState::Failed;
        }
        return false;
    }

    RasterWidth = WidthValue;
    RasterHeight = HeightValue;
    RasterEastSpacingM = EastSpacing;
    RasterNorthSpacingM = NorthSpacing;
    TilesX = CountX;
    TilesY = CountY;
    SampleCount = static_cast<uint64>(WidthValue) * HeightValue;
    StorageBytes = Required;
    SourceTable = SourcesValue;
    WrittenTiles.Init(0, static_cast<int32>(static_cast<uint64>(TilesX) * TilesY));
    CurrentState = EState::Writing;
    return true;
}

bool SkiPreparation::TerrainScratchStore::WriteLod0Tile(const uint32 TileX,
    const uint32 TileY, const TArrayView<const float> Heights,
    const uint64 HeightStrideSamples, const TArrayView<const uint8> Validity,
    const uint64 ValidityStrideBytes, const TArrayView<const uint8> Provenance,
    const uint64 ProvenanceStrideBytes, const TArrayView<const uint8> SourceIndices,
    const uint64 SourceStrideBytes, FString& OutError,
    const Cancellation* CancellationValue)
{
    FScopeLock Lock(&Mutex);
    OutError.Reset();
    if (CurrentState != EState::Writing)
    {
        OutError = TEXT("Canonical terrain scratch is not accepting LOD0 tiles.");
        return false;
    }
    if (TileX >= TilesX || TileY >= TilesY)
    {
        OutError = TEXT("Canonical terrain scratch tile coordinate is outside the raster.");
        return false;
    }
    const uint32 StartColumn = TileX * TerrainScratchTileSamples;
    const uint32 StartRow = TileY * TerrainScratchTileSamples;
    const uint32 TileWidth = FMath::Min(TerrainScratchTileSamples, RasterWidth - StartColumn);
    const uint32 TileHeight = FMath::Min(TerrainScratchTileSamples, RasterHeight - StartRow);
    if (!CheckedArrayLength(Heights.Num(), TileHeight, HeightStrideSamples, TileWidth)
        || !CheckedArrayLength(Validity.Num(), TileHeight, ValidityStrideBytes, TileWidth)
        || !CheckedArrayLength(Provenance.Num(), TileHeight, ProvenanceStrideBytes, TileWidth)
        || !CheckedArrayLength(SourceIndices.Num(), TileHeight, SourceStrideBytes, TileWidth))
    {
        OutError = TEXT("Canonical terrain scratch tile buffers or row strides are malformed.");
        return false;
    }
    const uint64 TileBitIndex = static_cast<uint64>(TileY) * TilesX + TileX;
    if (TileBitIndex >= static_cast<uint64>(WrittenTiles.Num())
        || WrittenTiles[static_cast<int32>(TileBitIndex)] != 0)
    {
        OutError = TEXT("Canonical terrain scratch tile has already been written.");
        return false;
    }

    for (uint32 LocalRow = 0; LocalRow < TileHeight; ++LocalRow)
    {
        const uint64 HeightRowStart = static_cast<uint64>(LocalRow) * HeightStrideSamples;
        const uint64 ValidityRowStart = static_cast<uint64>(LocalRow) * ValidityStrideBytes;
        const uint64 ProvenanceRowStart = static_cast<uint64>(LocalRow) * ProvenanceStrideBytes;
        const uint64 SourceRowStart = static_cast<uint64>(LocalRow) * SourceStrideBytes;
        for (uint32 LocalColumn = 0; LocalColumn < TileWidth; ++LocalColumn)
        {
            const float Sample = Heights[static_cast<int32>(HeightRowStart + LocalColumn)];
            const uint8 SampleValidity = Validity[static_cast<int32>(ValidityRowStart + LocalColumn)];
            const uint8 SampleProvenance = Provenance[static_cast<int32>(ProvenanceRowStart + LocalColumn)];
            const uint8 SampleSource = SourceIndices[static_cast<int32>(SourceRowStart + LocalColumn)];
            if (!IsSampleRecordValid(Sample, SampleValidity, SampleProvenance,
                    SampleSource, SourceTable.Num()))
            {
                OutError = TEXT("Canonical terrain scratch tile contains an invalid height, validity, provenance, or source record.");
                return false;
            }
        }
    }
    if (!CheckCancellation(OutError, CancellationValue))
    {
        CurrentState = EState::Failed;
        HeightFile.Reset(); ValidityFile.Reset(); ProvenanceFile.Reset(); SourceIndexFile.Reset();
        return false;
    }

    for (uint32 LocalRow = 0; LocalRow < TileHeight; ++LocalRow)
    {
        if (!CheckCancellation(OutError, CancellationValue))
        {
            CurrentState = EState::Failed;
            HeightFile.Reset(); ValidityFile.Reset(); ProvenanceFile.Reset(); SourceIndexFile.Reset();
            return false;
        }
        const uint64 GlobalRow = static_cast<uint64>(StartRow) + LocalRow;
        const uint64 SampleOffset = GlobalRow * RasterWidth + StartColumn;
        int64 HeightOffset = 0;
        int64 ByteOffset = 0;
        if (!CheckedFileOffset(SampleOffset, sizeof(float), HeightOffset)
            || !CheckedFileOffset(SampleOffset, 1, ByteOffset))
        {
            return Fail(OutError, TEXT("Canonical terrain scratch tile offset overflowed."));
        }
        const uint64 HeightRowStart = static_cast<uint64>(LocalRow) * HeightStrideSamples;
        const uint64 ValidityRowStart = static_cast<uint64>(LocalRow) * ValidityStrideBytes;
        const uint64 ProvenanceRowStart = static_cast<uint64>(LocalRow) * ProvenanceStrideBytes;
        const uint64 SourceRowStart = static_cast<uint64>(LocalRow) * SourceStrideBytes;
        const int64 HeightBytes = static_cast<int64>(TileWidth) * sizeof(float);
        if (!WriteAt(HeightFile.Get(), HeightOffset,
                reinterpret_cast<const uint8*>(&Heights[static_cast<int32>(HeightRowStart)]), HeightBytes)
            || !WriteAt(ValidityFile.Get(), ByteOffset,
                &Validity[static_cast<int32>(ValidityRowStart)], TileWidth)
            || !WriteAt(ProvenanceFile.Get(), ByteOffset,
                &Provenance[static_cast<int32>(ProvenanceRowStart)], TileWidth)
            || !WriteAt(SourceIndexFile.Get(), ByteOffset,
                &SourceIndices[static_cast<int32>(SourceRowStart)], TileWidth))
        {
            return Fail(OutError, TEXT("Cannot write canonical terrain scratch tile row."));
        }
    }
    WrittenTiles[static_cast<int32>(TileBitIndex)] = 1;
    return true;
}

bool SkiPreparation::TerrainScratchStore::Finalize(FString& OutError,
    const Cancellation* CancellationValue)
{
    FScopeLock Lock(&Mutex);
    OutError.Reset();
    if (CurrentState != EState::Writing)
    {
        OutError = TEXT("Canonical terrain scratch is not in the writing state.");
        return false;
    }
    if (!CheckCancellation(OutError, CancellationValue))
    {
        CurrentState = EState::Failed;
        HeightFile.Reset(); ValidityFile.Reset(); ProvenanceFile.Reset(); SourceIndexFile.Reset();
        return false;
    }
    for (const uint8 Written : WrittenTiles)
    {
        if (Written == 0)
        {
            OutError = TEXT("Canonical terrain scratch cannot finalize with missing LOD0 tiles.");
            return false;
        }
    }
    HeightFile->Flush();
    ValidityFile->Flush();
    ProvenanceFile->Flush();
    SourceIndexFile->Flush();
    HeightFile.Reset(); ValidityFile.Reset(); ProvenanceFile.Reset(); SourceIndexFile.Reset();

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const uint64 ExpectedHeightBytes = SampleCount * sizeof(float);
    const uint64 ExpectedOtherBytes = SampleCount;
    if (PlatformFile.FileSize(*HeightPath) != static_cast<int64>(ExpectedHeightBytes)
        || PlatformFile.FileSize(*ValidityPath) != static_cast<int64>(ExpectedOtherBytes)
        || PlatformFile.FileSize(*ProvenancePath) != static_cast<int64>(ExpectedOtherBytes)
        || PlatformFile.FileSize(*SourceIndexPath) != static_cast<int64>(ExpectedOtherBytes))
    {
        return Fail(OutError, TEXT("Canonical terrain scratch plane length does not match its checked dimensions."));
    }
    HeightFile.Reset(PlatformFile.OpenRead(*HeightPath));
    ValidityFile.Reset(PlatformFile.OpenRead(*ValidityPath));
    ProvenanceFile.Reset(PlatformFile.OpenRead(*ProvenancePath));
    SourceIndexFile.Reset(PlatformFile.OpenRead(*SourceIndexPath));
    if (!HeightFile || !ValidityFile || !ProvenanceFile || !SourceIndexFile)
    {
        return Fail(OutError, TEXT("Cannot reopen canonical terrain scratch planes for LOD copying."));
    }
    CurrentState = EState::Finalized;
    return true;
}

bool SkiPreparation::TerrainScratchStore::ReadLodTile(
    const SkiDomain::TerrainCoreTileDescriptor& Planned, TerrainScratchLodTile& OutTile,
    FString& OutError, const Cancellation* CancellationValue) const
{
    OutTile = {};
    FScopeLock Lock(&Mutex);
    OutError.Reset();
    if (CurrentState != EState::Finalized)
    {
        OutError = TEXT("Canonical terrain scratch must be finalized before LOD reads.");
        return false;
    }
    if (!CheckCancellation(OutError, CancellationValue))
    {
        return false;
    }

    SkiDomain::TerrainCoreTilePlan Plan;
    if (!SkiDomain::PlanTerrainCoreTiles(RasterWidth, RasterHeight, Plan))
    {
        OutError = TEXT("Canonical terrain scratch dimensions no longer produce a TerrainCore tile plan.");
        return false;
    }
    const auto Found = std::find_if(Plan.Tiles.begin(), Plan.Tiles.end(),
        [&Planned](const SkiDomain::TerrainCoreTileDescriptor& Candidate)
        {
            return Candidate.LodIndex == Planned.LodIndex && Candidate.TileX == Planned.TileX
                && Candidate.TileY == Planned.TileY;
        });
    if (Found == Plan.Tiles.end() || !SameTileGeometry(*Found, Planned))
    {
        OutError = TEXT("Requested LOD tile does not match the canonical TerrainCore tile plan.");
        return false;
    }

    const uint64 SampleTotal = TerrainCoreStoredSampleCount(Planned);
    if (SampleTotal == 0 || SampleTotal > SkiDomain::TerrainCoreMaxStoredSamples
        || SampleTotal > static_cast<uint64>(MAX_int32))
    {
        OutError = TEXT("Requested canonical LOD tile has invalid stored dimensions.");
        return false;
    }
    const uint32 StoredWidth = Planned.CoreWidth + Planned.HaloWest + Planned.HaloEast;
    const uint32 StoredHeight = Planned.CoreHeight + Planned.HaloNorth + Planned.HaloSouth;
    const int64 FirstLodColumnSigned = static_cast<int64>(Planned.StartColumn) - Planned.HaloWest;
    const int64 FirstLodRowSigned = static_cast<int64>(Planned.StartRow) - Planned.HaloNorth;
    if (FirstLodColumnSigned < 0 || FirstLodRowSigned < 0)
    {
        OutError = TEXT("Planned canonical LOD tile halo crosses the raster boundary.");
        return false;
    }

    OutTile.Descriptor = Planned;
    OutTile.Width = static_cast<uint16>(StoredWidth);
    OutTile.Height = static_cast<uint16>(StoredHeight);
    OutTile.Heights.SetNumUninitialized(static_cast<int32>(SampleTotal));
    OutTile.Validity.SetNumUninitialized(static_cast<int32>(SampleTotal));
    OutTile.Provenance.SetNumUninitialized(static_cast<int32>(SampleTotal));
    OutTile.SourceIndices.SetNumUninitialized(static_cast<int32>(SampleTotal));

    const uint64 FirstLodColumn = static_cast<uint64>(FirstLodColumnSigned);
    const uint64 FirstLodRow = static_cast<uint64>(FirstLodRowSigned);
    for (uint32 LocalRow = 0; LocalRow < StoredHeight; ++LocalRow)
    {
        if (!CheckCancellation(OutError, CancellationValue))
        {
            OutTile = {};
            return false;
        }
        const uint64 LodRow = FirstLodRow + LocalRow;
        const uint64 UnclampedSourceRow = LodRow * Planned.LodFactor;
        const uint64 SourceRow = FMath::Min<uint64>(UnclampedSourceRow, RasterHeight - 1ULL);
        const uint64 FirstUnclampedColumn = FirstLodColumn * Planned.LodFactor;
        const uint64 LastLodColumn = FirstLodColumn + StoredWidth - 1ULL;
        const uint64 LastUnclampedColumn = LastLodColumn * Planned.LodFactor;
        const uint64 SourceFirstColumn = FMath::Min<uint64>(FirstUnclampedColumn, RasterWidth - 1ULL);
        const uint64 SourceLastColumn = FMath::Min<uint64>(LastUnclampedColumn, RasterWidth - 1ULL);
        if (SourceLastColumn < SourceFirstColumn)
        {
            OutTile = {};
            OutError = TEXT("Canonical LOD source span is invalid.");
            return false;
        }
        const uint64 SourceSpan = SourceLastColumn - SourceFirstColumn + 1ULL;
        if (SourceSpan == 0 || SourceSpan > static_cast<uint64>(MAX_int32)
            || SourceSpan > (std::numeric_limits<uint64>::max)() / sizeof(float))
        {
            OutTile = {};
            OutError = TEXT("Canonical LOD source span exceeds its checked row buffer.");
            return false;
        }
        const uint64 SourceSampleOffset = SourceRow * RasterWidth + SourceFirstColumn;
        int64 HeightOffset = 0;
        int64 ByteOffset = 0;
        if (!CheckedFileOffset(SourceSampleOffset, sizeof(float), HeightOffset)
            || !CheckedFileOffset(SourceSampleOffset, 1, ByteOffset))
        {
            OutTile = {};
            OutError = TEXT("Canonical LOD source offset overflowed.");
            return false;
        }
        TArray<float> SpanHeights;
        TArray<uint8> SpanValidity;
        TArray<uint8> SpanProvenance;
        TArray<uint8> SpanSourceIndices;
        SpanHeights.SetNumUninitialized(static_cast<int32>(SourceSpan));
        SpanValidity.SetNumUninitialized(static_cast<int32>(SourceSpan));
        SpanProvenance.SetNumUninitialized(static_cast<int32>(SourceSpan));
        SpanSourceIndices.SetNumUninitialized(static_cast<int32>(SourceSpan));
        const int64 HeightBytes = static_cast<int64>(SourceSpan * sizeof(float));
        const int64 ByteCount = static_cast<int64>(SourceSpan);
        if (!ReadAt(HeightFile.Get(), HeightOffset,
                reinterpret_cast<uint8*>(SpanHeights.GetData()), HeightBytes)
            || !ReadAt(ValidityFile.Get(), ByteOffset, SpanValidity.GetData(), ByteCount)
            || !ReadAt(ProvenanceFile.Get(), ByteOffset, SpanProvenance.GetData(), ByteCount)
            || !ReadAt(SourceIndexFile.Get(), ByteOffset, SpanSourceIndices.GetData(), ByteCount))
        {
            OutTile = {};
            OutError = TEXT("Canonical LOD0 scratch row read failed.");
            return false;
        }
        for (uint32 LocalColumn = 0; LocalColumn < StoredWidth; ++LocalColumn)
        {
            const uint64 LodColumn = FirstLodColumn + LocalColumn;
            const uint64 UnclampedSourceColumn = LodColumn * Planned.LodFactor;
            const uint64 SourceColumn = FMath::Min<uint64>(UnclampedSourceColumn,
                RasterWidth - 1ULL);
            const uint64 SpanIndex = SourceColumn - SourceFirstColumn;
            const int32 TargetIndex = static_cast<int32>(static_cast<uint64>(LocalRow)
                * StoredWidth + LocalColumn);
            FMemory::Memcpy(&OutTile.Heights[TargetIndex],
                &SpanHeights[static_cast<int32>(SpanIndex)], sizeof(float));
            OutTile.Validity[TargetIndex] = SpanValidity[static_cast<int32>(SpanIndex)];
            OutTile.Provenance[TargetIndex] = SpanProvenance[static_cast<int32>(SpanIndex)];
            OutTile.SourceIndices[TargetIndex] = SpanSourceIndices[static_cast<int32>(SpanIndex)];
            if (!IsSampleRecordValid(OutTile.Heights[TargetIndex], OutTile.Validity[TargetIndex],
                    OutTile.Provenance[TargetIndex], OutTile.SourceIndices[TargetIndex], SourceTable.Num()))
            {
                OutTile = {};
                OutError = TEXT("Canonical LOD0 scratch contains a malformed sample record.");
                return false;
            }
        }
    }
    return true;
}

bool SkiPreparation::TerrainScratchStore::Cleanup(FString& OutError)
{
    FScopeLock Lock(&Mutex);
    OutError.Reset();
    HeightFile.Reset();
    ValidityFile.Reset();
    ProvenanceFile.Reset();
    SourceIndexFile.Reset();
    WrittenTiles.Reset();
    if (!bOwnsDirectory)
    {
        CurrentState = EState::Cleaned;
        return true;
    }
    if (!IFileManager::Get().DeleteDirectory(*RootDirectory, false, true))
    {
        OutError = TEXT("Cannot remove the canonical terrain scratch directory.");
        CurrentState = EState::Failed;
        return false;
    }
    bOwnsDirectory = false;
    RootDirectory.Reset();
    HeightPath.Reset();
    ValidityPath.Reset();
    ProvenancePath.Reset();
    SourceIndexPath.Reset();
    SourceTable.Reset();
    RasterWidth = 0;
    RasterHeight = 0;
    TilesX = 0;
    TilesY = 0;
    SampleCount = 0;
    StorageBytes = 0;
    CurrentState = EState::Cleaned;
    return true;
}

bool SkiPreparation::TerrainScratchStore::Fail(FString& OutError, const TCHAR* Message)
{
    OutError = Message;
    CurrentState = EState::Failed;
    HeightFile.Reset();
    ValidityFile.Reset();
    ProvenanceFile.Reset();
    SourceIndexFile.Reset();
    return false;
}

bool SkiPreparation::TerrainScratchStore::CheckCancellation(FString& OutError,
    const Cancellation* CancellationValue) const
{
    if (CancellationValue == nullptr || !CancellationValue->IsCancelled())
    {
        return true;
    }
    OutError = TEXT("Canonical terrain scratch operation was cancelled.");
    return false;
}
