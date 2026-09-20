#pragma once

#include "Blueprint/UserWidget.h"
#include "SkiBootstrapWidget.generated.h"

UCLASS()
class SKIPRESENTATION_API USkiBootstrapWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    bool IsBootstrapReady() const;

protected:
    virtual void NativeOnInitialized() override;
};
