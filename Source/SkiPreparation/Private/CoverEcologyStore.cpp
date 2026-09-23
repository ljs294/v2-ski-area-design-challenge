#include "SkiPreparation/CoverEcologyStore.h"

#include "Algo/Reverse.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>

namespace
{
using namespace SkiDomain;
using namespace SkiPreparation;

bool LeaseCurrent(const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 Session, const uint64 Operation)
{
    return !Lease || Lease->IsCurrent(Session, Operation);
}

bool IsCanonicalId(const FString& Value)
{
    return IsTerrainCoreSha256(TCHAR_TO_UTF8(*Value));
}

bool IsWithin(const FString& Root, const FString& Candidate)
{
    FString Base = FPaths::ConvertRelativePathToFull(Root);
    FString Value = FPaths::ConvertRelativePathToFull(Candidate);
    FPaths::NormalizeDirectoryName(Base);
    FPaths::NormalizeFilename(Value);
    return Value.Equals(Base, ESearchCase::IgnoreCase)
        || Value.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase);
}

bool NoReparsePath(const FString& Path, const bool RequireLeaf, FString& Error)
{
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full);
    TArray<FString> Parts;
    for (FString Cursor = Full; !Cursor.IsEmpty();)
    {
        Parts.Add(Cursor);
        const FString Parent = FPaths::GetPath(Cursor);
        if (Parent.IsEmpty() || Parent.Equals(Cursor, ESearchCase::IgnoreCase)) break;
        Cursor = Parent;
    }
    Algo::Reverse(Parts);
    IFileManager& Files = IFileManager::Get();
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        const FString& Item = Parts[Index];
        const bool Exists = Files.FileExists(*Item) || Files.DirectoryExists(*Item);
        if (!Exists) continue;
        if (Files.IsSymlink(*Item))
        {
            Error = TEXT("CoverEcology storage path contains a reparse point.");
            return false;
        }
        if (Index + 1 < Parts.Num() && !Files.DirectoryExists(*Item))
        {
            Error = TEXT("CoverEcology storage ancestor is not a directory.");
            return false;
        }
    }
    if (RequireLeaf && !Files.FileExists(*Full) && !Files.DirectoryExists(*Full))
    {
        Error = TEXT("CoverEcology storage path is missing.");
        return false;
    }
    return true;
}

bool EnsureDirectory(const FString& Root, const FString& Directory, FString& Error)
{
    if (!IsWithin(Root, Directory) || !NoReparsePath(Root, false, Error)
        || !NoReparsePath(Directory, false, Error)
        || !IFileManager::Get().MakeDirectory(*Directory, true)
        || !NoReparsePath(Directory, true, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to create secure CoverEcology directory.");
        return false;
    }
    return true;
}

bool TreeHasNoReparse(const FString& Directory)
{
    FString Error;
    if (!NoReparsePath(Directory, true, Error)) return false;
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    bool Valid = true;
    TFunction<bool(const FString&)> Visit = [&](const FString& Current)
    {
        return Platform.IterateDirectory(*Current,
            [&](const TCHAR* Name, const bool IsDirectory)
            {
                const FString Entry(Name);
                if (IFileManager::Get().IsSymlink(*Entry))
                {
                    Valid = false;
                    return false;
                }
                return !IsDirectory || Visit(Entry);
            }) && Valid;
    };
    return Visit(Directory);
}

bool TreeContainsExactly(const FString& Directory, const TSet<FString>& Expected,
    FString& Error)
{
    if (!TreeHasNoReparse(Directory))
    {
        Error = TEXT("Content-addressed storage tree contains a reparse point.");
        return false;
    }
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    TSet<FString> Actual;
    bool Valid = true;
    TFunction<bool(const FString&)> Visit = [&](const FString& Current)
    {
        return Platform.IterateDirectory(*Current,
            [&](const TCHAR* Name, const bool IsDirectory)
            {
                const FString Entry(Name);
                if (IFileManager::Get().IsSymlink(*Entry))
                {
                    Valid = false;
                    return false;
                }
                if (IsDirectory) return Visit(Entry);
                FString Relative = Entry;
                const FString DirectoryPrefix = Directory.EndsWith(TEXT("/"))
                    ? Directory : Directory + TEXT("/");
                if (!FPaths::MakePathRelativeTo(Relative, *DirectoryPrefix))
                {
                    Valid = false;
                    return false;
                }
                FPaths::NormalizeFilename(Relative);
                Actual.Add(Relative.ToLower());
                return true;
            }) && Valid;
    };
    if (!Visit(Directory) || Actual.Num() != Expected.Num())
    {
        Error = TEXT("Content-addressed storage tree has missing or undeclared files.");
        return false;
    }
    for (const FString& Item : Expected)
    {
        if (!Actual.Contains(Item.ToLower()))
        {
            Error = TEXT("Content-addressed storage tree has missing or undeclared files.");
            return false;
        }
    }
    return true;
}

void SafeCleanup(const FString& Parent, const FString& Directory)
{
    if (IsWithin(Parent, Directory) && !FPaths::IsSamePath(Parent, Directory)
        && IFileManager::Get().DirectoryExists(*Directory) && TreeHasNoReparse(Directory))
    {
        IFileManager::Get().DeleteDirectory(*Directory, false, true);
    }
}

TArray<uint8> Utf8Bytes(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    TArray<uint8> Result;
    Result.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Result;
}

bool LoadBoundedFile(const FString& Root, const FString& Path, const uint64 Maximum,
    TArray<uint8>& Out, FString& Error)
{
    Out.Reset();
    if (!IsWithin(Root, Path) || !NoReparsePath(Path, true, Error)) return false;
    const int64 Size = IFileManager::Get().FileSize(*Path);
    if (Size <= 0 || static_cast<uint64>(Size) > Maximum || Size > MAX_int32)
    {
        Error = TEXT("CoverEcology asset is missing or oversized.");
        return false;
    }
    if (!FFileHelper::LoadFileToArray(Out, *Path) || Out.Num() != Size
        || !NoReparsePath(Path, true, Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to read CoverEcology asset completely.");
        Out.Reset();
        return false;
    }
    return true;
}

bool ValidWorldCoverChannels(const TArrayView<const uint8> Classes,
    const TArrayView<const uint8> Validity, FString& Error)
{
    if (Classes.IsEmpty() || Validity.Num() != (Classes.Num() + 7) / 8)
    {
        Error = TEXT("CoverEcology WorldCover channels have inconsistent lengths.");
        return false;
    }
    const auto ValidClass = [](const uint8 Value)
    {
        switch (Value)
        {
        case 10: case 20: case 30: case 40: case 50: case 60:
        case 70: case 80: case 90: case 95: case 100:
            return true;
        default:
            return false;
        }
    };
    for (int32 Index = 0; Index < Classes.Num(); ++Index)
    {
        const bool IsValid = (Validity[Index / 8] & static_cast<uint8>(1U << (Index % 8))) != 0;
        if ((IsValid && !ValidClass(Classes[Index])) || (!IsValid && Classes[Index] != 0))
        {
            Error = TEXT("CoverEcology contains an invalid class or nonzero nodata cell.");
            return false;
        }
    }
    const int32 UsedBits = Classes.Num() % 8;
    if (UsedBits != 0)
    {
        const uint8 PaddingMask = static_cast<uint8>(0xffU << UsedBits);
        if ((Validity.Last() & PaddingMask) != 0)
        {
            Error = TEXT("CoverEcology validity padding bits must be zero.");
            return false;
        }
    }
    return true;
}

TSharedRef<FJsonObject> BoundsJson(const GeographicBounds& Bounds)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("west"), Bounds.WestDeg);
    Object->SetNumberField(TEXT("south"), Bounds.SouthDeg);
    Object->SetNumberField(TEXT("east"), Bounds.EastDeg);
    Object->SetNumberField(TEXT("north"), Bounds.NorthDeg);
    return Object;
}

