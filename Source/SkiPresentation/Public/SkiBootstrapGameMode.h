#pragma once

#include "GameFramework/GameModeBase.h"
#include "SkiBootstrapGameMode.generated.h"

UCLASS()
class SKIPRESENTATION_API ASkiBootstrapGameMode : public AGameModeBase
{
    GENERATED_BODY()

protected:
    virtual void BeginPlay() override;
};
