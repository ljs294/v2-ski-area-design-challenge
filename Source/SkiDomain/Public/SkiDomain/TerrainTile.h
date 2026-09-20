#pragma once

#include "SkiDomain/Heightfield.h"

#include <cstdint>
#include <vector>

namespace SkiDomain
{
constexpr std::uint32_t TerrainTileCells = 255;

struct TileKey
{
    std::uint32_t X = 0;
    std::uint32_t Y = 0;
    std::uint8_t Lod = 0;
};

struct TerrainVertex
{
    float EastM = 0.0F;
    float NorthM = 0.0F;
    float UpM = 0.0F;
    float NormalEast = 0.0F;
    float NormalNorth = 0.0F;
    float NormalUp = 1.0F;
    float U = 0.0F;
    float V = 0.0F;
};

struct TerrainTileMesh
{
    TileKey Key;
    Revision SourceRevision = 0;
    std::vector<TerrainVertex> Vertices;
    std::vector<std::uint32_t> Indices;
};

SKI_DOMAIN_API bool BuildTerrainTile(const Heightfield& Field, const TileKey& Key,
    bool AddSkirts, double SkirtDepthM, TerrainTileMesh& OutMesh) noexcept;
SKI_DOMAIN_API bool ValidateAdjacentLods(const std::vector<TileKey>& Keys) noexcept;
}
