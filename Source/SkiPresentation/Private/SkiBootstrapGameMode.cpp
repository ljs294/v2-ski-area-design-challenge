#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiApplication/Bootstrap.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

void ASkiBootstrapGameMode::BeginPlay()
{
    Super::BeginPlay();
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    UClass* WidgetClass = LoadClass<USkiBootstrapWidget>(nullptr, TEXT("/Game/P0Generated/WBP_Bootstrap.WBP_Bootstrap_C"));
    USkiBootstrapWidget* Widget = Controller && WidgetClass ? CreateWidget<USkiBootstrapWidget>(Controller, WidgetClass) : nullptr;
    const bool ExpectedMap = GetWorld()->GetOutermost()->GetName() == TEXT("/Game/P0Generated/Bootstrap");
    const bool Ready = ExpectedMap && SkiApplication::CheckDomainBoundary() && Widget && Widget->IsBootstrapReady();
    if (Widget)
    {
        Widget->AddToViewport();
        Controller->bShowMouseCursor = true;
        Controller->SetInputMode(FInputModeUIOnly());
    }

    // Explicit local startup probe, available in Shipping without enabling logging,
    // an automation listener, or editor modules. This does not qualify GPU visuals.
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP0Smoke")))
    {
        FString ReceiptPath;
        FString Token;
        FGuid ParsedToken;
        const bool ArgumentsValid = FParse::Value(FCommandLine::Get(), TEXT("SkiP0Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP0Token="), Token) && FGuid::Parse(Token, ParsedToken);
        const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"ready\":%s}"), *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), Ready ? TEXT("true") : TEXT("false"));
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath);
        FPlatformMisc::RequestExitWithStatus(false, Ready && Written ? 0 : 1);
    }
}
