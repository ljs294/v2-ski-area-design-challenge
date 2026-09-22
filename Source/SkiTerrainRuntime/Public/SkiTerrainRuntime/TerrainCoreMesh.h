#pragma once

#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiDomain/TerrainTile.h"

namespace SkiTerrainRuntime
{
/**
 * Builds the render mesh for one decoded TerrainCore tile. Core samples are emitted once;
 * halo samples are used only for normals. The canonical NW-SE diagonal is shared with
 * TerrainCoreTileCache::QueryCanonicalCell.
 */
SKITERRAINRUNTIME_API bool BuildTerrainCoreTileMesh(
    const SkiApplication::TerrainCoreTilePayload& Payload,
    const SkiDomain::TerrainCoreManifest& Manifest,
    SkiDomain::Revision Revision,
    bool AddSkirts,
    double SkirtDepthM,
    SkiDomain::TerrainTileMesh& OutMesh) noexcept;
}
