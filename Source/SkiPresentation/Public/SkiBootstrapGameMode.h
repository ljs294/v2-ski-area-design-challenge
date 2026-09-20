#pragma once

#include "GameFramework/GameModeBase.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiBootstrapGameMode.generated.h"

class ASkiTerrainActor;
class USkiP1Widget;

UCLASS()
class SKIPRESENTATION_API ASkiBootstrapGameMode : public AGameModeBase
{
    GENERATED_BODY()

protected:
    virtual void BeginPlay() override;

private:
    bool RunP1Smoke();
    void BeginP1Preparation(const SkiPreparation::Request& Request);
    void FinishP1Preparation(SkiPreparation::Result Result);

    UPROPERTY()
    TObjectPtr<USkiP1Widget> P1Widget;

    UPROPERTY()
    TObjectPtr<ASkiTerrainActor> TerrainActor;

    TSharedPtr<SkiApplication::TerrainSession> TerrainSession;
    TSharedPtr<SkiPreparation::Cancellation> PreparationCancellation;
};
