#include "SkiApplication/TerrainCoreRepository.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace
{
constexpr std::array<std::uint32_t, 64> Sha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

std::uint32_t RotateRight(const std::uint32_t Value, const std::uint32_t Bits) noexcept
{
    return (Value >> Bits) | (Value << (32U - Bits));
}

std::array<std::uint8_t, 32> Sha256(const std::uint8_t* Data, const std::size_t Size)
{
    std::array<std::uint32_t, 8> Hash{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    const std::size_t PaddedSize = ((Size + 9U + 63U) / 64U) * 64U;
    std::vector<std::uint8_t> Padded(PaddedSize, 0U);
    if (Size != 0) std::copy_n(Data, Size, Padded.begin());
    Padded[Size] = 0x80U;
    const std::uint64_t BitLength = static_cast<std::uint64_t>(Size) * 8ULL;
    for (std::uint32_t Index = 0; Index < 8U; ++Index)
    {
        Padded[PaddedSize - 1U - Index] = static_cast<std::uint8_t>(BitLength >> (Index * 8U));
    }

    for (std::size_t BlockOffset = 0; BlockOffset < Padded.size(); BlockOffset += 64U)
    {
        std::array<std::uint32_t, 64> Words{};
        for (std::uint32_t Index = 0; Index < 16U; ++Index)
        {
            const std::size_t Offset = BlockOffset + Index * 4U;
            Words[Index] = (static_cast<std::uint32_t>(Padded[Offset]) << 24U)
                | (static_cast<std::uint32_t>(Padded[Offset + 1U]) << 16U)
                | (static_cast<std::uint32_t>(Padded[Offset + 2U]) << 8U)
                | static_cast<std::uint32_t>(Padded[Offset + 3U]);
        }
        for (std::uint32_t Index = 16U; Index < 64U; ++Index)
        {
            const std::uint32_t S0 = RotateRight(Words[Index - 15U], 7U)
                ^ RotateRight(Words[Index - 15U], 18U) ^ (Words[Index - 15U] >> 3U);
            const std::uint32_t S1 = RotateRight(Words[Index - 2U], 17U)
                ^ RotateRight(Words[Index - 2U], 19U) ^ (Words[Index - 2U] >> 10U);
            Words[Index] = Words[Index - 16U] + S0 + Words[Index - 7U] + S1;
        }

        std::uint32_t A = Hash[0], B = Hash[1], C = Hash[2], D = Hash[3];
        std::uint32_t E = Hash[4], F = Hash[5], G = Hash[6], H = Hash[7];
        for (std::uint32_t Index = 0; Index < 64U; ++Index)
        {
            const std::uint32_t Sum1 = RotateRight(E, 6U) ^ RotateRight(E, 11U)
                ^ RotateRight(E, 25U);
            const std::uint32_t Choice = (E & F) ^ (~E & G);
            const std::uint32_t Temp1 = H + Sum1 + Choice + Sha256RoundConstants[Index]
                + Words[Index];
            const std::uint32_t Sum0 = RotateRight(A, 2U) ^ RotateRight(A, 13U)
                ^ RotateRight(A, 22U);
            const std::uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
            const std::uint32_t Temp2 = Sum0 + Majority;
            H = G; G = F; F = E; E = D + Temp1;
            D = C; C = B; B = A; A = Temp1 + Temp2;
        }
        Hash[0] += A; Hash[1] += B; Hash[2] += C; Hash[3] += D;
        Hash[4] += E; Hash[5] += F; Hash[6] += G; Hash[7] += H;
    }

    std::array<std::uint8_t, 32> Digest{};
    for (std::uint32_t Index = 0; Index < Hash.size(); ++Index)
    {
        Digest[Index * 4U] = static_cast<std::uint8_t>(Hash[Index] >> 24U);
        Digest[Index * 4U + 1U] = static_cast<std::uint8_t>(Hash[Index] >> 16U);
        Digest[Index * 4U + 2U] = static_cast<std::uint8_t>(Hash[Index] >> 8U);
        Digest[Index * 4U + 3U] = static_cast<std::uint8_t>(Hash[Index]);
    }
    return Digest;
}

std::string HexDigest(const std::array<std::uint8_t, 32>& Digest)
{
    constexpr char Hex[] = "0123456789abcdef";
    std::string Result;
    Result.reserve(64U);
    for (const std::uint8_t Byte : Digest)
    {
        Result.push_back(Hex[Byte >> 4U]);
        Result.push_back(Hex[Byte & 0x0fU]);
    }
    return Result;
}

template <typename UnsignedType>
void AppendLittleEndian(std::vector<std::uint8_t>& Bytes, const UnsignedType Value)
{
    static_assert(std::is_unsigned<UnsignedType>::value, "wire integers must be unsigned");
    for (std::size_t Index = 0; Index < sizeof(UnsignedType); ++Index)
    {
        Bytes.push_back(static_cast<std::uint8_t>(Value >> (Index * 8U)));
    }
}

void AppendDoubleLittleEndian(std::vector<std::uint8_t>& Bytes, const double Value)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t), "TerrainCore requires IEEE binary64");
    std::uint64_t Bits = 0;
    std::memcpy(&Bits, &Value, sizeof(Bits));
    AppendLittleEndian(Bytes, Bits);
}

