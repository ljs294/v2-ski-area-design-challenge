#include "SkiPreparation/TerrainPackageStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <algorithm>
#include <openssl/sha.h>

namespace
{
bool ReadUtf8Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, std::string& Out)
{
    FString Value;
    if (!Object || !Object->TryGetStringField(Name, Value)) return false;
    Out = TCHAR_TO_UTF8(*Value);
    return true;
}

bool ValidUint32Number(const double Value)
{
    return FMath::IsFinite(Value) && Value >= 0.0 && Value <= static_cast<double>(MAX_uint32)
        && FMath::FloorToDouble(Value) == Value;
}

TSharedRef<FJsonObject> BoundsObject(const SkiDomain::GeographicBounds& Bounds)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("west"), Bounds.WestDeg);
    Object->SetNumberField(TEXT("south"), Bounds.SouthDeg);
    Object->SetNumberField(TEXT("east"), Bounds.EastDeg);
    Object->SetNumberField(TEXT("north"), Bounds.NorthDeg);
    return Object;
}

bool ReadBounds(const TSharedPtr<FJsonObject>& Object, SkiDomain::GeographicBounds& Out)
{
    return Object && Object->TryGetNumberField(TEXT("west"), Out.WestDeg)
        && Object->TryGetNumberField(TEXT("south"), Out.SouthDeg)
        && Object->TryGetNumberField(TEXT("east"), Out.EastDeg)
        && Object->TryGetNumberField(TEXT("north"), Out.NorthDeg);
}

