#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
struct FM0TerrainCoreTiming
{
    double ShardWriteSeconds = 0.0;
    double InternalVerifySeconds = 0.0;
    uint64 StagingBytes = 0;
    uint64 ObservedPeakPhysicalBytes = 0;
};

extern thread_local FM0TerrainCoreTiming* GM0TerrainCoreTiming;
}