template <typename UnsignedType>
bool ReadLittleEndian(const std::vector<std::uint8_t>& Bytes, std::size_t& Cursor,
    const std::size_t Limit, UnsignedType& OutValue)
{
    static_assert(std::is_unsigned<UnsignedType>::value, "wire integers must be unsigned");
    if (Cursor > Limit || sizeof(UnsignedType) > Limit - Cursor) return false;
    OutValue = 0;
    for (std::size_t Index = 0; Index < sizeof(UnsignedType); ++Index)
    {
        OutValue |= static_cast<UnsignedType>(Bytes[Cursor++]) << (Index * 8U);
    }
    return true;
}

std::uint64_t SourceTableSize(const SkiDomain::TerrainCoreManifest& Manifest);

std::string GridSha256(const SkiDomain::TerrainCoreManifest& Manifest)
{
    std::vector<std::uint8_t> Bytes;
    constexpr char Marker[] = "TerrainCoreGridProvenance-v1";
    Bytes.insert(Bytes.end(), Marker, Marker + sizeof(Marker) - 1U);
    Bytes.insert(Bytes.end(), Manifest.ContentId.begin(), Manifest.ContentId.end());
    AppendLittleEndian(Bytes, Manifest.Width);
    AppendLittleEndian(Bytes, Manifest.Height);
    AppendDoubleLittleEndian(Bytes, Manifest.DeliveredEastSpacingM);
    AppendDoubleLittleEndian(Bytes, Manifest.DeliveredNorthSpacingM);
    Bytes.push_back(static_cast<std::uint8_t>(Manifest.Registration));
    AppendDoubleLittleEndian(Bytes, Manifest.SampleCenterBounds.WestM);
    AppendDoubleLittleEndian(Bytes, Manifest.SampleCenterBounds.SouthM);
    AppendDoubleLittleEndian(Bytes, Manifest.SampleCenterBounds.EastM);
    AppendDoubleLittleEndian(Bytes, Manifest.SampleCenterBounds.NorthM);
    AppendDoubleLittleEndian(Bytes, Manifest.OuterBounds.WestM);
    AppendDoubleLittleEndian(Bytes, Manifest.OuterBounds.SouthM);
    AppendDoubleLittleEndian(Bytes, Manifest.OuterBounds.EastM);
    AppendDoubleLittleEndian(Bytes, Manifest.OuterBounds.NorthM);
    AppendLittleEndian(Bytes, static_cast<std::uint32_t>(Manifest.RowOrientation.size()));
    Bytes.insert(Bytes.end(), Manifest.RowOrientation.begin(), Manifest.RowOrientation.end());
    return HexDigest(Sha256(Bytes.data(), Bytes.size()));
}