bool ReadBounds(const TSharedPtr<FJsonObject>& Object, GeographicBounds& Out)
{
    return Object && Object->TryGetNumberField(TEXT("west"), Out.WestDeg)
        && Object->TryGetNumberField(TEXT("south"), Out.SouthDeg)
        && Object->TryGetNumberField(TEXT("east"), Out.EastDeg)
        && Object->TryGetNumberField(TEXT("north"), Out.NorthDeg);
}

bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, std::string& Out)
{
    FString Value;
    if (!Object || !Object->TryGetStringField(Name, Value)) return false;
    Out = TCHAR_TO_UTF8(*Value);
    return true;
}

bool ReadUint32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, uint32& Out)
{
    double Value = 0.0;
    if (!Object || !Object->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value)
        || Value < 0.0 || Value > MAX_uint32 || FMath::FloorToDouble(Value) != Value)
        return false;
    Out = static_cast<uint32>(Value);
    return true;
}

bool ReadUint64String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, uint64& Out)
{
    FString Value;
    return Object && Object->TryGetStringField(Name, Value)
        && !Value.IsEmpty() && LexTryParseString(Out, *Value);
}

const TCHAR* StatusName(const OptionalSourceStatus Status)
{
    switch (Status)
    {
    case OptionalSourceStatus::Acquired: return TEXT("acquired");
    case OptionalSourceStatus::Unavailable: return TEXT("unavailable");
    case OptionalSourceStatus::NotRequested: return TEXT("not-requested");
    case OptionalSourceStatus::FailedOptional: return TEXT("failed-optional");
    default: return TEXT("invalid");
    }
}

bool ParseStatus(const FString& Value, OptionalSourceStatus& Out)
{
    if (Value == TEXT("acquired")) Out = OptionalSourceStatus::Acquired;
    else if (Value == TEXT("unavailable")) Out = OptionalSourceStatus::Unavailable;
    else if (Value == TEXT("not-requested")) Out = OptionalSourceStatus::NotRequested;
    else if (Value == TEXT("failed-optional")) Out = OptionalSourceStatus::FailedOptional;
    else return false;
    return true;
}

bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& Out, FString& Error)
{
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Out) || !Out)
    {
        Error = TEXT("Content-addressed metadata is not valid JSON.");
        return false;
    }
    return true;
}

