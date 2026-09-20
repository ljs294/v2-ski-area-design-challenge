#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
SKIPREPARATION_API bool ValidateSelectorMessage(const FString& Json, const FString& ExpectedToken,
    uint64 ExpectedGeneration, Request& OutRequest, FString& OutError);
}
