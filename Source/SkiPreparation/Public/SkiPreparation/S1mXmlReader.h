#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/S1mLineage.h"

namespace SkiPreparation
{
/**
 * Performs only a bounded XML safety preflight. It deliberately does not claim that
 * a sidecar is well formed or emit CRS, datum, footprint, date, or RMSE evidence.
 * The repository has no authoritative per-tile S1M XML schema and real fixture to
 * bind those facts to. Well-bounded input therefore fails closed as unproven until
 * that source contract is pinned.
 *
 * On failure, OutEvidence is reset except that its Artifact is retained when the
 * caller's exact byte count matches the supplied buffer and its identity is present.
 */
SKIPREPARATION_API bool PreflightS1mXmlSidecar(const TArray<uint8>& XmlBytes,
    const FS1mLineageArtifactEvidence& Artifact, const FS1mLineageLimits& Limits,
    FS1mXmlSidecarEvidence& OutEvidence, FString& OutFailureCode, FString& OutFailureDetail);
}
