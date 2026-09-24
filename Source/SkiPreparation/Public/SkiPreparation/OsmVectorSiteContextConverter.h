#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/OsmVectorPackage.h"

namespace SkiPreparation
{
class Cancellation;

/**
 * Canonical UTF-8 SiteContext schema-2 vector asset plus the verified source
 * binding needed by the caller when it writes the containing SiteContext.
 * Metadata is deliberately not duplicated into the flat vector JSON.
 */
struct SKIPREPARATION_API OsmVectorSiteContextAsset
{
    TArray<uint8> VectorJsonUtf8;
    std::string SourcePackageContentId;
    std::string TerrainCoreId;
    SkiDomain::MetricBounds ExtentM;
    OsmVectorSourceProvenance Source;
};

/**
 * Re-verifies the package against TerrainCore and emits one SiteContext
 * schema-2 feature per OSM part, preserving identity, order and coordinates.
 * The output is reset on failure; callers must bind its provenance and extent
 * when writing the SiteContext manifest and asset.
 */
SKIPREPARATION_API bool ConvertVerifiedOsmVectorPackageToSiteContextAsset(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation& CancellationValue, OsmVectorSiteContextAsset& OutAsset,
    FString& OutError);
}