bool OpenCoverAt(const FString& Root, const FString& Directory,
    const FString& ExpectedId, CoverEcologyPackageIndex& Out, FString& Error)
{
    Out = {};
    if (!IsCanonicalId(ExpectedId) || !IsWithin(Root, Directory)
        || !NoReparsePath(Directory, true, Error)) return false;
    TArray<uint8> ManifestBytes;
    const FString ManifestPath = FPaths::Combine(Directory, TEXT("manifest.json"));
    if (!LoadBoundedFile(Root, ManifestPath, CoverEcologyMaxManifestBytes,
        ManifestBytes, Error)) return false;
    ManifestBytes.Add(0);
    const FString Json = UTF8_TO_TCHAR(reinterpret_cast<const char*>(ManifestBytes.GetData()));
    CoverEcologyManifest Manifest;
    if (!ParseCoverEcologyManifest(Json, Manifest, Error)
        || UTF8_TO_TCHAR(Manifest.ContentId.c_str()) != ExpectedId)
    {
        if (Error.IsEmpty()) Error = TEXT("CoverEcology identity does not match its directory.");
        return false;
    }
    const FString Canonical = SerializeCoverEcologyManifest(Manifest, true);
    const TArray<uint8> CanonicalBytes = Utf8Bytes(Canonical);
    const FString DerivedId = Sha256(Utf8Bytes(
        SerializeCoverEcologyManifest(Manifest, false)));
    if (DerivedId != ExpectedId || CanonicalBytes.Num() + 1 != ManifestBytes.Num()
        || FMemory::Memcmp(CanonicalBytes.GetData(), ManifestBytes.GetData(),
            ManifestBytes.Num() - 1) != 0)
    {
        Error = TEXT("CoverEcology manifest is not canonical.");
        return false;
    }
    TArray<uint8> Classes;
    TArray<uint8> Validity;
    for (const CoverEcologyAsset& Asset : Manifest.Assets)
    {
        const FString AssetPath = FPaths::Combine(Directory,
            UTF8_TO_TCHAR(Asset.Path.c_str()));
        TArray<uint8> Bytes;
        if (!LoadBoundedFile(Root, AssetPath, CoverEcologyMaxAssetBytes, Bytes, Error)
            || static_cast<uint64>(Bytes.Num()) != Asset.Length
            || Sha256(Bytes) != UTF8_TO_TCHAR(Asset.Sha256.c_str()))
        {
            if (Error.IsEmpty()) Error = TEXT("CoverEcology asset verification failed.");
            return false;
        }
        if (Asset.Type == "semantic-cover-u8") Classes = MoveTemp(Bytes);
        else Validity = MoveTemp(Bytes);
    }
    if (!ValidWorldCoverChannels(Classes, Validity, Error)) return false;
    TSet<FString> ExpectedFiles{TEXT("manifest.json")};
    for (const CoverEcologyAsset& Asset : Manifest.Assets)
        ExpectedFiles.Add(UTF8_TO_TCHAR(Asset.Path.c_str()));
    if (!TreeContainsExactly(Directory, ExpectedFiles, Error)) return false;
    Out.Manifest = std::move(Manifest);
    Out.PackageDirectory = Directory;
    return true;
}

bool OpenReceiptAt(const FString& Root, const FString& Directory,
    const FString& ExpectedId, InstalledTerrainIndex& Out, FString& Error)
{
    Out = {};
    TArray<uint8> Bytes;
    if (!IsCanonicalId(ExpectedId) || !IsWithin(Root, Directory)
        || !LoadBoundedFile(Root, FPaths::Combine(Directory, TEXT("receipt.json")),
            InstalledTerrainMaxReceiptBytes, Bytes, Error)) return false;
    Bytes.Add(0);
    const FString Json = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
    InstalledTerrainReceipt Receipt;
    if (!ParseInstalledTerrainReceipt(Json, Receipt, Error)
        || UTF8_TO_TCHAR(Receipt.ContentId.c_str()) != ExpectedId)
    {
        if (Error.IsEmpty()) Error = TEXT("Installed-terrain identity does not match its directory.");
        return false;
    }
    const TArray<uint8> Canonical = Utf8Bytes(SerializeInstalledTerrainReceipt(Receipt, true));
    const FString DerivedId = Sha256(Utf8Bytes(
        SerializeInstalledTerrainReceipt(Receipt, false)));
    if (DerivedId != ExpectedId || Canonical.Num() + 1 != Bytes.Num()
        || FMemory::Memcmp(Canonical.GetData(), Bytes.GetData(), Canonical.Num()) != 0)
    {
        Error = TEXT("Installed-terrain receipt is not canonical.");
        return false;
    }
    const TSet<FString> ExpectedFiles{TEXT("receipt.json")};
    if (!TreeContainsExactly(Directory, ExpectedFiles, Error)) return false;
    Out.Receipt = std::move(Receipt);
    Out.ReceiptDirectory = Directory;
    return true;
}
}