std::string SourceDictionarySha256(const SkiDomain::TerrainCoreManifest& Manifest)
{
    std::vector<std::uint8_t> Bytes;
    constexpr char Marker[] = "TerrainCoreSourceDictionary-v1";
    Bytes.insert(Bytes.end(), Marker, Marker + sizeof(Marker) - 1U);
    AppendLittleEndian(Bytes, static_cast<std::uint32_t>(SourceTableSize(Manifest)));
    const auto AppendSource = [&Bytes](const SkiDomain::TerrainCoreSource& Source)
    {
        AppendLittleEndian(Bytes, static_cast<std::uint32_t>(Source.SourceId.size()));
        Bytes.insert(Bytes.end(), Source.SourceId.begin(), Source.SourceId.end());
        AppendLittleEndian(Bytes, static_cast<std::uint32_t>(Source.Product.size()));
        Bytes.insert(Bytes.end(), Source.Product.begin(), Source.Product.end());
    };
    AppendSource(Manifest.Source);
    for (const SkiDomain::TerrainCoreSource& Source : Manifest.AdditionalSources)
    {
        AppendSource(Source);
    }
    return HexDigest(Sha256(Bytes.data(), Bytes.size()));
}

std::uint64_t TileStoredSampleCount(const SkiDomain::TerrainCoreTileDescriptor& Descriptor)
{
    const std::uint64_t Width = static_cast<std::uint64_t>(Descriptor.CoreWidth)
        + Descriptor.HaloWest + Descriptor.HaloEast;
    const std::uint64_t Height = static_cast<std::uint64_t>(Descriptor.CoreHeight)
        + Descriptor.HaloNorth + Descriptor.HaloSouth;
    return Width * Height;
}

std::uint64_t SourceTableSize(const SkiDomain::TerrainCoreManifest& Manifest)
{
    return 1ULL + Manifest.AdditionalSources.size();
}

bool ValidProvenanceSample(const std::uint8_t Validity, const std::uint8_t Provenance,
    const std::uint8_t SourceIndex, const std::uint64_t SourceCount)
{
    if (Validity == 0U)
    {
        return Provenance == static_cast<std::uint8_t>(SkiApplication::TerrainSampleProvenance::NoData)
            && SourceIndex == SkiApplication::TerrainCoreNoSourceIndex;
    }
    return Validity == 1U
        && Provenance <= static_cast<std::uint8_t>(SkiApplication::TerrainSampleProvenance::ArcSec13)
        && SourceIndex < SourceCount;
}

