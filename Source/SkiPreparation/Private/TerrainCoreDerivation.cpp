#include "SkiPreparation/TerrainCoreDerivation.h"

#include "SkiPreparation/TerrainPackageStore.h"

#include <cmath>
#include <limits>
#include <zlib.h>

namespace
{
constexpr int TerrainCoreCompressionLevel = Z_BEST_COMPRESSION;

bool IsNoData(const float Value, const double NoData) noexcept
{
    return !std::isfinite(Value) || Value == static_cast<float>(NoData);
}

bool Compress(const TArrayView<const uint8> Raw, TArray<uint8>& Out, FString& Error)
{
    Out.Reset();
    if (Raw.Num() <= 0)
    {
        Error = TEXT("TerrainCore tile has no raw data.");
        return false;
    }
    const uLongf Bound = compressBound(static_cast<uLong>(Raw.Num()));
    if (Bound > static_cast<uLongf>(MAX_int32))
    {
        Error = TEXT("TerrainCore tile compression bound exceeds the runtime array limit.");
        return false;
    }
    Out.SetNumUninitialized(static_cast<int32>(Bound));
    uLongf Written = Bound;
    const int Result = compress2(Out.GetData(), &Written, Raw.GetData(),
        static_cast<uLong>(Raw.Num()), TerrainCoreCompressionLevel);
    if (Result != Z_OK || Written == 0 || Written > static_cast<uLongf>(MAX_int32))
    {
        Out.Reset();
        Error = FString::Printf(TEXT("TerrainCore tile compression failed (%d)."), Result);
        return false;
    }
    Out.SetNum(static_cast<int32>(Written), EAllowShrinking::Yes);
    return true;
}

bool Decompress(const TArrayView<const uint8> Compressed, const uint64 RawBytes,
    TArray<uint8>& Out, FString& Error)
{
    Out.Reset();
    if (Compressed.IsEmpty() || RawBytes == 0 || RawBytes > MAX_int32)
    {
        Error = TEXT("TerrainCore tile compressed or raw length is invalid.");
        return false;
    }
    Out.SetNumUninitialized(static_cast<int32>(RawBytes));
    uLongf Written = static_cast<uLongf>(RawBytes);
    const int Result = uncompress(Out.GetData(), &Written, Compressed.GetData(),
        static_cast<uLong>(Compressed.Num()));
    if (Result != Z_OK || Written != RawBytes)
    {
        Out.Reset();
        Error = FString::Printf(TEXT("TerrainCore tile decompression failed (%d)."), Result);
        return false;
    }
    return true;
}
}

uint64 SkiPreparation::TerrainCoreStoredSampleCount(
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor) noexcept
{
    const uint64 Width = static_cast<uint64>(Descriptor.CoreWidth)
        + Descriptor.HaloWest + Descriptor.HaloEast;
    const uint64 Height = static_cast<uint64>(Descriptor.CoreHeight)
        + Descriptor.HaloNorth + Descriptor.HaloSouth;
    if (Width == 0 || Height == 0 || Width > SkiDomain::TerrainCoreTileSamples + 2ULL
        || Height > SkiDomain::TerrainCoreTileSamples + 2ULL
        || Width > (std::numeric_limits<uint64>::max)() / Height)
    {
        return 0;
    }
    return Width * Height;
}