bool ParseUnsigned(const FString& Json, SkiDomain::TerrainManifest& Out, FString& Error)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root)
    {
        Error = TEXT("Manifest is not valid JSON.");
        return false;
    }
    double Schema = 0.0;
    if (!Root->TryGetNumberField(TEXT("schemaVersion"), Schema) || !ValidUint32Number(Schema))
    {
        Error = TEXT("Manifest schemaVersion is missing or invalid.");
        return false;
    }
    Out.SchemaVersion = static_cast<uint32>(Schema);
    ReadUtf8Field(Root, TEXT("contentId"), Out.ContentId);
    ReadUtf8Field(Root, TEXT("name"), Out.Name);
    ReadUtf8Field(Root, TEXT("source"), Out.Source);
    ReadUtf8Field(Root, TEXT("requestedAtUtc"), Out.RequestedAtUtc);
    ReadUtf8Field(Root, TEXT("generatorVersion"), Out.GeneratorVersion);
    ReadUtf8Field(Root, TEXT("horizontalFrame"), Out.HorizontalFrame);
    ReadUtf8Field(Root, TEXT("verticalDatum"), Out.VerticalDatum);
    ReadUtf8Field(Root, TEXT("rowOrientation"), Out.RowOrientation);
    const TSharedPtr<FJsonObject>* RequestedBounds = nullptr;
    const TSharedPtr<FJsonObject>* ActualBounds = nullptr;
    if (!Root->TryGetObjectField(TEXT("requestedBounds"), RequestedBounds) || !RequestedBounds
        || !Root->TryGetObjectField(TEXT("actualBounds"), ActualBounds) || !ActualBounds
        || !ReadBounds(*RequestedBounds, Out.RequestedBounds)
        || !ReadBounds(*ActualBounds, Out.ActualBounds))
    {
        Error = TEXT("Manifest bounds are missing or invalid.");
        return false;
    }
    const TSharedPtr<FJsonObject>* Origin = nullptr;
    if (!Root->TryGetObjectField(TEXT("localOrigin"), Origin) || !Origin || !*Origin
        || !(*Origin)->TryGetNumberField(TEXT("latitude"), Out.LocalOrigin.LatitudeDeg)
        || !(*Origin)->TryGetNumberField(TEXT("longitude"), Out.LocalOrigin.LongitudeDeg)
        || !(*Origin)->TryGetNumberField(TEXT("heightM"), Out.LocalOrigin.HeightM))
    {
        Error = TEXT("Manifest local origin is missing or invalid.");
        return false;
    }
    double HeightWidth = 0.0, HeightHeight = 0.0, CoverWidth = 0.0, CoverHeight = 0.0;
    if (!Root->TryGetNumberField(TEXT("heightWidth"), HeightWidth)
        || !Root->TryGetNumberField(TEXT("heightHeight"), HeightHeight)
        || !Root->TryGetNumberField(TEXT("coverWidth"), CoverWidth)
        || !Root->TryGetNumberField(TEXT("coverHeight"), CoverHeight)
        || !Root->TryGetNumberField(TEXT("eastSpacingM"), Out.EastSpacingM)
        || !Root->TryGetNumberField(TEXT("northSpacingM"), Out.NorthSpacingM)
        || !Root->TryGetNumberField(TEXT("nodata"), Out.NoDataValue))
    {
        Error = TEXT("Manifest grid metadata is missing.");
        return false;
    }
    if (!ValidUint32Number(HeightWidth) || !ValidUint32Number(HeightHeight)
        || !ValidUint32Number(CoverWidth) || !ValidUint32Number(CoverHeight))
    {
        Error = TEXT("Manifest dimensions must be finite unsigned integers.");
        return false;
    }
    Out.HeightWidth = static_cast<uint32>(HeightWidth);
    Out.HeightHeight = static_cast<uint32>(HeightHeight);
    Out.CoverWidth = static_cast<uint32>(CoverWidth);
    Out.CoverHeight = static_cast<uint32>(CoverHeight);
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
    {
        Error = TEXT("Manifest assets are missing.");
        return false;
    }
    Out.Assets.clear();
    for (const TSharedPtr<FJsonValue>& Value : *Assets)
    {
        const TSharedPtr<FJsonObject> AssetObject = Value ? Value->AsObject() : nullptr;
        if (!AssetObject)
        {
            Error = TEXT("Manifest asset is not an object.");
            return false;
        }
        SkiDomain::TerrainAsset Asset;
        double Length = 0.0;
        if (!ReadUtf8Field(AssetObject, TEXT("path"), Asset.Path)
            || !ReadUtf8Field(AssetObject, TEXT("type"), Asset.Type)
            || !ReadUtf8Field(AssetObject, TEXT("sha256"), Asset.Sha256)
            || !AssetObject->TryGetNumberField(TEXT("length"), Length)
            || !AssetObject->TryGetBoolField(TEXT("required"), Asset.Required))
        {
            Error = TEXT("Manifest asset fields are invalid.");
            return false;
        }
        if (!FMath::IsFinite(Length) || Length < 0.0 || FMath::FloorToDouble(Length) != Length
            || Length > static_cast<double>(SkiDomain::MaxAssetBytes))
        {
            Error = TEXT("Manifest asset length is invalid.");
            return false;
        }
        ReadUtf8Field(AssetObject, TEXT("missingReason"), Asset.MissingReason);
        ReadUtf8Field(AssetObject, TEXT("source"), Asset.Source);
        ReadUtf8Field(AssetObject, TEXT("license"), Asset.License);
        Asset.Length = static_cast<uint64>(Length);
        Out.Assets.push_back(std::move(Asset));
    }
    return true;
}
}

SkiPreparation::PackageStore::PackageStore(FString InDataRoot)
    : Root(FPaths::ConvertRelativePathToFull(std::move(InDataRoot)))
{
}

FString SkiPreparation::Sha256(const TArrayView<const uint8> Bytes)
{
    uint8 Signature[SHA256_DIGEST_LENGTH];
    if (!SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Signature)) return {};
    FString Result;
    Result.Reserve(64);
    for (const uint8 Byte : Signature)
    {
        Result += FString::Printf(TEXT("%02x"), Byte);
    }
    return Result;
}