const SkiDomain::TerrainCoreTileDescriptor* FindTile(
    const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiApplication::TerrainCoreTileKey& Key)
{
    const auto It = std::find_if(Manifest.Tiles.begin(), Manifest.Tiles.end(),
        [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
        {
            return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
        });
    return It == Manifest.Tiles.end() ? nullptr : &*It;
}

bool ReadDescriptorTile(const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiApplication::TerrainCoreTileKey& Key,
    SkiDomain::TerrainCoreTileDescriptor& OutDescriptor, std::string& OutError)
{
    if (Key.Lod != 0U || Key.Lod >= SkiDomain::TerrainCoreLodFactors.size())
    {
        OutError = "terrain provenance sidecars are only defined for LOD0 tiles";
        return false;
    }
    const SkiDomain::TerrainCoreTileDescriptor* Descriptor = FindTile(Manifest, Key);
    if (!Descriptor)
    {
        OutError = "TerrainCore provenance tile not found";
        return false;
    }
    OutDescriptor = *Descriptor;
    return true;
}

struct SidecarSamples
{
    const std::uint8_t* Provenance = nullptr;
    const std::uint8_t* SourceIndices = nullptr;
    std::size_t SampleCount = 0;
};

bool ValidateSidecar(const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiApplication::TerrainCoreTileKey& Key,
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    const std::vector<std::uint8_t>& Validity,
    const std::vector<std::uint8_t>& Bytes,
    const std::string& ExpectedGridSha256,
    SidecarSamples& OutSamples, std::string& OutError)
{
    OutSamples = {};
    const std::uint64_t ExpectedSamples = TileStoredSampleCount(Descriptor);
    if (ExpectedSamples == 0U || ExpectedSamples > SkiDomain::TerrainCoreMaxStoredSamples
        || Validity.size() != ExpectedSamples)
    {
        OutError = "TerrainCore provenance validity sample count does not match the tile";
        return false;
    }
    const std::uint64_t ExpectedBytes = 265ULL + ExpectedSamples * 2ULL;
    if (ExpectedBytes > SkiApplication::TerrainCoreProvenanceSidecarMaxBytes
        || Bytes.size() != ExpectedBytes)
    {
        OutError = "TerrainCore provenance sidecar is truncated or has an invalid length";
        return false;
    }
    const std::size_t DigestOffset = Bytes.size() - 32U;
    const std::array<std::uint8_t, 32> Digest = Sha256(Bytes.data(), DigestOffset);
    if (!std::equal(Digest.begin(), Digest.end(), Bytes.begin() + DigestOffset))
    {
        OutError = "TerrainCore provenance sidecar integrity hash mismatch";
        return false;
    }

    std::size_t Cursor = 0;
    const std::size_t BodyLimit = DigestOffset;
    constexpr std::array<std::uint8_t, 4> Magic{'T', 'C', 'P', '1'};
    if (!std::equal(Magic.begin(), Magic.end(), Bytes.begin()))
    {
        OutError = "TerrainCore provenance sidecar magic is invalid";
        return false;
    }
    Cursor += Magic.size();
    std::uint32_t Schema = 0;
    if (!ReadLittleEndian(Bytes, Cursor, BodyLimit, Schema)
        || Schema != SkiApplication::TerrainCoreProvenanceSidecarSchema)
    {
        OutError = "TerrainCore provenance sidecar schema is unsupported";
        return false;
    }
    constexpr std::size_t ShaTextBytes = 64U;
    if (Cursor > BodyLimit || ShaTextBytes * 3U > BodyLimit - Cursor)
    {
        OutError = "TerrainCore provenance sidecar header is truncated";
        return false;
    }
    const std::string TerrainCoreId(Bytes.begin() + Cursor, Bytes.begin() + Cursor + ShaTextBytes);
    Cursor += ShaTextBytes;
    const std::string GridIdentity(Bytes.begin() + Cursor, Bytes.begin() + Cursor + ShaTextBytes);
    Cursor += ShaTextBytes;
    const std::string SourceDictionaryIdentity(Bytes.begin() + Cursor,
        Bytes.begin() + Cursor + ShaTextBytes);
    Cursor += ShaTextBytes;
    if (TerrainCoreId != Manifest.ContentId || GridIdentity != ExpectedGridSha256
        || SourceDictionaryIdentity != SourceDictionarySha256(Manifest))
    {
        OutError = "TerrainCore provenance sidecar belongs to different content, grid, or source dictionary";
        return false;
    }

    std::uint8_t Lod = 0;
    std::uint32_t TileX = 0, TileY = 0, StartColumn = 0, StartRow = 0;
    std::uint16_t CoreWidth = 0, CoreHeight = 0;
    std::uint8_t HaloWest = 0, HaloNorth = 0, HaloEast = 0, HaloSouth = 0;
    std::uint64_t SampleCount = 0;
    if (Cursor >= BodyLimit)
    {
        OutError = "TerrainCore provenance sidecar tile key is truncated";
        return false;
    }
    Lod = Bytes[Cursor++];
    if (!ReadLittleEndian(Bytes, Cursor, BodyLimit, TileX)
        || !ReadLittleEndian(Bytes, Cursor, BodyLimit, TileY)
        || !ReadLittleEndian(Bytes, Cursor, BodyLimit, StartColumn)
        || !ReadLittleEndian(Bytes, Cursor, BodyLimit, StartRow)
        || !ReadLittleEndian(Bytes, Cursor, BodyLimit, CoreWidth)
        || !ReadLittleEndian(Bytes, Cursor, BodyLimit, CoreHeight)
        || Cursor > BodyLimit || 4U > BodyLimit - Cursor)
    {
        OutError = "TerrainCore provenance sidecar tile geometry is truncated";
        return false;
    }
    HaloWest = Bytes[Cursor++];
    HaloNorth = Bytes[Cursor++];
    HaloEast = Bytes[Cursor++];
    HaloSouth = Bytes[Cursor++];
    if (!ReadLittleEndian(Bytes, Cursor, BodyLimit, SampleCount))
    {
        OutError = "TerrainCore provenance sidecar sample count is truncated";
        return false;
    }
    if (Lod != Key.Lod || TileX != Key.X || TileY != Key.Y
        || StartColumn != Descriptor.StartColumn || StartRow != Descriptor.StartRow
        || CoreWidth != Descriptor.CoreWidth || CoreHeight != Descriptor.CoreHeight
        || HaloWest != Descriptor.HaloWest || HaloNorth != Descriptor.HaloNorth
        || HaloEast != Descriptor.HaloEast || HaloSouth != Descriptor.HaloSouth
        || SampleCount != ExpectedSamples)
    {
        OutError = "TerrainCore provenance sidecar key, grid, or sample count does not match its tile";
        return false;
    }
    if (ExpectedSamples > BodyLimit - Cursor
        || ExpectedSamples > BodyLimit - Cursor - ExpectedSamples)
    {
        OutError = "TerrainCore provenance sidecar sample planes are truncated";
        return false;
    }
    const std::size_t Samples = static_cast<std::size_t>(ExpectedSamples);
    const std::uint8_t* Provenance = Bytes.data() + Cursor;
    const std::uint8_t* SourceIndices = Provenance + Samples;
    Cursor += Samples * 2U;
    if (Cursor != BodyLimit)
    {
        OutError = "TerrainCore provenance sidecar has trailing sample data";
        return false;
    }
    const std::uint64_t SourceCount = SourceTableSize(Manifest);
    for (std::size_t Index = 0; Index < Samples; ++Index)
    {
        if (!ValidProvenanceSample(Validity[Index], Provenance[Index], SourceIndices[Index], SourceCount))
        {
            OutError = "TerrainCore provenance sidecar contains malformed sample records";
            return false;
        }
    }
    OutSamples = {Provenance, SourceIndices, Samples};
    return true;
}
}

