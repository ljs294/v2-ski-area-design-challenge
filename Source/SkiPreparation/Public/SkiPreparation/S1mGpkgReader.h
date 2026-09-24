#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/S1mLineage.h"

namespace SkiPreparation
{
/**
 * Reads the one observed S1M GeoPackage schema through SQLiteCore in read-only mode.
 *
 * The result contains only GeoPackage evidence. It does not create or imply XML-sidecar,
 * COG-header, or vertical-RMSE proof. The caller supplies the already identified artifact;
 * its exact byte count must match the local file before any rows are accepted.
 */
SKIPREPARATION_API bool ReadS1mGeoPackage(const FString& Path,
    const FS1mLineageArtifactEvidence& Artifact, const FS1mLineageLimits& Limits,
    FS1mGeoPackageEvidence& OutEvidence, FString& OutFailureCode, FString& OutFailureDetail);
}
