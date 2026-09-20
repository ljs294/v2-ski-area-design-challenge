#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
struct SKIPREPARATION_API PackageAssetBytes
{
    FString Path;
    FString Type;
    TArray<uint8> Bytes;
    bool Required = true;
    FString MissingReason;
    FString Source;
    FString License;
};

class SKIPREPARATION_API PackageStore
{
public:
    explicit PackageStore(FString InDataRoot);

    bool WriteAndActivate(SkiDomain::TerrainManifest Manifest,
        const SkiDomain::Heightfield& Heightfield, FString& OutPackageDirectory,
        SkiDomain::TerrainManifest& OutManifest, FString& OutError,
        const TArray<PackageAssetBytes>& AdditionalAssets = {}) const;
    bool Load(const FString& ContentId, SkiDomain::TerrainManifest& OutManifest,
        SkiDomain::Heightfield& OutHeightfield, FString& OutError,
        TArray<uint8>* OutCover = nullptr) const;

    const FString& DataRoot() const noexcept { return Root; }

private:
    FString Root;
};

SKIPREPARATION_API FString SerializeManifest(const SkiDomain::TerrainManifest& Manifest,
    bool IncludeContentId);
SKIPREPARATION_API bool ParseManifest(const FString& Json, SkiDomain::TerrainManifest& OutManifest,
    FString& OutError);
SKIPREPARATION_API FString Sha256(const TArrayView<const uint8> Bytes);
}