std::string SkiApplication::TerrainCoreProvenanceSidecarPath(
    const TerrainCoreTileKey& Key)
{
    if (Key.Lod != 0U) return {};
    return "provenance/lod0/" + std::to_string(Key.X) + "/"
        + std::to_string(Key.Y) + ".tcp";
}

bool SkiApplication::EncodeTerrainCoreTileProvenanceSidecar(
    const SkiDomain::TerrainCoreManifest& Manifest, const TerrainCoreTileKey& Key,
    const std::vector<std::uint8_t>& Validity,
    const std::vector<std::uint8_t>& Provenance,
    const std::vector<std::uint8_t>& SourceIndices,
    std::vector<std::uint8_t>& OutBytes, std::string& OutError)
{
    OutBytes.clear();
    OutError.clear();
    const SkiDomain::TerrainCoreValidation Validation = SkiDomain::ValidateTerrainCore(Manifest);
    if (!Validation.Ok() || !SkiDomain::IsTerrainCoreSha256(Manifest.ContentId))
    {
        OutError = "verified TerrainCore manifest is required to encode provenance";
        return false;
    }
    SkiDomain::TerrainCoreTileDescriptor Descriptor;
    if (!ReadDescriptorTile(Manifest, Key, Descriptor, OutError)) return false;
    const std::uint64_t ExpectedSamples = TileStoredSampleCount(Descriptor);
    if (ExpectedSamples == 0U || ExpectedSamples > SkiDomain::TerrainCoreMaxStoredSamples
        || Validity.size() != ExpectedSamples || Provenance.size() != ExpectedSamples
        || SourceIndices.size() != ExpectedSamples)
    {
        OutError = "TerrainCore provenance planes do not match the tile sample count";
        return false;
    }
    const std::uint64_t SourceCount = SourceTableSize(Manifest);
    for (std::size_t Index = 0; Index < Validity.size(); ++Index)
    {
        if (!ValidProvenanceSample(Validity[Index], Provenance[Index], SourceIndices[Index], SourceCount))
        {
            OutError = "TerrainCore provenance planes contain malformed sample records";
            return false;
        }
    }

    const std::string GridIdentity = GridSha256(Manifest);
    if (!SkiDomain::IsTerrainCoreSha256(GridIdentity))
    {
        OutError = "TerrainCore grid identity could not be computed";
        return false;
    }
    std::vector<std::uint8_t> Bytes;
    Bytes.reserve(static_cast<std::size_t>(265ULL + ExpectedSamples * 2ULL));
    constexpr std::array<std::uint8_t, 4> Magic{'T', 'C', 'P', '1'};
    Bytes.insert(Bytes.end(), Magic.begin(), Magic.end());
    AppendLittleEndian(Bytes, TerrainCoreProvenanceSidecarSchema);
    Bytes.insert(Bytes.end(), Manifest.ContentId.begin(), Manifest.ContentId.end());
    Bytes.insert(Bytes.end(), GridIdentity.begin(), GridIdentity.end());
    const std::string SourceDictionaryIdentity = SourceDictionarySha256(Manifest);
    Bytes.insert(Bytes.end(), SourceDictionaryIdentity.begin(), SourceDictionaryIdentity.end());
    Bytes.push_back(Key.Lod);
    AppendLittleEndian(Bytes, Key.X);
    AppendLittleEndian(Bytes, Key.Y);
    AppendLittleEndian(Bytes, Descriptor.StartColumn);
    AppendLittleEndian(Bytes, Descriptor.StartRow);
    AppendLittleEndian(Bytes, Descriptor.CoreWidth);
    AppendLittleEndian(Bytes, Descriptor.CoreHeight);
    Bytes.push_back(Descriptor.HaloWest);
    Bytes.push_back(Descriptor.HaloNorth);
    Bytes.push_back(Descriptor.HaloEast);
    Bytes.push_back(Descriptor.HaloSouth);
    AppendLittleEndian(Bytes, ExpectedSamples);
    Bytes.insert(Bytes.end(), Provenance.begin(), Provenance.end());
    Bytes.insert(Bytes.end(), SourceIndices.begin(), SourceIndices.end());
    const std::array<std::uint8_t, 32> Integrity = Sha256(Bytes.data(), Bytes.size());
    Bytes.insert(Bytes.end(), Integrity.begin(), Integrity.end());
    if (Bytes.size() > TerrainCoreProvenanceSidecarMaxBytes)
    {
        OutError = "TerrainCore provenance sidecar exceeds its size limit";
        return false;
    }
    OutBytes = std::move(Bytes);
    return true;
}

