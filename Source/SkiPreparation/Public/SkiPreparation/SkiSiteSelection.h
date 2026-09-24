#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
/** The creation rule applies at write time; legacy installed names are read unchanged. */
SKIPREPARATION_API bool ValidateResortName(const FString& Candidate,
    FString& OutTrimmedName, FString& OutError);

struct SKIPREPARATION_API SiteRectangleM
{
    double WestM = 0.0;
    double SouthM = 0.0;
    double EastM = 0.0;
    double NorthM = 0.0;
};

enum class SiteCoverage : uint8
{
    Unknown,
    SupportedUsUsgs3Dep,
    Unsupported,
};

struct SKIPREPARATION_API SiteSelectionDecision
{
    bool bValidGeometry = false;
    bool bMayDownload = false;
    double WidthM = 0.0;
    double HeightM = 0.0;
    double AreaSqKm = 0.0;
    FString Reason;
};

/** North-up, free-aspect metric rectangle. Coverage is a catalog-supplied fact. */
SKIPREPARATION_API SiteSelectionDecision ValidateSiteSelection(
    const SiteRectangleM& Rectangle, SiteCoverage Coverage,
    double MaximumSideM = 4000.0);
}
