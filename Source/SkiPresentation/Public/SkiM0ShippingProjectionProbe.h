#pragma once

#include "CoreMinimal.h"

class UWorld;
namespace SkiPreparation { struct FM0RasterProjectionReceipt; }

/** Game-thread packaged PROJ check through the engine's UFS-aware GeoReferencing module. */
SKIPRESENTATION_API bool ProbeM0ShippingProjection(
    UWorld& World, SkiPreparation::FM0RasterProjectionReceipt& Receipt);