FString SkiPreparation::SerializeCoverEcologyManifest(
    const CoverEcologyManifest& Manifest, const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
    if (IncludeContentId)
        Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Manifest.GeneratorVersion.c_str()));
    Root->SetStringField(TEXT("coverRevision"), LexToString(Manifest.CoverRevision));
    TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
    Source->SetStringField(TEXT("sourceId"), UTF8_TO_TCHAR(Manifest.Source.SourceId.c_str()));
    Source->SetStringField(TEXT("product"), UTF8_TO_TCHAR(Manifest.Source.Product.c_str()));
    Source->SetStringField(TEXT("acquisitionEpoch"), UTF8_TO_TCHAR(Manifest.Source.AcquisitionEpoch.c_str()));
    Source->SetStringField(TEXT("provenance"), UTF8_TO_TCHAR(Manifest.Source.Provenance.c_str()));
    Source->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Manifest.Source.License.c_str()));
    Source->SetStringField(TEXT("attribution"), UTF8_TO_TCHAR(Manifest.Source.Attribution.c_str()));
    Root->SetObjectField(TEXT("source"), Source);
    TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
    Transform->SetNumberField(TEXT("width"), Manifest.Transform.Width);
    Transform->SetNumberField(TEXT("height"), Manifest.Transform.Height);
    Transform->SetNumberField(TEXT("longitudeStepDeg"), Manifest.Transform.LongitudeStepDeg);
    Transform->SetNumberField(TEXT("latitudeStepDeg"), Manifest.Transform.LatitudeStepDeg);
    Transform->SetObjectField(TEXT("sampleCenterBounds"), BoundsJson(Manifest.Transform.SampleCenterBounds));
    Transform->SetObjectField(TEXT("outerBounds"), BoundsJson(Manifest.Transform.OuterBounds));
    Transform->SetStringField(TEXT("horizontalCrs"), UTF8_TO_TCHAR(Manifest.Transform.HorizontalCrs.c_str()));
    Transform->SetStringField(TEXT("pixelRegistration"), UTF8_TO_TCHAR(Manifest.Transform.PixelRegistration.c_str()));
    Transform->SetStringField(TEXT("rowOrientation"), UTF8_TO_TCHAR(Manifest.Transform.RowOrientation.c_str()));
    Root->SetObjectField(TEXT("transform"), Transform);
    Root->SetStringField(TEXT("semanticClassEncoding"), UTF8_TO_TCHAR(Manifest.SemanticClassEncoding.c_str()));
    Root->SetStringField(TEXT("validityEncoding"), UTF8_TO_TCHAR(Manifest.ValidityEncoding.c_str()));
    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const CoverEcologyAsset& Asset : Manifest.Assets)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("path"), UTF8_TO_TCHAR(Asset.Path.c_str()));
        Object->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Asset.Type.c_str()));
        Object->SetStringField(TEXT("sha256"), UTF8_TO_TCHAR(Asset.Sha256.c_str()));
        Object->SetStringField(TEXT("length"), LexToString(Asset.Length));
        Assets.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("assets"), Assets);
    FString Output;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Output, 0));
    return Output;
}

