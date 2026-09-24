#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiDomain/SiteSelection.h"
#include "SkiSiteMap.generated.h"

class SSkiSiteMap;

/** UMG host for the native Slate site map used by the resort picker. */
UCLASS()
class SKIPRESENTATION_API USkiSiteMapWidget final : public UWidget
{
    GENERATED_BODY()

public:
    void SetPickerActive(bool bActive);
    void SetBoundaryEditingEnabled(bool bEnabled);
    void SetCenter(double LatitudeDeg, double LongitudeDeg);
    bool FitToBounds(const SkiDomain::GeographicBounds& Bounds, double PaddingPx = 48.0);
    void SetContoursEnabled(bool bEnabled);
    void SetContourUnits(bool bMetric);
    void SetNetworkEnabled(bool bEnabled);
    void ClearSelection();
    /** Development viewport smoke only; requires the harness launch token. */
    bool CreateViewportSmokeBoundary(const FGuid& SmokeToken);
    bool HasValidSelection() const;
    bool TryGetSelectedBounds(SkiDomain::GeographicBounds& OutBounds) const;
    SkiDomain::SiteRectangleM GetSelectionMeters() const;
    void SetPreviewStatusHandler(TFunction<void(const FString&)> Handler);
    void SetSelectionStatusHandler(TFunction<void(const FString&)> Handler);

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
    TSharedPtr<SSkiSiteMap> SiteMap;
    TFunction<void(const FString&)> PreviewStatusHandler;
    TFunction<void(const FString&)> SelectionStatusHandler;
    double CenterLatitudeDeg = 47.25;
    double CenterLongitudeDeg = -121.55;
    SkiDomain::GeographicBounds PendingFitBounds;
    double PendingFitPaddingPx = 48.0;
    bool bPickerActive = false;
    bool bBoundaryEditingEnabled = false;
    bool bNetworkEnabled = true;
    bool bContoursEnabled = true;
    bool bMetricContourLabels = false;
    bool bHasPendingFit = false;
};