std::shared_ptr<SkiApplication::TerrainCoreRepository>
SkiApplication::TerrainCoreRepository::Create(SkiDomain::TerrainCoreManifest InManifest,
    TileReader InReader, std::string& OutError)
{
    OutError.clear();
    if (!InReader)
    {
        OutError = "tile reader is required";
        return {};
    }
    const SkiDomain::TerrainCoreValidation Validation = SkiDomain::ValidateTerrainCore(InManifest);
    if (!Validation.Ok())
    {
        OutError = "TerrainCore manifest validation failed";
        return {};
    }
    auto Immutable = std::make_shared<const SkiDomain::TerrainCoreManifest>(std::move(InManifest));
    return std::shared_ptr<TerrainCoreRepository>(new TerrainCoreRepository(
        std::move(Immutable), std::move(InReader), ProvenanceSidecarReader{}));
}

std::shared_ptr<SkiApplication::TerrainCoreRepository>
SkiApplication::TerrainCoreRepository::Create(SkiDomain::TerrainCoreManifest InManifest,
    TileReader InReader, ProvenanceSidecarReader InProvenanceReader,
    std::string& OutError)
{
    if (!InProvenanceReader)
    {
        OutError.clear();
        OutError = "provenance sidecar reader is required for verified provenance";
        return {};
    }
    std::shared_ptr<TerrainCoreRepository> Repository = Create(
        std::move(InManifest), std::move(InReader), OutError);
    if (Repository) Repository->ProvenanceReader = std::move(InProvenanceReader);
    return Repository;
}

SkiApplication::TerrainCoreRepository::TerrainCoreRepository(
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> InManifest, TileReader InReader,
    ProvenanceSidecarReader InProvenanceReader)
    : Manifest(std::move(InManifest)), Reader(std::move(InReader)),
      ProvenanceReader(std::move(InProvenanceReader))
{
}

std::shared_ptr<const SkiDomain::TerrainCoreManifest>
SkiApplication::TerrainCoreRepository::Metadata() const
{
    return Manifest;
}