bool SkiPreparation::ParseCoverEcologyManifest(const FString& Json,
    CoverEcologyManifest& OutManifest, FString& OutError)
{
    OutManifest = {};
    const FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0
        || static_cast<uint64>(Encoded.Length()) > CoverEcologyMaxManifestBytes)
    {
        OutError = TEXT("CoverEcology manifest is empty or oversized.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!ParseJsonObject(Json, Root, OutError)) return false;
    CoverEcologyManifest Value;
    uint32 RevisionSchema = 0;
    uint64 Revision = 0;
    const TSharedPtr<FJsonObject>* Source = nullptr;
    const TSharedPtr<FJsonObject>* Transform = nullptr;
    if (!ReadUint32(Root, TEXT("schemaVersion"), RevisionSchema)
        || !ReadString(Root, TEXT("contentId"), Value.ContentId)
        || !ReadString(Root, TEXT("generatorVersion"), Value.GeneratorVersion)
        || !ReadUint64String(Root, TEXT("coverRevision"), Revision)
        || !Root->TryGetObjectField(TEXT("source"), Source) || !Source
        || !Root->TryGetObjectField(TEXT("transform"), Transform) || !Transform)
    {
        OutError = TEXT("CoverEcology manifest fields are missing or invalid.");
        return false;
    }
    Value.SchemaVersion = RevisionSchema;
    Value.CoverRevision = Revision;
    if (!ReadString(*Source, TEXT("sourceId"), Value.Source.SourceId)
        || !ReadString(*Source, TEXT("product"), Value.Source.Product)
        || !ReadString(*Source, TEXT("acquisitionEpoch"), Value.Source.AcquisitionEpoch)
        || !ReadString(*Source, TEXT("provenance"), Value.Source.Provenance)
        || !ReadString(*Source, TEXT("license"), Value.Source.License)
        || !ReadString(*Source, TEXT("attribution"), Value.Source.Attribution)
        || !ReadUint32(*Transform, TEXT("width"), Value.Transform.Width)
        || !ReadUint32(*Transform, TEXT("height"), Value.Transform.Height)
        || !(*Transform)->TryGetNumberField(TEXT("longitudeStepDeg"), Value.Transform.LongitudeStepDeg)
        || !(*Transform)->TryGetNumberField(TEXT("latitudeStepDeg"), Value.Transform.LatitudeStepDeg)
        || !ReadString(*Transform, TEXT("horizontalCrs"), Value.Transform.HorizontalCrs)
        || !ReadString(*Transform, TEXT("pixelRegistration"), Value.Transform.PixelRegistration)
        || !ReadString(*Transform, TEXT("rowOrientation"), Value.Transform.RowOrientation)
        || !ReadString(Root, TEXT("semanticClassEncoding"), Value.SemanticClassEncoding)
        || !ReadString(Root, TEXT("validityEncoding"), Value.ValidityEncoding))
    {
        OutError = TEXT("CoverEcology source or transform is invalid.");
        return false;
    }
    const TSharedPtr<FJsonObject>* Centers = nullptr;
    const TSharedPtr<FJsonObject>* Outer = nullptr;
    if (!(*Transform)->TryGetObjectField(TEXT("sampleCenterBounds"), Centers) || !Centers
        || !(*Transform)->TryGetObjectField(TEXT("outerBounds"), Outer) || !Outer
        || !ReadBounds(*Centers, Value.Transform.SampleCenterBounds)
        || !ReadBounds(*Outer, Value.Transform.OuterBounds))
    {
        OutError = TEXT("CoverEcology geographic bounds are invalid.");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets
        || Assets->Num() > static_cast<int32>(CoverEcologyMaxAssets))
    {
        OutError = TEXT("CoverEcology assets are missing or oversized.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Item : *Assets)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        CoverEcologyAsset Asset;
        if (!Object || !ReadString(Object, TEXT("path"), Asset.Path)
            || !ReadString(Object, TEXT("type"), Asset.Type)
            || !ReadString(Object, TEXT("sha256"), Asset.Sha256)
            || !ReadUint64String(Object, TEXT("length"), Asset.Length))
        {
            OutError = TEXT("CoverEcology asset fields are invalid.");
            return false;
        }
        Value.Assets.push_back(std::move(Asset));
    }
    const CoverEcologyValidation Validation = ValidateCoverEcology(Value,
        static_cast<uint64>(Encoded.Length()));
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("CoverEcology validation failed (%d, item %llu)."),
            static_cast<int32>(Validation.Error), static_cast<uint64>(Validation.Index));
        return false;
    }
    OutManifest = std::move(Value);
    return true;
}

FString SkiPreparation::SerializeInstalledTerrainReceipt(
    const InstalledTerrainReceipt& Receipt, const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Receipt.SchemaVersion);
    if (IncludeContentId)
        Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Receipt.ContentId.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Receipt.GeneratorVersion.c_str()));
    Root->SetStringField(TEXT("terrainCoreId"), UTF8_TO_TCHAR(Receipt.TerrainCoreId.c_str()));
    Root->SetStringField(TEXT("surroundTerrainCoreId"), UTF8_TO_TCHAR(Receipt.SurroundTerrainCoreId.c_str()));
    Root->SetStringField(TEXT("coverEcologyId"), UTF8_TO_TCHAR(Receipt.CoverEcologyId.c_str()));
    TArray<TSharedPtr<FJsonValue>> Outcomes;
    for (const OptionalSourceOutcome& Outcome : Receipt.OptionalSources)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("sourceId"), UTF8_TO_TCHAR(Outcome.SourceId.c_str()));
        Object->SetStringField(TEXT("product"), UTF8_TO_TCHAR(Outcome.Product.c_str()));
        Object->SetStringField(TEXT("status"), StatusName(Outcome.Status));
        Object->SetStringField(TEXT("artifactId"), UTF8_TO_TCHAR(Outcome.ArtifactId.c_str()));
        Object->SetStringField(TEXT("reasonCode"), UTF8_TO_TCHAR(Outcome.ReasonCode.c_str()));
        Object->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Outcome.License.c_str()));
        Object->SetStringField(TEXT("attribution"), UTF8_TO_TCHAR(Outcome.Attribution.c_str()));
        Outcomes.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("optionalSources"), Outcomes);
    FString Output;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Output, 0));
    return Output;
}

