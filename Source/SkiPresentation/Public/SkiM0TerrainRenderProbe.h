#pragma once

#include "CoreMinimal.h"

class UWorld;

/** Starts an offscreen, tokened packaged TerrainCore screenshot probe and exits on completion. */
bool StartM0TerrainRenderProbe(UWorld* World, const FString& PackageRoot,
    const FString& ContentId, const FString& ReceiptPath, const FString& Token);