FString SkiPreparation::SerializeManifest(const SkiDomain::TerrainManifest& Manifest,
    const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
    if (IncludeContentId) Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    Root->SetStringField(TEXT("name"), UTF8_TO_TCHAR(Manifest.Name.c_str()));
    Root->SetStringField(TEXT("source"), UTF8_TO_TCHAR(Manifest.Source.c_str()));
    Root->SetStringField(TEXT("requestedAtUtc"), UTF8_TO_TCHAR(Manifest.RequestedAtUtc.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Manifest.GeneratorVersion.c_str()));
    Root->SetObjectField(TEXT("requestedBounds"), BoundsObject(Manifest.RequestedBounds));
    Root->SetObjectField(TEXT("actualBounds"), BoundsObject(Manifest.ActualBounds));
    TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
    Origin->SetNumberField(TEXT("latitude"), Manifest.LocalOrigin.LatitudeDeg);
    Origin->SetNumberField(TEXT("longitude"), Manifest.LocalOrigin.LongitudeDeg);
    Origin->SetNumberField(TEXT("heightM"), Manifest.LocalOrigin.HeightM);
    Root->SetObjectField(TEXT("localOrigin"), Origin);
    Root->SetStringField(TEXT("horizontalFrame"), UTF8_TO_TCHAR(Manifest.HorizontalFrame.c_str()));
    Root->SetStringField(TEXT("verticalDatum"), UTF8_TO_TCHAR(Manifest.VerticalDatum.c_str()));
    Root->SetNumberField(TEXT("heightWidth"), Manifest.HeightWidth);
    Root->SetNumberField(TEXT("heightHeight"), Manifest.HeightHeight);
    Root->SetNumberField(TEXT("coverWidth"), Manifest.CoverWidth);
    Root->SetNumberField(TEXT("coverHeight"), Manifest.CoverHeight);
    Root->SetNumberField(TEXT("eastSpacingM"), Manifest.EastSpacingM);
    Root->SetNumberField(TEXT("northSpacingM"), Manifest.NorthSpacingM);
    Root->SetNumberField(TEXT("nodata"), Manifest.NoDataValue);
    Root->SetStringField(TEXT("rowOrientation"), UTF8_TO_TCHAR(Manifest.RowOrientation.c_str()));
    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const SkiDomain::TerrainAsset& Asset : Manifest.Assets)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("path"), UTF8_TO_TCHAR(Asset.Path.c_str()));
        Object->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Asset.Type.c_str()));
        Object->SetStringField(TEXT("sha256"), UTF8_TO_TCHAR(Asset.Sha256.c_str()));
        Object->SetNumberField(TEXT("length"), static_cast<double>(Asset.Length));
        Object->SetBoolField(TEXT("required"), Asset.Required);
        Object->SetStringField(TEXT("missingReason"), UTF8_TO_TCHAR(Asset.MissingReason.c_str()));
        Object->SetStringField(TEXT("source"), UTF8_TO_TCHAR(Asset.Source.c_str()));
        Object->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Asset.License.c_str()));
        Assets.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("assets"), Assets);
    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
    FJsonSerializer::Serialize(Root, Writer);
    return Output;
}

bool SkiPreparation::ParseManifest(const FString& Json, SkiDomain::TerrainManifest& OutManifest,
    FString& OutError)
{
    if (FTCHARToUTF8(Json).Length() > static_cast<int64>(SkiDomain::MaxManifestBytes))
    {
        OutError = TEXT("Manifest exceeds 1 MiB.");
        return false;
    }
    SkiDomain::TerrainManifest Candidate;
    if (!ParseUnsigned(Json, Candidate, OutError)) return false;
    const SkiDomain::ManifestValidation Validation = SkiDomain::ValidateManifest(Candidate);
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("Manifest validation failed (%d, asset %llu)."),
            static_cast<int32>(Validation.Error), static_cast<uint64>(Validation.AssetIndex));
        return false;
    }
    OutManifest = std::move(Candidate);
    return true;
}