bool SkiApplication::TerrainCoreRepository::ReadTile(const TerrainCoreTileKey& Key,
    TerrainCoreTilePayload& OutTile, std::string& OutError) const
{
    return ReadTile(Key, OutTile, OutError, {});
}

bool SkiApplication::TerrainCoreRepository::ReadTile(const TerrainCoreTileKey& Key,
    TerrainCoreTilePayload& OutTile, std::string& OutError,
    const TerrainCoreReadCancellation& Cancellation) const
{
    OutTile = {};
    OutError.clear();
    if (Cancellation.IsCancellationRequested())
    {
        OutError = "TerrainCore tile read cancelled";
        return false;
    }
    if (!Manifest || Key.Lod >= SkiDomain::TerrainCoreLodFactors.size())
    {
        OutError = "invalid TerrainCore tile key";
        return false;
    }
    const SkiDomain::TerrainCoreTileDescriptor* Descriptor = FindTile(*Manifest, Key);
    if (!Descriptor)
    {
        OutError = "TerrainCore tile not found";
        return false;
    }
    TerrainCoreTilePayload Candidate;
    if (!Reader(*Descriptor, Candidate, OutError))
    {
        Candidate = {};
        if (OutError.empty()) OutError = "TerrainCore tile read failed";
        return false;
    }
    if (Cancellation.IsCancellationRequested())
    {
        OutError = "TerrainCore tile read cancelled";
        return false;
    }
    const std::uint64_t StoredWidth = static_cast<std::uint64_t>(Descriptor->CoreWidth)
        + Descriptor->HaloWest + Descriptor->HaloEast;
    const std::uint64_t StoredHeight = static_cast<std::uint64_t>(Descriptor->CoreHeight)
        + Descriptor->HaloNorth + Descriptor->HaloSouth;
    const std::uint64_t Expected = StoredWidth * StoredHeight;
    if (Expected == 0 || Expected > SkiDomain::TerrainCoreMaxSamples
        || Candidate.Heights.size() != Expected || Candidate.Validity.size() != Expected)
    {
        OutError = "TerrainCore tile payload dimensions do not match metadata";
        return false;
    }
    for (std::size_t Index = 0; Index < Candidate.Validity.size(); ++Index)
    {
        if (Candidate.Validity[Index] > 1
            || (Candidate.Validity[Index] != 0 && !std::isfinite(Candidate.Heights[Index])))
        {
            OutError = "TerrainCore tile payload contains invalid samples";
            return false;
        }
    }
    Candidate.Key = Key;
    Candidate.Descriptor = *Descriptor;
    OutTile = std::move(Candidate);
    return true;
}