bool SkiPreparation::ParseInstalledTerrainReceipt(const FString& Json,
    InstalledTerrainReceipt& OutReceipt, FString& OutError)
{
    OutReceipt = {};
    const FTCHARToUTF8 Encoded(*Json);
    if (Encoded.Length() <= 0
        || static_cast<uint64>(Encoded.Length()) > InstalledTerrainMaxReceiptBytes)
    {
        OutError = TEXT("Installed-terrain receipt is empty or oversized.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!ParseJsonObject(Json, Root, OutError)) return false;
    InstalledTerrainReceipt Value;
    if (!ReadUint32(Root, TEXT("schemaVersion"), Value.SchemaVersion)
        || !ReadString(Root, TEXT("contentId"), Value.ContentId)
        || !ReadString(Root, TEXT("generatorVersion"), Value.GeneratorVersion)
        || !ReadString(Root, TEXT("terrainCoreId"), Value.TerrainCoreId)
        || !ReadString(Root, TEXT("surroundTerrainCoreId"), Value.SurroundTerrainCoreId)
        || !ReadString(Root, TEXT("coverEcologyId"), Value.CoverEcologyId))
    {
        OutError = TEXT("Installed-terrain receipt fields are missing or invalid.");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Outcomes = nullptr;
    if (!Root->TryGetArrayField(TEXT("optionalSources"), Outcomes) || !Outcomes
        || Outcomes->Num() > static_cast<int32>(InstalledTerrainMaxOptionalSources))
    {
        OutError = TEXT("Installed-terrain optional-source outcomes are invalid.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Item : *Outcomes)
    {
        const TSharedPtr<FJsonObject> Object = Item ? Item->AsObject() : nullptr;
        OptionalSourceOutcome Outcome;
        FString Status;
        if (!Object || !ReadString(Object, TEXT("sourceId"), Outcome.SourceId)
            || !ReadString(Object, TEXT("product"), Outcome.Product)
            || !Object->TryGetStringField(TEXT("status"), Status)
            || !ParseStatus(Status, Outcome.Status)
            || !ReadString(Object, TEXT("artifactId"), Outcome.ArtifactId)
            || !ReadString(Object, TEXT("reasonCode"), Outcome.ReasonCode)
            || !ReadString(Object, TEXT("license"), Outcome.License)
            || !ReadString(Object, TEXT("attribution"), Outcome.Attribution))
        {
            OutError = TEXT("Installed-terrain optional-source outcome is invalid.");
            return false;
        }
        Value.OptionalSources.push_back(std::move(Outcome));
    }
    const CoverEcologyValidation Validation = ValidateInstalledTerrainReceipt(Value,
        static_cast<uint64>(Encoded.Length()));
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("Installed-terrain receipt validation failed (%d, item %llu)."),
            static_cast<int32>(Validation.Error), static_cast<uint64>(Validation.Index));
        return false;
    }
    OutReceipt = std::move(Value);
    return true;
}

SkiPreparation::CoverEcologyStore::CoverEcologyStore(FString InDataRoot)
    : Root(FPaths::ConvertRelativePathToFull(std::move(InDataRoot)))
{
}

bool SkiPreparation::CoverEcologyStore::WriteAndActivate(CoverEcologyManifest Manifest,
    const TArrayView<const uint8> Classes, const TArrayView<const uint8> Validity,
    FString& OutPackageDirectory, CoverEcologyManifest& OutManifest, FString& OutError,
    const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration) const
{
    OutPackageDirectory.Reset();
    OutManifest = {};
    OutError.Reset();
    if (!LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
    {
        OutError = TEXT("CoverEcology operation is no longer current.");
        return false;
    }
    const uint64 Cells = static_cast<uint64>(Manifest.Transform.Width) * Manifest.Transform.Height;
    if (Cells > MAX_int32 || Classes.Num() != static_cast<int32>(Cells)
        || Validity.Num() != static_cast<int32>((Cells + 7ULL) / 8ULL))
    {
        OutError = TEXT("CoverEcology channel byte counts do not match the grid.");
        return false;
    }
    if (!ValidWorldCoverChannels(Classes, Validity, OutError)) return false;
    const TArray<uint8> ClassCopy(Classes);
    const TArray<uint8> ValidityCopy(Validity);
    Manifest.ContentId.clear();
    Manifest.Assets = {
        {"channels/classes.u8", "semantic-cover-u8", TCHAR_TO_UTF8(*Sha256(ClassCopy)),
            static_cast<uint64>(ClassCopy.Num())},
        {"channels/validity.bits", "validity-bitset", TCHAR_TO_UTF8(*Sha256(ValidityCopy)),
            static_cast<uint64>(ValidityCopy.Num())},
    };
    Manifest.ContentId = std::string(64, '0');
    if (!ValidateCoverEcology(Manifest).Ok())
    {
        OutError = TEXT("CoverEcology input metadata is invalid.");
        return false;
    }
    Manifest.ContentId.clear();
    Manifest.ContentId = TCHAR_TO_UTF8(*Sha256(Utf8Bytes(
        SerializeCoverEcologyManifest(Manifest, false))));
    const FString ManifestJson = SerializeCoverEcologyManifest(Manifest, true);
    const FTCHARToUTF8 EncodedManifest(*ManifestJson);
    if (!ValidateCoverEcology(Manifest, static_cast<uint64>(EncodedManifest.Length())).Ok())
    {
        OutError = TEXT("CoverEcology derived manifest is invalid.");
        return false;
    }

    const FString StagingParent = FPaths::Combine(Root, TEXT(".coverecology-staging"));
    const FString Stage = FPaths::Combine(StagingParent,
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".work"));
    const FString Channels = FPaths::Combine(Stage, TEXT("channels"));
    if (!EnsureDirectory(Root, StagingParent, OutError)
        || !EnsureDirectory(Root, Channels, OutError))
    {
        SafeCleanup(StagingParent, Stage);
        return false;
    }
    const auto Cleanup = [&]() { SafeCleanup(StagingParent, Stage); };
    if (!FFileHelper::SaveArrayToFile(ClassCopy, *FPaths::Combine(Channels, TEXT("classes.u8")))
        || !FFileHelper::SaveArrayToFile(ValidityCopy,
            *FPaths::Combine(Channels, TEXT("validity.bits")))
        || !FFileHelper::SaveStringToFile(ManifestJson,
            *FPaths::Combine(Stage, TEXT("manifest.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = TEXT("Unable to write CoverEcology staging package.");
        Cleanup();
        return false;
    }
    const FString Id = UTF8_TO_TCHAR(Manifest.ContentId.c_str());
    CoverEcologyPackageIndex Staged;
    if (!OpenCoverAt(Root, Stage, Id, Staged, OutError)
        || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
    {
        if (OutError.IsEmpty()) OutError = TEXT("CoverEcology operation became stale before activation.");
        Cleanup();
        return false;
    }
    const FString Packages = FPaths::Combine(Root, TEXT("CoverEcology"));
    const FString Target = FPaths::Combine(Packages, Id);
    if (!EnsureDirectory(Root, Packages, OutError))
    {
        Cleanup();
        return false;
    }
    if (IFileManager::Get().DirectoryExists(*Target))
    {
        CoverEcologyPackageIndex Existing;
        if (!OpenCoverAt(Root, Target, Id, Existing, OutError)
            || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
        {
            Cleanup();
            return false;
        }
        Cleanup();
    }
    else
    {
        if (!NoReparsePath(Packages, true, OutError))
        {
            Cleanup();
            return false;
        }
        bool Moved = false;
        const auto Activate = [&]()
        {
            Moved = IFileManager::Get().Move(*Target, *Stage, false, false, true, true);
        };
        const bool ActivationAuthorized = Lease
            ? Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Activate)
            : (Activate(), true);
        if (!ActivationAuthorized || !Moved)
        {
            if (OutError.IsEmpty()) OutError = TEXT("Unable to atomically activate CoverEcology.");
            Cleanup();
            return false;
        }
    }
    CoverEcologyPackageIndex Final;
    if (!OpenCoverAt(Root, Target, Id, Final, OutError)
        || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
    {
        if (OutError.IsEmpty()) OutError = TEXT("CoverEcology operation became stale before publication.");
        return false;
    }
    OutPackageDirectory = Target;
    OutManifest = std::move(Final.Manifest);
    return true;
}

bool SkiPreparation::CoverEcologyStore::Open(const FString& ContentId,
    CoverEcologyPackageIndex& OutIndex, FString& OutError) const
{
    return OpenCoverAt(Root, FPaths::Combine(Root, TEXT("CoverEcology"), ContentId),
        ContentId, OutIndex, OutError);
}

bool SkiPreparation::CoverEcologyStore::Verify(const CoverEcologyPackageIndex& Index,
    FString& OutError) const
{
    CoverEcologyPackageIndex Verified;
    return Open(UTF8_TO_TCHAR(Index.Manifest.ContentId.c_str()), Verified, OutError);
}

bool SkiPreparation::CoverEcologyStore::ReadChannels(const CoverEcologyPackageIndex& Index,
    TArray<uint8>& OutClasses, TArray<uint8>& OutValidity, FString& OutError) const
{
    OutClasses.Reset();
    OutValidity.Reset();
    CoverEcologyPackageIndex Verified;
    if (!Open(UTF8_TO_TCHAR(Index.Manifest.ContentId.c_str()), Verified, OutError)) return false;
    for (const CoverEcologyAsset& Asset : Verified.Manifest.Assets)
    {
        TArray<uint8>& Output = Asset.Type == "semantic-cover-u8" ? OutClasses : OutValidity;
        if (!LoadBoundedFile(Root, FPaths::Combine(Verified.PackageDirectory,
            UTF8_TO_TCHAR(Asset.Path.c_str())), CoverEcologyMaxAssetBytes, Output, OutError))
        {
            OutClasses.Reset();
            OutValidity.Reset();
            return false;
        }
    }
    return true;
}

SkiPreparation::InstalledTerrainStore::InstalledTerrainStore(FString InDataRoot)
    : Root(FPaths::ConvertRelativePathToFull(std::move(InDataRoot)))
{
}

bool SkiPreparation::InstalledTerrainStore::WriteAndActivate(InstalledTerrainReceipt Receipt,
    FString& OutReceiptDirectory, InstalledTerrainReceipt& OutReceipt, FString& OutError,
    const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration) const
{
    OutReceiptDirectory.Reset();
    OutReceipt = {};
    OutError.Reset();
    if (!LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
    {
        OutError = TEXT("Installed-terrain operation is no longer current.");
        return false;
    }
    TerrainCorePackageStore CoreStore(Root);
    CoverEcologyStore CoverStore(Root);
    TerrainCorePackageIndex Core;
    TerrainCorePackageIndex Surround;
    CoverEcologyPackageIndex Cover;
    const FString CoreId = UTF8_TO_TCHAR(Receipt.TerrainCoreId.c_str());
    const FString SurroundId = UTF8_TO_TCHAR(Receipt.SurroundTerrainCoreId.c_str());
    const FString CoverId = UTF8_TO_TCHAR(Receipt.CoverEcologyId.c_str());
    if (!CoreStore.Open(CoreId, Core, OutError) || !CoreStore.Verify(Core, OutError)
        || !CoreStore.Open(SurroundId, Surround, OutError) || !CoreStore.Verify(Surround, OutError)
        || !CoverStore.Open(CoverId, Cover, OutError) || !CoverStore.Verify(Cover, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Installed-terrain components could not be reopened and verified.");
        return false;
    }
    Receipt.ContentId = std::string(64, '0');
    if (!ValidateInstalledTerrainReceipt(Receipt).Ok())
    {
        OutError = TEXT("Installed-terrain receipt input is invalid.");
        return false;
    }
    Receipt.ContentId.clear();
    Receipt.ContentId = TCHAR_TO_UTF8(*Sha256(Utf8Bytes(
        SerializeInstalledTerrainReceipt(Receipt, false))));
    const FString Json = SerializeInstalledTerrainReceipt(Receipt, true);
    const FTCHARToUTF8 Encoded(*Json);
    if (!ValidateInstalledTerrainReceipt(Receipt, static_cast<uint64>(Encoded.Length())).Ok())
    {
        OutError = TEXT("Installed-terrain receipt is invalid.");
        return false;
    }
    const FString StagingParent = FPaths::Combine(Root, TEXT(".installedterrain-staging"));
    const FString Stage = FPaths::Combine(StagingParent,
        FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".work"));
    if (!EnsureDirectory(Root, StagingParent, OutError)
        || !EnsureDirectory(Root, Stage, OutError))
    {
        SafeCleanup(StagingParent, Stage);
        return false;
    }
    const auto Cleanup = [&]() { SafeCleanup(StagingParent, Stage); };
    if (!FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Stage, TEXT("receipt.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = TEXT("Unable to write installed-terrain staging receipt.");
        Cleanup();
        return false;
    }
    const FString Id = UTF8_TO_TCHAR(Receipt.ContentId.c_str());
    InstalledTerrainIndex Staged;
    if (!OpenReceiptAt(Root, Stage, Id, Staged, OutError)
        || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration)
        || !CoreStore.Open(CoreId, Core, OutError) || !CoreStore.Verify(Core, OutError)
        || !CoverStore.Open(CoverId, Cover, OutError) || !CoverStore.Verify(Cover, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Installed-terrain operation became stale or a component changed.");
        Cleanup();
        return false;
    }
    const FString Receipts = FPaths::Combine(Root, TEXT("InstalledTerrain"));
    const FString Target = FPaths::Combine(Receipts, Id);
    if (!EnsureDirectory(Root, Receipts, OutError))
    {
        Cleanup();
        return false;
    }
    if (IFileManager::Get().DirectoryExists(*Target))
    {
        InstalledTerrainIndex Existing;
        if (!OpenReceiptAt(Root, Target, Id, Existing, OutError)
            || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
        {
            Cleanup();
            return false;
        }
        Cleanup();
    }
    else
    {
        if (!NoReparsePath(Receipts, true, OutError))
        {
            Cleanup();
            return false;
        }
        bool Moved = false;
        const auto Activate = [&]()
        {
            Moved = IFileManager::Get().Move(*Target, *Stage, false, false, true, true);
        };
        const bool ActivationAuthorized = Lease
            ? Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Activate)
            : (Activate(), true);
        if (!ActivationAuthorized || !Moved)
        {
            if (OutError.IsEmpty()) OutError = TEXT("Unable to atomically activate installed terrain.");
            Cleanup();
            return false;
        }
    }
    InstalledTerrainIndex Final;
    if (!Open(Id, Final, OutError)
        || !LeaseCurrent(Lease, SessionGeneration, OperationGeneration))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Installed-terrain operation became stale before publication.");
        return false;
    }
    OutReceiptDirectory = Target;
    OutReceipt = std::move(Final.Receipt);
    return true;
}

bool SkiPreparation::InstalledTerrainStore::Open(const FString& ContentId,
    InstalledTerrainIndex& OutIndex, FString& OutError) const
{
    OutIndex = {};
    InstalledTerrainIndex Candidate;
    if (!OpenReceiptAt(Root, FPaths::Combine(Root, TEXT("InstalledTerrain"), ContentId),
        ContentId, Candidate, OutError)) return false;
    TerrainCorePackageStore CoreStore(Root);
    CoverEcologyStore CoverStore(Root);
    TerrainCorePackageIndex Core;
    TerrainCorePackageIndex Surround;
    CoverEcologyPackageIndex Cover;
    if (!CoreStore.Open(UTF8_TO_TCHAR(Candidate.Receipt.TerrainCoreId.c_str()), Core, OutError)
        || !CoreStore.Verify(Core, OutError)
        || !CoreStore.Open(UTF8_TO_TCHAR(Candidate.Receipt.SurroundTerrainCoreId.c_str()), Surround, OutError)
        || !CoreStore.Verify(Surround, OutError)
        || !CoverStore.Open(UTF8_TO_TCHAR(Candidate.Receipt.CoverEcologyId.c_str()), Cover, OutError)
        || !CoverStore.Verify(Cover, OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Installed-terrain component verification failed.");
        return false;
    }
    OutIndex = std::move(Candidate);
    return true;
}