bool SkiPreparation::DeriveTerrainCoreTile(const SkiDomain::Heightfield& Finest,
    const SkiDomain::TerrainCoreTileDescriptor& Planned,
    TerrainCoreEncodedTile& OutTile, FString& OutError)
{
    OutTile = {};
    OutError.Reset();
    if (!SkiDomain::IsValidHeightfield(Finest)
        || Planned.LodIndex >= SkiDomain::TerrainCoreLodFactors.size()
        || Planned.LodFactor != SkiDomain::TerrainCoreLodFactors[Planned.LodIndex])
    {
        OutError = TEXT("TerrainCore derivation input or LOD is invalid.");
        return false;
    }
    const uint64 StoredSamples = TerrainCoreStoredSampleCount(Planned);
    if (StoredSamples == 0 || StoredSamples > MAX_int32)
    {
        OutError = TEXT("TerrainCore planned tile dimensions are invalid.");
        return false;
    }
    const int64 FirstLodColumn = static_cast<int64>(Planned.StartColumn) - Planned.HaloWest;
    const int64 FirstLodRow = static_cast<int64>(Planned.StartRow) - Planned.HaloNorth;
    if (FirstLodColumn < 0 || FirstLodRow < 0)
    {
        OutError = TEXT("TerrainCore planned tile halo crosses the raster boundary.");
        return false;
    }
    const uint32 StoredWidth = Planned.CoreWidth + Planned.HaloWest + Planned.HaloEast;
    const uint32 StoredHeight = Planned.CoreHeight + Planned.HaloNorth + Planned.HaloSouth;
    TArray<float> Heights;
    TArray<uint8> Validity;
    Heights.SetNumUninitialized(static_cast<int32>(StoredSamples));
    Validity.Init(0, static_cast<int32>((StoredSamples + 7ULL) / 8ULL));
    for (uint32 LocalRow = 0; LocalRow < StoredHeight; ++LocalRow)
    {
        const uint64 LodRow = static_cast<uint64>(FirstLodRow) + LocalRow;
        const uint64 SourceRow = FMath::Min<uint64>(LodRow * Planned.LodFactor,
            static_cast<uint64>(Finest.Height - 1U));
        for (uint32 LocalColumn = 0; LocalColumn < StoredWidth; ++LocalColumn)
        {
            const uint64 LodColumn = static_cast<uint64>(FirstLodColumn) + LocalColumn;
            const uint64 SourceColumn = FMath::Min<uint64>(LodColumn * Planned.LodFactor,
                static_cast<uint64>(Finest.Width - 1U));
            const uint64 SourceIndex = SourceRow * Finest.Width + SourceColumn;
            const int32 TargetIndex = static_cast<int32>(LocalRow * StoredWidth + LocalColumn);
            const float Value = Finest.Samples[SourceIndex];
            Heights[TargetIndex] = Value;
            if (!IsNoData(Value, Finest.NoDataValue))
            {
                Validity[TargetIndex / 8] |= static_cast<uint8>(1U << (TargetIndex % 8));
            }
        }
    }
    const uint64 HeightRawBytes = StoredSamples * sizeof(float);
    TArrayView<const uint8> HeightView(reinterpret_cast<const uint8*>(Heights.GetData()),
        static_cast<int32>(HeightRawBytes));
    if (!Compress(HeightView, OutTile.CompressedHeights, OutError)
        || !Compress(Validity, OutTile.CompressedValidity, OutError))
    {
        OutTile = {};
        return false;
    }
    OutTile.Descriptor = Planned;
    OutTile.Descriptor.HeightBytes = static_cast<uint64>(OutTile.CompressedHeights.Num());
    OutTile.Descriptor.HeightRawBytes = HeightRawBytes;
    OutTile.Descriptor.ValidityBytes = static_cast<uint64>(OutTile.CompressedValidity.Num());
    OutTile.Descriptor.ValidityRawBytes = static_cast<uint64>(Validity.Num());
    OutTile.Descriptor.HeightSha256 = TCHAR_TO_UTF8(
        *Sha256(MakeArrayView(OutTile.CompressedHeights)));
    OutTile.Descriptor.ValiditySha256 = TCHAR_TO_UTF8(
        *Sha256(MakeArrayView(OutTile.CompressedValidity)));
    return true;
}

bool SkiPreparation::DecodeTerrainCoreTile(
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    const TArrayView<const uint8> CompressedHeights,
    const TArrayView<const uint8> CompressedValidity,
    TerrainCoreDecodedTile& OutTile, FString& OutError)
{
    OutTile = {};
    OutError.Reset();
    const uint64 Samples = TerrainCoreStoredSampleCount(Descriptor);
    if (Samples == 0 || Samples > MAX_int32
        || Descriptor.HeightBytes != static_cast<uint64>(CompressedHeights.Num())
        || Descriptor.ValidityBytes != static_cast<uint64>(CompressedValidity.Num()))
    {
        OutError = TEXT("TerrainCore tile descriptor lengths are inconsistent.");
        return false;
    }
    TArray<uint8> HeightBytes;
    TArray<uint8> ValidityBytes;
    if (Descriptor.HeightRawBytes != Samples * sizeof(float)
        || Descriptor.ValidityRawBytes != (Samples + 7ULL) / 8ULL
        || !Decompress(CompressedHeights, Descriptor.HeightRawBytes, HeightBytes, OutError)
        || !Decompress(CompressedValidity, Descriptor.ValidityRawBytes, ValidityBytes, OutError))
    {
        return false;
    }
    if (Samples % 8ULL != 0)
    {
        const uint8 UsedMask = static_cast<uint8>((1U << (Samples % 8ULL)) - 1U);
        if ((ValidityBytes.Last() & static_cast<uint8>(~UsedMask)) != 0)
        {
            OutError = TEXT("TerrainCore validity bitset has nonzero padding bits.");
            return false;
        }
    }
    OutTile.Descriptor = Descriptor;
    OutTile.StoredWidth = Descriptor.CoreWidth + Descriptor.HaloWest + Descriptor.HaloEast;
    OutTile.StoredHeight = Descriptor.CoreHeight + Descriptor.HaloNorth + Descriptor.HaloSouth;
    OutTile.Heights.SetNumUninitialized(static_cast<int32>(Samples));
    FMemory::Memcpy(OutTile.Heights.GetData(), HeightBytes.GetData(), HeightBytes.Num());
    OutTile.Validity.SetNumUninitialized(static_cast<int32>(Samples));
    for (int32 Index = 0; Index < OutTile.Validity.Num(); ++Index)
    {
        OutTile.Validity[Index] = (ValidityBytes[Index / 8] & (1U << (Index % 8))) != 0 ? 1 : 0;
    }
    for (int32 Index = 0; Index < OutTile.Heights.Num(); ++Index)
    {
        if (OutTile.Validity[Index] != 0 && !FMath::IsFinite(OutTile.Heights[Index]))
        {
            OutTile = {};
            OutError = TEXT("TerrainCore valid height sample is nonfinite.");
            return false;
        }
    }
    return true;
}