bool SkiPreparation::PackageStore::WriteAndActivate(SkiDomain::TerrainManifest Manifest,
    const SkiDomain::Heightfield& Heightfield, FString& OutPackageDirectory,
    SkiDomain::TerrainManifest& OutManifest, FString& OutError,
    const TArray<PackageAssetBytes>& AdditionalAssets) const
{
    if (!SkiDomain::IsValidHeightfield(Heightfield))
    {
        OutError = TEXT("Heightfield is invalid.");
        return false;
    }
    TArray<uint8> ElevationBytes;
    const uint64 ByteCount = static_cast<uint64>(Heightfield.Samples.size()) * sizeof(float);
    if (ByteCount > SkiDomain::MaxAssetBytes || ByteCount > MAX_int32)
    {
        OutError = TEXT("Elevation asset exceeds its limit.");
        return false;
    }
    ElevationBytes.Append(reinterpret_cast<const uint8*>(Heightfield.Samples.data()),
        static_cast<int32>(ByteCount));
    SkiDomain::TerrainAsset Elevation{"elevation.f32le", "heightfield-f32le",
        TCHAR_TO_UTF8(*Sha256(ElevationBytes)), ByteCount, true, {},
        "USGS 3DEP or deterministic fixture", "USGS public domain / fixture"};
    Manifest.Assets.erase(std::remove_if(Manifest.Assets.begin(), Manifest.Assets.end(),
        [](const SkiDomain::TerrainAsset& Asset) { return Asset.Path == "elevation.f32le"; }), Manifest.Assets.end());
    Manifest.Assets.insert(Manifest.Assets.begin(), std::move(Elevation));
    for (const PackageAssetBytes& Input : AdditionalAssets)
    {
        const FTCHARToUTF8 PathUtf8(*Input.Path);
        const FTCHARToUTF8 TypeUtf8(*Input.Type);
        const FTCHARToUTF8 ReasonUtf8(*Input.MissingReason);
        const FTCHARToUTF8 SourceUtf8(*Input.Source);
        const FTCHARToUTF8 LicenseUtf8(*Input.License);
        SkiDomain::TerrainAsset Asset{std::string(PathUtf8.Get(), PathUtf8.Length()),
            std::string(TypeUtf8.Get(), TypeUtf8.Length()), {}, static_cast<uint64>(Input.Bytes.Num()),
            Input.Required, std::string(ReasonUtf8.Get(), ReasonUtf8.Length()),
            std::string(SourceUtf8.Get(), SourceUtf8.Length()), std::string(LicenseUtf8.Get(), LicenseUtf8.Length())};
        if (!Input.Bytes.IsEmpty()) Asset.Sha256 = TCHAR_TO_UTF8(*Sha256(Input.Bytes));
        Manifest.Assets.push_back(std::move(Asset));
    }
    Manifest.HeightWidth = Heightfield.Width;
    Manifest.HeightHeight = Heightfield.Height;
    Manifest.EastSpacingM = Heightfield.EastSpacingM;
    Manifest.NorthSpacingM = Heightfield.NorthSpacingM;
    Manifest.NoDataValue = Heightfield.NoDataValue;
    const FString UnsignedJson = SerializeManifest(Manifest, false);
    const FTCHARToUTF8 UnsignedUtf8(*UnsignedJson);
    const TArrayView<const uint8> UnsignedBytes(reinterpret_cast<const uint8*>(UnsignedUtf8.Get()),
        UnsignedUtf8.Length());
    Manifest.ContentId = TCHAR_TO_UTF8(*Sha256(UnsignedBytes));
    if (!SkiDomain::ValidateManifest(Manifest).Ok())
    {
        OutError = TEXT("Generated manifest failed validation.");
        return false;
    }
    const FString StagingRoot = FPaths::Combine(Root, TEXT(".staging"));
    const FString Stage = FPaths::Combine(StagingRoot, FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Packages = FPaths::Combine(Root, TEXT("TerrainPackages"));
    const FString Target = FPaths::Combine(Packages, UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    IFileManager& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*Stage, true) || !Files.MakeDirectory(*Packages, true))
    {
        OutError = TEXT("Unable to create terrain staging directories.");
        return false;
    }
    const auto Cleanup = [&]() { Files.DeleteDirectory(*Stage, false, true); };
    if (!FFileHelper::SaveArrayToFile(ElevationBytes, *FPaths::Combine(Stage, TEXT("elevation.f32le"))))
    {
        OutError = TEXT("Unable to write staged elevation.");
        Cleanup();
        return false;
    }
    for (const PackageAssetBytes& Asset : AdditionalAssets)
    {
        if (Asset.Bytes.IsEmpty()) continue;
        const FString Destination = FPaths::Combine(Stage, Asset.Path);
        if (!Files.MakeDirectory(*FPaths::GetPath(Destination), true)
            || !FFileHelper::SaveArrayToFile(Asset.Bytes, *Destination))
        {
            OutError = TEXT("Unable to write staged package asset: ") + Asset.Path;
            Cleanup();
            return false;
        }
    }
    const FString ManifestJson = SerializeManifest(Manifest, true);
    const FTCHARToUTF8 ManifestUtf8(*ManifestJson);
    uint64 StagingBytes = static_cast<uint64>(ManifestUtf8.Length());
    for (const SkiDomain::TerrainAsset& Asset : Manifest.Assets)
    {
        if (StagingBytes > SkiDomain::MaxPackageBytes - Asset.Length)
        {
            OutError = TEXT("Package staging exceeds 1 GiB including its manifest.");
            Cleanup();
            return false;
        }
        StagingBytes += Asset.Length;
    }
    if (!FFileHelper::SaveStringToFile(ManifestJson, *FPaths::Combine(Stage, TEXT("manifest.json")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = TEXT("Unable to write staged manifest.");
        Cleanup();
        return false;
    }
    SkiDomain::TerrainManifest VerifiedManifest;
    SkiDomain::Heightfield VerifiedHeightfield;
    FString VerifyError;
    const FString StageContentId = UTF8_TO_TCHAR(Manifest.ContentId.c_str());
    PackageStore StageStore(StagingRoot);
    const FString SyntheticTarget = FPaths::Combine(StagingRoot, TEXT("TerrainPackages"), StageContentId);
    Files.MakeDirectory(*FPaths::GetPath(SyntheticTarget), true);
    if (!Files.Move(*SyntheticTarget, *Stage, false, false, true, true))
    {
        OutError = TEXT("Unable to move staged package for verification.");
        Cleanup();
        return false;
    }
    if (!StageStore.Load(StageContentId, VerifiedManifest, VerifiedHeightfield, VerifyError))
    {
        OutError = TEXT("Staged package verification failed: ") + VerifyError;
        Files.DeleteDirectory(*SyntheticTarget, false, true);
        return false;
    }
    if (Files.DirectoryExists(*Target))
    {
        Files.DeleteDirectory(*SyntheticTarget, false, true);
        SkiDomain::TerrainManifest ExistingManifest;
        SkiDomain::Heightfield ExistingHeightfield;
        if (!Load(StageContentId, ExistingManifest, ExistingHeightfield, OutError)) return false;
    }
    else if (!Files.Move(*Target, *SyntheticTarget, false, false, true, true))
    {
        OutError = TEXT("Unable to atomically activate terrain package.");
        Files.DeleteDirectory(*SyntheticTarget, false, true);
        return false;
    }
    OutPackageDirectory = Target;
    OutManifest = std::move(Manifest);
    return true;
}

bool SkiPreparation::PackageStore::Load(const FString& ContentId,
    SkiDomain::TerrainManifest& OutManifest, SkiDomain::Heightfield& OutHeightfield,
    FString& OutError, TArray<uint8>* OutCover) const
{
    if (OutCover) OutCover->Reset();
    if (ContentId.Len() != 64 || ContentId.Contains(TEXT("/")) || ContentId.Contains(TEXT("\\")))
    {
        OutError = TEXT("Content ID is invalid.");
        return false;
    }
    const FString Directory = FPaths::Combine(Root, TEXT("TerrainPackages"), ContentId);
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Directory, TEXT("manifest.json")))
        || !ParseManifest(Json, OutManifest, OutError)
        || UTF8_TO_TCHAR(OutManifest.ContentId.c_str()) != ContentId)
    {
        if (OutError.IsEmpty()) OutError = TEXT("Manifest content identity does not match its directory.");
        return false;
    }
    const auto AssetIt = std::find_if(OutManifest.Assets.begin(), OutManifest.Assets.end(),
        [](const SkiDomain::TerrainAsset& Asset) { return Asset.Path == "elevation.f32le"; });
    if (AssetIt == OutManifest.Assets.end())
    {
        OutError = TEXT("Elevation asset is missing from manifest.");
        return false;
    }
    uint64 TotalBytes = 0;
    for (const SkiDomain::TerrainAsset& Asset : OutManifest.Assets)
    {
        if (Asset.Length == 0 && !Asset.Required) continue;
        TArray<uint8> VerifiedBytes;
        const FString AssetPath = FPaths::Combine(Directory, UTF8_TO_TCHAR(Asset.Path.c_str()));
        if (!FFileHelper::LoadFileToArray(VerifiedBytes, *AssetPath)
            || static_cast<uint64>(VerifiedBytes.Num()) != Asset.Length
            || Sha256(VerifiedBytes) != UTF8_TO_TCHAR(Asset.Sha256.c_str())
            || TotalBytes > SkiDomain::MaxPackageBytes - Asset.Length)
        {
            OutError = TEXT("Package asset length, total size, or hash is invalid: ")
                + FString(UTF8_TO_TCHAR(Asset.Path.c_str()));
            return false;
        }
        if (OutCover && Asset.Path == "cover.u8") *OutCover = VerifiedBytes;
        TotalBytes += Asset.Length;
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Directory, TEXT("elevation.f32le")))
        || static_cast<uint64>(Bytes.Num()) != AssetIt->Length)
    {
        OutError = TEXT("Elevation asset length or hash is invalid.");
        return false;
    }
    const uint64 Samples = static_cast<uint64>(OutManifest.HeightWidth) * OutManifest.HeightHeight;
    if (Samples * sizeof(float) != static_cast<uint64>(Bytes.Num()))
    {
        OutError = TEXT("Elevation dimensions do not match its byte length.");
        return false;
    }
    OutHeightfield = {};
    OutHeightfield.Width = OutManifest.HeightWidth;
    OutHeightfield.Height = OutManifest.HeightHeight;
    OutHeightfield.WestM = 0.0;
    OutHeightfield.NorthM = static_cast<double>(OutManifest.HeightHeight - 1) * OutManifest.NorthSpacingM;
    OutHeightfield.EastSpacingM = OutManifest.EastSpacingM;
    OutHeightfield.NorthSpacingM = OutManifest.NorthSpacingM;
    OutHeightfield.NoDataValue = OutManifest.NoDataValue;
    OutHeightfield.CurrentRevision = 1;
    OutHeightfield.Samples.resize(Samples);
    FMemory::Memcpy(OutHeightfield.Samples.data(), Bytes.GetData(), Bytes.Num());
    if (OutCover && static_cast<uint64>(OutCover->Num())
        != static_cast<uint64>(OutManifest.CoverWidth) * OutManifest.CoverHeight)
    {
        OutError = TEXT("Cover dimensions do not match its byte length.");
        return false;
    }
    return SkiDomain::IsValidHeightfield(OutHeightfield);
}
