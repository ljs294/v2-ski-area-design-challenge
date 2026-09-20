#pragma once

#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
class SKIPREPARATION_API FixtureTerrainProvider final : public Provider
{
public:
    explicit FixtureTerrainProvider(FString InDataRoot);
    Result Prepare(const Request& RequestValue, const TSharedRef<Cancellation>& CancellationValue,
        const ProgressCallback& OnProgress) override;

private:
    FString DataRoot;
};
}
