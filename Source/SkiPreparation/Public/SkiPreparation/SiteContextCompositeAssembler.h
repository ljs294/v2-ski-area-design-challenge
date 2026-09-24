#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainQuality.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/ImageryPyramid.h"
#include "SkiPreparation/OsmVectorSiteContextConverter.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainCorePackageStore.h"

namespace SkiPreparation
{
/**
 * Read access to an already-produced imagery pyramid. Inspect must report facts
 * from the bytes actually stored at RelativePath; Read is used to copy those
 * exact bytes into the immutable SiteContext component.
 */
class SKIPREPARATION_API ISiteContextCompositeImagerySource : public IImageryPyramidAssetReader
{
public:
    virtual bool Read(const std::string& RelativePath,
        const Cancellation& CancellationValue, TArray<uint8>& OutBytes,
        FString& OutError) = 0;
};

struct SKIPREPARATION_API SiteContextCompositeAssemblyResult
{
    SiteContextManifest SiteContext;
    CompositeInstallReceipt Receipt;
    FString SiteContextPackageDirectory;
    FString InstalledReceiptDirectory;
};

/**
 * Reopens and verifies the prepared TerrainCore and CoverEcology, verifies the
 * imagery pyramid and converter-produced OSM vectors, stores SiteContext schema
 * 2 with structured OSM source lineage, then publishes a schema-3 composite
 * receipt as the final activation step.
 * The imagery reader's paths follow ImageryPyramid's lod/Y/X convention; the
 * stored SiteContext paths are explicitly translated to lod/X/Y. The
 * converter-produced vector lineage is copied to the content-addressed
 * SiteContext manifest, not reduced to the public-facing attribution fields.
 */
SKIPREPARATION_API bool AssembleAndActivateSiteContextComposite(
    const FString& DataRoot, const FString& TerrainCoreId, const FString& CoverEcologyId,
    const FString& GeneratorVersion, const ImageryPyramidManifest& Imagery,
    ISiteContextCompositeImagerySource& ImagerySource,
    const OsmVectorSiteContextAsset& Vectors,
    const SkiDomain::TerrainProvenanceCounts& ProvenanceCounts,
    const SkiDomain::TerrainQualityReport& Quality,
    const Cancellation& CancellationValue,
    SiteContextCompositeAssemblyResult& OutResult, FString& OutError);
}
