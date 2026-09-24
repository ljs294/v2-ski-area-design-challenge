#pragma once

#include "CoreMinimal.h"

class UWorld;

// Invoke from the packaged bootstrap dispatch for -SkiM0MapSpike.
// ReceiptPath must be an absolute writable path. The runner owns its Slate
// lifetime, travels to the current level, and writes a receipt after teardown.
SKIPRESENTATION_API bool StartSkiSiteMapSpike(UWorld* World, const FString& ReceiptPath,
    const FString& Token);
