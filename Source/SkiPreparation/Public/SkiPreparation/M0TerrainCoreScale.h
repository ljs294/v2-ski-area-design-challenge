#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
/** Opt-in synthetic store spike. Returns false and fills OutError on failure. */
SKIPREPARATION_API bool RunM0TerrainCoreScale(uint32 Side, FString& OutReport,
    FString& OutError);
/** Retains the package for a packaged renderer probe; caller owns deleting OutPackageRoot. */
SKIPREPARATION_API bool RunM0TerrainCoreScale(uint32 Side, FString& OutReport,
    FString& OutError, FString& OutPackageRoot, FString& OutContentId);
}