bool SkiApplication::TerrainCoreRepository::ReadVerifiedProvenanceSummary(
    TerrainCoreProvenanceSummary& OutSummary, std::string& OutError,
    const TerrainCoreReadCancellation& Cancellation) const
{
    OutSummary = {};
    OutError.clear();
    if (!Manifest || !ProvenanceReader)
    {
        OutError = "TerrainCore provenance sidecars are unavailable";
        return false;
    }
    if (!SkiDomain::ValidateTerrainCore(*Manifest).Ok())
    {
        OutError = "TerrainCore manifest is no longer valid";
        return false;
    }
    if (Cancellation.IsCancellationRequested())
    {
        OutError = "TerrainCore provenance summary cancelled";
        return false;
    }

    TerrainCoreProvenanceSummary Candidate;
    Candidate.TerrainCoreId = Manifest->ContentId;
    Candidate.GridSha256 = GridSha256(*Manifest);
    Candidate.SourceDictionarySha256 = SourceDictionarySha256(*Manifest);
    Candidate.Width = Manifest->Width;
    Candidate.Height = Manifest->Height;
    Candidate.TotalSamples = static_cast<std::uint64_t>(Manifest->Width) * Manifest->Height;
    Candidate.Sources.reserve(static_cast<std::size_t>(SourceTableSize(*Manifest)));
    Candidate.Sources.push_back({Manifest->Source.SourceId, 0});
    for (const SkiDomain::TerrainCoreSource& Source : Manifest->AdditionalSources)
    {
        Candidate.Sources.push_back({Source.SourceId, 0});
    }
    if (!SkiDomain::IsTerrainCoreSha256(Candidate.GridSha256)
        || !SkiDomain::IsTerrainCoreSha256(Candidate.SourceDictionarySha256)
        || Candidate.TotalSamples == 0U || Candidate.TotalSamples > SkiDomain::TerrainCoreMaxSamples)
    {
        OutError = "TerrainCore grid identity or sample count is invalid";
        return false;
    }

    for (const SkiDomain::TerrainCoreTileDescriptor& Descriptor : Manifest->Tiles)
    {
        if (Descriptor.LodIndex != 0U) continue;
        if (Cancellation.IsCancellationRequested())
        {
            OutError = "TerrainCore provenance summary cancelled";
            return false;
        }
        const TerrainCoreTileKey Key{Descriptor.LodIndex, Descriptor.TileX, Descriptor.TileY};
        TerrainCoreTilePayload Tile;
        if (!ReadTile(Key, Tile, OutError, Cancellation)) return false;
        std::vector<std::uint8_t> Bytes;
        const std::uint64_t Samples = TileStoredSampleCount(Descriptor);
        const std::uint64_t MaximumBytes = 265ULL + Samples * 2ULL;
        if (MaximumBytes > TerrainCoreProvenanceSidecarMaxBytes)
        {
            OutError = "TerrainCore provenance tile exceeds the configured sidecar bound";
            return false;
        }
        if (!ProvenanceReader(Descriptor, MaximumBytes, Bytes, OutError, Cancellation))
        {
            if (OutError.empty()) OutError = "TerrainCore provenance sidecar read failed";
            return false;
        }
        if (Cancellation.IsCancellationRequested())
        {
            OutError = "TerrainCore provenance summary cancelled";
            return false;
        }
        SidecarSamples Sidecar;
        if (!ValidateSidecar(*Manifest, Key, Descriptor, Tile.Validity,
                Bytes, Candidate.GridSha256, Sidecar, OutError))
        {
            return false;
        }

        const std::uint32_t StoredWidth = Descriptor.CoreWidth
            + Descriptor.HaloWest + Descriptor.HaloEast;
        const std::uint32_t CoreStartColumn = Descriptor.HaloWest;
        const std::uint32_t CoreStartRow = Descriptor.HaloNorth;
        for (std::uint32_t Row = 0; Row < Descriptor.CoreHeight; ++Row)
        {
            if (Cancellation.IsCancellationRequested())
            {
                OutError = "TerrainCore provenance summary cancelled";
                return false;
            }
            for (std::uint32_t Column = 0; Column < Descriptor.CoreWidth; ++Column)
            {
                const std::size_t Index = static_cast<std::size_t>(CoreStartRow + Row)
                    * StoredWidth + CoreStartColumn + Column;
                const std::uint8_t Class = Sidecar.Provenance[Index];
                if (Class == static_cast<std::uint8_t>(TerrainSampleProvenance::NoData))
                {
                    ++Candidate.NoDataSamples;
                    continue;
                }
                ++Candidate.ValidSamples;
                ++Candidate.SamplesByProvenance[Class];
                TerrainCoreProvenanceSourceCount& Source = Candidate.Sources[Sidecar.SourceIndices[Index]];
                ++Source.SampleCount;
                ++Source.SamplesByProvenance[Class];
            }
        }
    }

    if (Candidate.ValidSamples + Candidate.NoDataSamples != Candidate.TotalSamples)
    {
        OutError = "TerrainCore provenance tile cores do not cover the canonical sample grid";
        return false;
    }
    std::uint64_t ProvenanceTotal = 0;
    std::uint64_t SourceTotal = 0;
    for (const std::uint64_t Count : Candidate.SamplesByProvenance) ProvenanceTotal += Count;
    for (const TerrainCoreProvenanceSourceCount& Source : Candidate.Sources) SourceTotal += Source.SampleCount;
    if (ProvenanceTotal != Candidate.ValidSamples || SourceTotal != Candidate.ValidSamples)
    {
        OutError = "TerrainCore provenance counts are internally inconsistent";
        return false;
    }
    OutSummary = std::move(Candidate);
    return true;
}
