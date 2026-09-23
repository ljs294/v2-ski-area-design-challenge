#include "SkiPreparation/TerrainCorePackageStore.h"

#include "Dom/JsonObject.h"
#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkiPreparation/TerrainPackageStore.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <winternl.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <openssl/sha.h>

namespace
{
constexpr uint64 TerrainCoreShardTargetBytes = 256ULL * 1024ULL * 1024ULL;
bool ValidateNoReparsePath(const FString& Path, bool RequireLeaf, FString* Error);
bool IsWithinDirectory(const FString& Directory, const FString& Candidate);

#if PLATFORM_WINDOWS
struct FScopedTerrainHandle
{
    HANDLE Value = INVALID_HANDLE_VALUE;
    FScopedTerrainHandle() = default;
    explicit FScopedTerrainHandle(const HANDLE InValue) : Value(InValue) {}
    ~FScopedTerrainHandle() { Reset(); }
    FScopedTerrainHandle(const FScopedTerrainHandle&) = delete;
    FScopedTerrainHandle& operator=(const FScopedTerrainHandle&) = delete;
    FScopedTerrainHandle(FScopedTerrainHandle&& Other) noexcept : Value(Other.Release()) {}
    FScopedTerrainHandle& operator=(FScopedTerrainHandle&& Other) noexcept
    {
        if (this != &Other) { Reset(); Value = Other.Release(); }
        return *this;
    }
    void Reset(HANDLE NewValue = INVALID_HANDLE_VALUE)
    {
        if (Value != INVALID_HANDLE_VALUE) CloseHandle(Value);
        Value = NewValue;
    }
    HANDLE Release()
    {
        const HANDLE Result = Value;
        Value = INVALID_HANDLE_VALUE;
        return Result;
    }
    explicit operator bool() const { return Value != INVALID_HANDLE_VALUE; }
};

using FNtCreateFile = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using FNtSetInformationFile = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID,
    ULONG, FILE_INFORMATION_CLASS);

struct FTerrainFileRenameInformation
{
    BOOLEAN ReplaceIfExists = 0;
    HANDLE RootDirectory = nullptr;
    ULONG FileNameLength = 0;
    WCHAR FileName[1]{};
};

FNtCreateFile TerrainNtCreateFile()
{
    static FNtCreateFile Function = []()
    {
        const FARPROC Address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
        FNtCreateFile Result = nullptr;
        static_assert(sizeof(Result) == sizeof(Address));
        FMemory::Memcpy(&Result, &Address, sizeof(Result));
        return Result;
    }();
    return Function;
}

FNtSetInformationFile TerrainNtSetInformationFile()
{
    static FNtSetInformationFile Function = []()
    {
        const FARPROC Address = GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
        FNtSetInformationFile Result = nullptr;
        static_assert(sizeof(Result) == sizeof(Address));
        FMemory::Memcpy(&Result, &Address, sizeof(Result));
        return Result;
    }();
    return Function;
}

FString NormalizeFinalPath(FString Value)
{
    Value.ReplaceInline(TEXT("\\\\?\\UNC\\"), TEXT("//"), ESearchCase::IgnoreCase);
    Value.ReplaceInline(TEXT("\\\\?\\"), TEXT(""), ESearchCase::IgnoreCase);
    Value.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (Value.Len() > 3 && Value.EndsWith(TEXT("/"))) Value.LeftChopInline(1);
    return Value;
}

bool FinalPathForHandle(const HANDLE Handle, FString& Out)
{
    TArray<TCHAR> Buffer;
    Buffer.SetNumUninitialized(512);
    for (;;)
    {
        const DWORD Written = GetFinalPathNameByHandleW(Handle, Buffer.GetData(),
            static_cast<DWORD>(Buffer.Num()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (Written == 0) return false;
        if (Written < static_cast<DWORD>(Buffer.Num()))
        {
            Out = NormalizeFinalPath(FString(static_cast<int32>(Written), Buffer.GetData()));
            return true;
        }
        if (Written > 32768U) return false;
        Buffer.SetNumUninitialized(static_cast<int32>(Written + 1U));
    }
}

bool CanonicalWithin(const FString& Root, const FString& Candidate)
{
    const FString Prefix = Root.EndsWith(TEXT("/")) ? Root : Root + TEXT("/");
    return Candidate.Equals(Root, ESearchCase::IgnoreCase)
        || Candidate.StartsWith(Prefix, ESearchCase::IgnoreCase);
}

bool HandleAttributes(const HANDLE Handle, DWORD& OutAttributes)
{
    FILE_ATTRIBUTE_TAG_INFO Info{};
    if (!GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Info, sizeof(Info)))
        return false;
    OutAttributes = Info.FileAttributes;
    return true;
}

bool OpenRelativeHandle(const HANDLE Parent, const FString& Name, const bool Directory,
    const bool Create, const bool AllowReparse, const bool DeleteAccess,
    const bool WriteChildren,
    FScopedTerrainHandle& Out, const bool ReadData = false,
    const ULONG ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
{
    Out.Reset();
    if (Name.IsEmpty() || Name == TEXT(".") || Name == TEXT("..")
        || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\"))) return false;
    FNtCreateFile NtCreate = TerrainNtCreateFile();
    if (!NtCreate) return false;
    UNICODE_STRING NativeName{};
    NativeName.Buffer = const_cast<PWCH>(*Name);
    NativeName.Length = static_cast<USHORT>(Name.Len() * sizeof(TCHAR));
    NativeName.MaximumLength = NativeName.Length;
    OBJECT_ATTRIBUTES Attributes{};
    Attributes.Length = sizeof(Attributes);
    Attributes.RootDirectory = Parent;
    Attributes.ObjectName = &NativeName;
    Attributes.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK Io{};
    HANDLE Handle = INVALID_HANDLE_VALUE;
    const ACCESS_MASK Access = Directory
        ? FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE
            | (WriteChildren ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : 0U)
            | (DeleteAccess ? DELETE : 0U)
        : (Create ? FILE_GENERIC_READ | FILE_GENERIC_WRITE
            : FILE_READ_ATTRIBUTES | (ReadData ? FILE_READ_DATA : 0U))
            | SYNCHRONIZE | (DeleteAccess ? DELETE : 0U);
    const ULONG Options = (Directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE)
        | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT;
    const ULONG Disposition = Create ? (Directory ? 3UL : 2UL) : 1UL;
    const NTSTATUS Status = NtCreate(&Handle, Access, &Attributes, &Io, nullptr,
        FILE_ATTRIBUTE_NORMAL, ShareAccess,
        Disposition, Options, nullptr, 0);
    if (Status < 0 || Handle == INVALID_HANDLE_VALUE) return false;
    Out.Reset(Handle);
    DWORD FileAttributes = 0;
    if (!HandleAttributes(Handle, FileAttributes)
        || (!AllowReparse && (FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
    {
        Out.Reset();
        return false;
    }
    return true;
}

bool OpenPinnedAbsoluteDirectory(const FString& Path, const bool Create,
    FScopedTerrainHandle& Out, FString& OutCanonical, const bool DeleteAccess = false,
    const bool WriteChildren = false)
{
    Out.Reset();
    OutCanonical.Reset();
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full);
    if (Full.Len() < 3 || Full[1] != TEXT(':') || Full[2] != TEXT('/')) return false;
    const FString Volume = Full.Left(3).Replace(TEXT("/"), TEXT("\\"));
    FScopedTerrainHandle Current(CreateFileW(*Volume,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    DWORD VolumeAttributes = 0;
    FString VolumeCanonical;
    if (!Current || !HandleAttributes(Current.Value, VolumeAttributes)
        || (VolumeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || !FinalPathForHandle(Current.Value, VolumeCanonical)) return false;
    TArray<FString> Segments;
    Full.Mid(3).ParseIntoArray(Segments, TEXT("/"), true);
    int32 FirstMissing = Segments.Num();
    if (Create)
    {
        FString Probe = Full.Left(3);
        for (int32 Index = 0; Index < Segments.Num(); ++Index)
        {
            Probe = FPaths::Combine(Probe, Segments[Index]);
            if (!IFileManager::Get().DirectoryExists(*Probe))
            {
                FirstMissing = Index;
                break;
            }
        }
    }
    for (int32 Index = 0; Index < Segments.Num(); ++Index)
    {
        const FString& Segment = Segments[Index];
        FScopedTerrainHandle Child;
        if (!OpenRelativeHandle(Current.Value, Segment, true, Create, false,
            DeleteAccess && Index + 1 == Segments.Num(),
            (WriteChildren && Index + 1 == Segments.Num())
                || (Create && Index + 1 >= FirstMissing), Child)) return false;
        FString ChildCanonical;
        if (!FinalPathForHandle(Child.Value, ChildCanonical)
            || !CanonicalWithin(VolumeCanonical, ChildCanonical)) return false;
        Current = std::move(Child);
    }
    if (!FinalPathForHandle(Current.Value, OutCanonical)
        || !CanonicalWithin(VolumeCanonical, OutCanonical)) return false;
    Out = std::move(Current);
    return true;
}

bool OpenPinnedNewFile(const FString& Root, const FString& Path,
    FScopedTerrainHandle& Out, FString& Error)
{
    FScopedTerrainHandle RootHandle, ParentHandle;
    FString RootCanonical, ParentCanonical;
    if (!OpenPinnedAbsoluteDirectory(Root, false, RootHandle, RootCanonical)
        || !OpenPinnedAbsoluteDirectory(FPaths::GetPath(Path), false,
            ParentHandle, ParentCanonical, false, true)
        || !CanonicalWithin(RootCanonical, ParentCanonical)
        || !OpenRelativeHandle(ParentHandle.Value, FPaths::GetCleanFilename(Path),
            false, true, false, false, false, Out))
    {
        Error = TEXT("Unable to securely create a TerrainCore file.");
        return false;
    }
    FString FileCanonical;
    if (!FinalPathForHandle(Out.Value, FileCanonical)
        || !CanonicalWithin(RootCanonical, FileCanonical))
    {
        Out.Reset();
        Error = TEXT("TerrainCore file escaped its pinned storage root.");
        return false;
    }
    return true;
}

bool WriteAll(const HANDLE Handle, const uint8* Data, uint64 Bytes)
{
    while (Bytes > 0)
    {
        const DWORD Chunk = static_cast<DWORD>(FMath::Min<uint64>(Bytes, MAX_uint32));
        DWORD Written = 0;
        if (!WriteFile(Handle, Data, Chunk, &Written, nullptr) || Written != Chunk) return false;
        Data += Chunk;
        Bytes -= Chunk;
    }
    return true;
}

bool SecureWriteUtf8(const FString& Root, const FString& Path, const FString& Value,
    FString& Error)
{
    FScopedTerrainHandle Handle;
    if (!OpenPinnedNewFile(Root, Path, Handle, Error)) return false;
    const FTCHARToUTF8 Utf8(*Value);
    if (!WriteAll(Handle.Value, reinterpret_cast<const uint8*>(Utf8.Get()),
        static_cast<uint64>(Utf8.Length())) || !FlushFileBuffers(Handle.Value))
    {
        Error = TEXT("Unable to securely write TerrainCore UTF-8 content.");
        return false;
    }
    return true;
}

bool MarkHandleForDeletion(const HANDLE Handle)
{
    FILE_DISPOSITION_INFO Disposition{1};
    return SetFileInformationByHandle(Handle, FileDispositionInfo,
        &Disposition, sizeof(Disposition)) != 0;
}

bool DeleteHandleTree(const HANDLE Directory, const FString& RootCanonical, int32 Depth)
{
    if (Depth > 64) return false;
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(64 * 1024);
    bool Restart = true;
    for (;;)
    {
        const FILE_INFO_BY_HANDLE_CLASS InfoClass = Restart
            ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
        Restart = false;
        if (!GetFileInformationByHandleEx(Directory, InfoClass,
            Buffer.GetData(), static_cast<DWORD>(Buffer.Num())))
        {
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
        uint8* Cursor = Buffer.GetData();
        for (;;)
        {
            const FILE_ID_BOTH_DIR_INFO* Entry =
                reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(Cursor);
            const FString Name(static_cast<int32>(Entry->FileNameLength / sizeof(WCHAR)),
                Entry->FileName);
            if (Name != TEXT(".") && Name != TEXT(".."))
            {
                const bool IsDirectory = (Entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                FScopedTerrainHandle Child;
                if (!OpenRelativeHandle(Directory, Name, IsDirectory, false, true,
                    true, false, Child))
                    return false;
                FString ChildCanonical;
                if (!FinalPathForHandle(Child.Value, ChildCanonical)
                    || !CanonicalWithin(RootCanonical, ChildCanonical)) return false;
                DWORD Attributes = 0;
                if (!HandleAttributes(Child.Value, Attributes)) return false;
                const bool Reparse = (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                if (IsDirectory && !Reparse
                    && !DeleteHandleTree(Child.Value, RootCanonical, Depth + 1))
                    return false;
                if (!MarkHandleForDeletion(Child.Value)) return false;
            }
            if (Entry->NextEntryOffset == 0) break;
            Cursor += Entry->NextEntryOffset;
        }
    }
}

bool SecureDeleteDirectoryTree(const FString& Root, const FString& Directory)
{
    FScopedTerrainHandle RootHandle, DirectoryHandle;
    FString RootCanonical, DirectoryCanonical;
    if (!OpenPinnedAbsoluteDirectory(Root, false, RootHandle, RootCanonical)) return false;
    if (!OpenPinnedAbsoluteDirectory(Directory, false, DirectoryHandle, DirectoryCanonical, true))
        return !IFileManager::Get().DirectoryExists(*Directory);
    if (!CanonicalWithin(RootCanonical, DirectoryCanonical)
        || RootCanonical.Equals(DirectoryCanonical, ESearchCase::IgnoreCase)
        || !DeleteHandleTree(DirectoryHandle.Value, RootCanonical, 0)
        || !MarkHandleForDeletion(DirectoryHandle.Value)) return false;
    return true;
}

bool SecureMoveDirectory(const FString& Root, const FString& Source,
    const FString& DestinationParent, const FString& DestinationName, FString& Error)
{
    FScopedTerrainHandle RootHandle, SourceHandle, ParentHandle;
    FString RootCanonical, SourceCanonical, ParentCanonical;
    if (!OpenPinnedAbsoluteDirectory(Root, false, RootHandle, RootCanonical))
    {
        Error = FString::Printf(TEXT("Unable to pin TerrainCore activation root (%lu)."),
            GetLastError());
        return false;
    }
    if (!OpenPinnedAbsoluteDirectory(Source, false, SourceHandle, SourceCanonical, true))
    {
        Error = FString::Printf(TEXT("Unable to pin TerrainCore activation source (%lu)."),
            GetLastError());
        return false;
    }
    if (!OpenPinnedAbsoluteDirectory(DestinationParent, false, ParentHandle,
            ParentCanonical, false, true))
    {
        Error = FString::Printf(TEXT("Unable to pin TerrainCore activation parent (%lu)."),
            GetLastError());
        return false;
    }
    if (!CanonicalWithin(RootCanonical, SourceCanonical)
        || !CanonicalWithin(RootCanonical, ParentCanonical))
    {
        Error = TEXT("TerrainCore activation directories escaped the pinned root.");
        return false;
    }
    const uint32 NameBytes = DestinationName.Len() * sizeof(TCHAR);
    FNtSetInformationFile NtSetInformation = TerrainNtSetInformationFile();
    if (!NtSetInformation)
    {
        Error = TEXT("Native pinned TerrainCore activation is unavailable.");
        return false;
    }
    TArray<uint8> Storage;
    Storage.SetNumZeroed(offsetof(FTerrainFileRenameInformation, FileName) + NameBytes);
    FTerrainFileRenameInformation* Rename =
        reinterpret_cast<FTerrainFileRenameInformation*>(Storage.GetData());
    Rename->ReplaceIfExists = 0;
    Rename->RootDirectory = ParentHandle.Value;
    Rename->FileNameLength = NameBytes;
    FMemory::Memcpy(Rename->FileName, *DestinationName, NameBytes);
    IO_STATUS_BLOCK Io{};
    const NTSTATUS RenameStatus = NtSetInformation(SourceHandle.Value, &Io, Rename,
        static_cast<ULONG>(Storage.Num()), static_cast<FILE_INFORMATION_CLASS>(10));
    if (RenameStatus < 0)
    {
        Error = FString::Printf(
            TEXT("Unable to atomically activate pinned TerrainCore directory (NTSTATUS 0x%08x)."),
            static_cast<uint32>(RenameStatus));
        return false;
    }
    FString ActivatedCanonical;
    const FString Expected = NormalizeFinalPath(FPaths::Combine(ParentCanonical, DestinationName));
    if (!FinalPathForHandle(SourceHandle.Value, ActivatedCanonical)
        || !ActivatedCanonical.Equals(Expected, ESearchCase::IgnoreCase)
        || !CanonicalWithin(RootCanonical, ActivatedCanonical))
    {
        Error = TEXT("Activated TerrainCore directory escaped its pinned destination.");
        return false;
    }
    return true;
}

bool OpenPinnedFileBeneathRoot(const FString& Root, const FString& Directory,
    const FString& RelativePath, FScopedTerrainHandle& Out, FString& Error)
{
    Out.Reset();
    const FTCHARToUTF8 RelativeUtf8(*RelativePath);
    if (!SkiDomain::IsSafeTerrainCorePath(
            std::string(RelativeUtf8.Get(), RelativeUtf8.Length()))
        || !IsWithinDirectory(Root, Directory))
    {
        Error = TEXT("TerrainCore metadata path is unsafe.");
        return false;
    }

    FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
    FString FullDirectory = FPaths::ConvertRelativePathToFull(Directory);
    FPaths::NormalizeDirectoryName(FullRoot);
    FPaths::NormalizeDirectoryName(FullDirectory);

    FScopedTerrainHandle Current;
    FString RootCanonical;
    if (!OpenPinnedAbsoluteDirectory(FullRoot, false, Current, RootCanonical))
    {
        Error = TEXT("Unable to pin the TerrainCore metadata root.");
        return false;
    }

    FString RelativeDirectory;
    if (!FullDirectory.Equals(FullRoot, ESearchCase::IgnoreCase))
        RelativeDirectory = FullDirectory.Mid(FullRoot.Len() + 1);
    TArray<FString> Segments;
    RelativeDirectory.ParseIntoArray(Segments, TEXT("/"), true);
    FString DirectoryCanonical = RootCanonical;
    for (const FString& Segment : Segments)
    {
        FScopedTerrainHandle Child;
        if (!OpenRelativeHandle(Current.Value, Segment, true, false, false,
                false, false, Child)
            || !FinalPathForHandle(Child.Value, DirectoryCanonical)
            || !CanonicalWithin(RootCanonical, DirectoryCanonical))
        {
            Error = TEXT("Unable to resolve the TerrainCore metadata directory securely.");
            return false;
        }
        Current = std::move(Child);
    }

    TArray<FString> RelativeSegments;
    RelativePath.ParseIntoArray(RelativeSegments, TEXT("/"), true);
    if (RelativeSegments.IsEmpty())
    {
        Error = TEXT("TerrainCore file path is empty.");
        return false;
    }
    for (int32 Index = 0; Index < RelativeSegments.Num(); ++Index)
    {
        const bool IsLeaf = Index + 1 == RelativeSegments.Num();
        FScopedTerrainHandle Child;
        if (!OpenRelativeHandle(Current.Value, RelativeSegments[Index], !IsLeaf,
                false, false, false, false, Child, IsLeaf, FILE_SHARE_READ))
        {
            Error = TEXT("Unable to open TerrainCore file securely.");
            return false;
        }
        if (IsLeaf) Out = std::move(Child);
        else Current = std::move(Child);
    }
    FString FileCanonical;
    if (!FinalPathForHandle(Out.Value, FileCanonical)
        || !CanonicalWithin(DirectoryCanonical, FileCanonical))
    {
        Out.Reset();
        Error = TEXT("TerrainCore metadata escaped its pinned package directory.");
        return false;
    }
    return true;
}
#endif

#if !PLATFORM_WINDOWS
bool SecureWriteUtf8(const FString& Root, const FString& Path, const FString& Value,
    FString& Error)
{
    if (!ValidateNoReparsePath(Root, true, &Error)
        || !ValidateNoReparsePath(Path, false, &Error)
        || !FFileHelper::SaveStringToFile(Value, *Path,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        || !ValidateNoReparsePath(Path, true, &Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to securely write TerrainCore UTF-8 content.");
        return false;
    }
    return true;
}
#endif

class FSecureTerrainWriter
{
public:
    bool Open(const FString& Root, const FString& Path, FString& Error)
    {
        Close();
#if PLATFORM_WINDOWS
        return OpenPinnedNewFile(Root, Path, Handle, Error);
#else
        IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
        Fallback.Reset(Platform.OpenWrite(*Path, false, false));
        if (!Fallback) Error = TEXT("Unable to open secure TerrainCore output.");
        return Fallback.IsValid();
#endif
    }
    bool Write(const uint8* Data, const int64 Bytes)
    {
#if PLATFORM_WINDOWS
        return Handle && WriteAll(Handle.Value, Data, static_cast<uint64>(Bytes));
#else
        return Fallback && Fallback->Write(Data, Bytes);
#endif
    }
    void Close()
    {
#if PLATFORM_WINDOWS
        Handle.Reset();
#else
        Fallback.Reset();
#endif
    }
    explicit operator bool() const
    {
#if PLATFORM_WINDOWS
        return static_cast<bool>(Handle);
#else
        return Fallback.IsValid();
#endif
    }
private:
#if PLATFORM_WINDOWS
    FScopedTerrainHandle Handle;
#else
    TUniquePtr<IFileHandle> Fallback;
#endif
};

bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, std::string& Out)
{
    FString Value;
    if (!Object || !Object->TryGetStringField(Name, Value)) return false;
    const FTCHARToUTF8 Utf8(*Value);
    Out.assign(Utf8.Get(), Utf8.Length());
    return true;
}

bool ReadUint(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, uint64 Maximum,
    uint64& Out)
{
    double Value = 0.0;
    if (!Object || !Object->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value)
        || Value < 0.0 || Value > static_cast<double>(Maximum)
        || FMath::FloorToDouble(Value) != Value)
    {
        return false;
    }
    Out = static_cast<uint64>(Value);
    return true;
}

TSharedRef<FJsonObject> BoundsObject(const SkiDomain::MetricBounds& Bounds)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("westM"), Bounds.WestM);
    Object->SetNumberField(TEXT("southM"), Bounds.SouthM);
    Object->SetNumberField(TEXT("eastM"), Bounds.EastM);
    Object->SetNumberField(TEXT("northM"), Bounds.NorthM);
    return Object;
}

TSharedRef<FJsonObject> SourceObject(const SkiDomain::TerrainCoreSource& Source)
{
    TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("sourceId"), UTF8_TO_TCHAR(Source.SourceId.c_str()));
    Object->SetStringField(TEXT("product"), UTF8_TO_TCHAR(Source.Product.c_str()));
    Object->SetStringField(TEXT("acquisitionEpoch"), UTF8_TO_TCHAR(Source.AcquisitionEpoch.c_str()));
    Object->SetStringField(TEXT("horizontalCrs"), UTF8_TO_TCHAR(Source.HorizontalCrs.c_str()));
    Object->SetStringField(TEXT("horizontalDatum"), UTF8_TO_TCHAR(Source.HorizontalDatum.c_str()));
    Object->SetStringField(TEXT("verticalDatum"), UTF8_TO_TCHAR(Source.VerticalDatum.c_str()));
    Object->SetStringField(TEXT("license"), UTF8_TO_TCHAR(Source.License.c_str()));
    Object->SetStringField(TEXT("attribution"), UTF8_TO_TCHAR(Source.Attribution.c_str()));
    Object->SetNumberField(TEXT("nativeEastSpacingM"), Source.NativeEastSpacingM);
    Object->SetNumberField(TEXT("nativeNorthSpacingM"), Source.NativeNorthSpacingM);
    if (!Source.NativeSpacingReported)
        Object->SetBoolField(TEXT("nativeSpacingReported"), false);
    Object->SetNumberField(TEXT("horizontalAccuracyM"), Source.HorizontalAccuracyM);
    Object->SetNumberField(TEXT("verticalAccuracyM"), Source.VerticalAccuracyM);
    Object->SetBoolField(TEXT("hasHorizontalAccuracy"), Source.HasHorizontalAccuracy);
    Object->SetBoolField(TEXT("hasVerticalAccuracy"), Source.HasVerticalAccuracy);
    return Object;
}

bool ReadSource(const TSharedPtr<FJsonObject>& Object, SkiDomain::TerrainCoreSource& Source)
{
    const bool Required = ReadString(Object, TEXT("sourceId"), Source.SourceId)
        && ReadString(Object, TEXT("product"), Source.Product)
        && ReadString(Object, TEXT("acquisitionEpoch"), Source.AcquisitionEpoch)
        && ReadString(Object, TEXT("horizontalCrs"), Source.HorizontalCrs)
        && ReadString(Object, TEXT("horizontalDatum"), Source.HorizontalDatum)
        && ReadString(Object, TEXT("verticalDatum"), Source.VerticalDatum)
        && ReadString(Object, TEXT("license"), Source.License)
        && ReadString(Object, TEXT("attribution"), Source.Attribution)
        && Object->TryGetNumberField(TEXT("nativeEastSpacingM"), Source.NativeEastSpacingM)
        && Object->TryGetNumberField(TEXT("nativeNorthSpacingM"), Source.NativeNorthSpacingM)
        && Object->TryGetNumberField(TEXT("horizontalAccuracyM"), Source.HorizontalAccuracyM)
        && Object->TryGetNumberField(TEXT("verticalAccuracyM"), Source.VerticalAccuracyM)
        && Object->TryGetBoolField(TEXT("hasHorizontalAccuracy"), Source.HasHorizontalAccuracy)
        && Object->TryGetBoolField(TEXT("hasVerticalAccuracy"), Source.HasVerticalAccuracy);
    if (!Required) return false;
    bool Reported = true;
    if (Object->HasField(TEXT("nativeSpacingReported"))
        && !Object->TryGetBoolField(TEXT("nativeSpacingReported"), Reported)) return false;
    Source.NativeSpacingReported = Reported;
    return true;
}

bool ReadBounds(const TSharedPtr<FJsonObject>& Object, SkiDomain::MetricBounds& Out)
{
    return Object && Object->TryGetNumberField(TEXT("westM"), Out.WestM)
        && Object->TryGetNumberField(TEXT("southM"), Out.SouthM)
        && Object->TryGetNumberField(TEXT("eastM"), Out.EastM)
        && Object->TryGetNumberField(TEXT("northM"), Out.NorthM);
}

FString WriteJson(const TSharedRef<FJsonObject>& Root)
{
    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output, 0);
    FJsonSerializer::Serialize(Root, Writer);
    return Output;
}

bool IsCanonicalId(const FString& Value)
{
    if (Value.Len() != 64) return false;
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('0') && Character <= TEXT('9'))
            || (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
    }
    return true;
}

FString HashUtf8(const FString& Value)
{
    const FTCHARToUTF8 Utf8(*Value);
    return SkiPreparation::Sha256(MakeArrayView(
        reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

bool HashFile(const FString& Path, uint64 ExpectedBytes, FString& OutHash, FString& Error)
{
    IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
    const int64 Size = Files.FileSize(*Path);
    if (Size < 0 || static_cast<uint64>(Size) != ExpectedBytes)
    {
        Error = TEXT("TerrainCore shard length does not match the manifest.");
        return false;
    }
    TUniquePtr<IFileHandle> Handle(Files.OpenRead(*Path));
    if (!Handle)
    {
        Error = TEXT("TerrainCore shard could not be opened.");
        return false;
    }
    SHA256_CTX Context;
    if (SHA256_Init(&Context) != 1)
    {
        Error = TEXT("TerrainCore shard hash initialization failed.");
        return false;
    }
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(1024 * 1024);
    uint64 Remaining = ExpectedBytes;
    while (Remaining > 0)
    {
        const int64 Count = static_cast<int64>(FMath::Min<uint64>(Remaining, Buffer.Num()));
        if (!Handle->Read(Buffer.GetData(), Count)
            || SHA256_Update(&Context, Buffer.GetData(), static_cast<size_t>(Count)) != 1)
        {
            Error = TEXT("TerrainCore shard could not be read for hashing.");
            return false;
        }
        Remaining -= static_cast<uint64>(Count);
    }
    uint8 Digest[SHA256_DIGEST_LENGTH];
    if (SHA256_Final(Digest, &Context) != 1)
    {
        Error = TEXT("TerrainCore shard hash finalization failed.");
        return false;
    }
    OutHash.Reset();
    OutHash.Reserve(64);
    for (const uint8 Byte : Digest) OutHash += FString::Printf(TEXT("%02x"), Byte);
    return true;
}

bool IsWithinDirectory(const FString& Directory, const FString& Candidate)
{
    FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(Directory);
    FString NormalizedCandidate = FPaths::ConvertRelativePathToFull(Candidate);
    FPaths::NormalizeDirectoryName(NormalizedDirectory);
    FPaths::NormalizeFilename(NormalizedCandidate);
    return NormalizedCandidate.Equals(NormalizedDirectory, ESearchCase::IgnoreCase)
        || NormalizedCandidate.StartsWith(NormalizedDirectory + TEXT("/"),
            ESearchCase::IgnoreCase);
}

bool ValidateNoReparsePath(const FString& Path, const bool RequireLeaf,
    FString* Error = nullptr)
{
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full);
    TArray<FString> Ancestors;
    FString Cursor = Full;
    while (!Cursor.IsEmpty())
    {
        Ancestors.Add(Cursor);
        const FString Parent = FPaths::GetPath(Cursor);
        if (Parent.IsEmpty() || Parent.Equals(Cursor, ESearchCase::IgnoreCase)) break;
        Cursor = Parent;
    }
    Algo::Reverse(Ancestors);
    IFileManager& Files = IFileManager::Get();
    for (int32 Index = 0; Index < Ancestors.Num(); ++Index)
    {
        const FString& Candidate = Ancestors[Index];
        const bool Exists = Files.FileExists(*Candidate) || Files.DirectoryExists(*Candidate);
        if (!Exists) continue;
        if (Files.IsSymlink(*Candidate))
        {
            if (Error) *Error = TEXT("TerrainCore storage path contains a reparse point.");
            return false;
        }
        if (Index + 1 < Ancestors.Num() && !Files.DirectoryExists(*Candidate))
        {
            if (Error) *Error = TEXT("TerrainCore storage ancestor is not a directory.");
            return false;
        }
    }
    if (RequireLeaf && !Files.FileExists(*Full) && !Files.DirectoryExists(*Full))
    {
        if (Error) *Error = TEXT("TerrainCore storage path is missing.");
        return false;
    }
    return true;
}

bool IsWellFormedUtf8(const TArray<uint8>& Bytes)
{
    const auto IsContinuation = [](const uint8 Byte)
    {
        return Byte >= 0x80U && Byte <= 0xbfU;
    };
    int32 Index = 0;
    while (Index < Bytes.Num())
    {
        const uint8 First = Bytes[Index++];
        if (First <= 0x7fU) continue;
        if (First >= 0xc2U && First <= 0xdfU)
        {
            if (Index >= Bytes.Num() || !IsContinuation(Bytes[Index++])) return false;
            continue;
        }
        if (First >= 0xe0U && First <= 0xefU)
        {
            if (Index + 1 >= Bytes.Num()) return false;
            const uint8 Second = Bytes[Index++];
            const uint8 Third = Bytes[Index++];
            if (!IsContinuation(Third)
                || (First == 0xe0U && (Second < 0xa0U || Second > 0xbfU))
                || (First == 0xedU && (Second < 0x80U || Second > 0x9fU))
                || (First != 0xe0U && First != 0xedU && !IsContinuation(Second)))
            {
                return false;
            }
            continue;
        }
        if (First >= 0xf0U && First <= 0xf4U)
        {
            if (Index + 2 >= Bytes.Num()) return false;
            const uint8 Second = Bytes[Index++];
            const uint8 Third = Bytes[Index++];
            const uint8 Fourth = Bytes[Index++];
            if (!IsContinuation(Third) || !IsContinuation(Fourth)
                || (First == 0xf0U && (Second < 0x90U || Second > 0xbfU))
                || (First == 0xf4U && (Second < 0x80U || Second > 0x8fU))
                || (First != 0xf0U && First != 0xf4U && !IsContinuation(Second)))
            {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool DecodeBoundedUtf8(const TArray<uint8>& Bytes, FString& Out, FString& Error)
{
    Out.Reset();
    if (Bytes.IsEmpty() || !IsWellFormedUtf8(Bytes))
    {
        Error = TEXT("TerrainCore metadata is empty or is not valid UTF-8.");
        return false;
    }
    const FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),
        Bytes.Num());
    Out = FString(Converted.Length(), Converted.Get());
    return true;
}

bool SecureReadUtf8(const FString& Root, const FString& Directory,
    const FString& LeafName, const uint64 MaximumBytes, FString& Out,
    uint64& OutBytes, FString& Error)
{
    Out.Reset();
    OutBytes = 0;
    if (MaximumBytes == 0 || MaximumBytes > static_cast<uint64>(MAX_int32)
        || !IsWithinDirectory(Root, Directory))
    {
        Error = TEXT("TerrainCore metadata path or size limit is invalid.");
        return false;
    }

    int64 Size = -1;
    TArray<uint8> Bytes;
#if PLATFORM_WINDOWS
    FScopedTerrainHandle Handle;
    if (!OpenPinnedFileBeneathRoot(Root, Directory, LeafName, Handle, Error)) return false;
    LARGE_INTEGER NativeSize{};
    if (!GetFileSizeEx(Handle.Value, &NativeSize))
    {
        Error = TEXT("Unable to size TerrainCore metadata securely.");
        return false;
    }
    Size = NativeSize.QuadPart;
    if (Size <= 0 || static_cast<uint64>(Size) > MaximumBytes)
    {
        Error = TEXT("TerrainCore metadata is missing or oversized.");
        return false;
    }
    Bytes.SetNumUninitialized(static_cast<int32>(Size));
    uint8* Cursor = Bytes.GetData();
    uint64 Remaining = static_cast<uint64>(Size);
    while (Remaining > 0)
    {
        const DWORD Requested = static_cast<DWORD>(FMath::Min<uint64>(Remaining, MAX_uint32));
        DWORD Read = 0;
        if (!ReadFile(Handle.Value, Cursor, Requested, &Read, nullptr)
            || Read != Requested)
        {
            Error = TEXT("Unable to read TerrainCore metadata completely.");
            return false;
        }
        Cursor += Read;
        Remaining -= Read;
    }
    LARGE_INTEGER FinalSize{};
    if (!GetFileSizeEx(Handle.Value, &FinalSize) || FinalSize.QuadPart != Size)
    {
        Error = TEXT("TerrainCore metadata changed while it was being read.");
        return false;
    }
#else
    const FString Path = FPaths::Combine(Directory, LeafName);
    if (!ValidateNoReparsePath(Root, true, &Error)
        || !ValidateNoReparsePath(Directory, true, &Error)
        || !ValidateNoReparsePath(Path, true, &Error)) return false;
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(Platform.OpenRead(*Path));
    if (!Handle)
    {
        Error = TEXT("Unable to open TerrainCore metadata.");
        return false;
    }
    Size = Handle->Size();
    if (Size <= 0 || static_cast<uint64>(Size) > MaximumBytes)
    {
        Error = TEXT("TerrainCore metadata is missing or oversized.");
        return false;
    }
    Bytes.SetNumUninitialized(static_cast<int32>(Size));
    if (!Handle->Read(Bytes.GetData(), Size)
        || Handle->Size() != Size
        || !ValidateNoReparsePath(Path, true, &Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to read TerrainCore metadata completely.");
        return false;
    }
#endif
    if (!DecodeBoundedUtf8(Bytes, Out, Error)) return false;
    OutBytes = static_cast<uint64>(Size);
    return true;
}

bool EnsureSecureDirectory(const FString& Directory, FString& Error)
{
#if PLATFORM_WINDOWS
    FScopedTerrainHandle Handle;
    FString Canonical;
    if (!OpenPinnedAbsoluteDirectory(Directory, true, Handle, Canonical))
    {
        Error = TEXT("Unable to create or pin a secure TerrainCore directory.");
        return false;
    }
    return true;
#else
    if (!ValidateNoReparsePath(Directory, false, &Error)
        || !IFileManager::Get().MakeDirectory(*Directory, true)
        || !ValidateNoReparsePath(Directory, true, &Error))
    {
        if (Error.IsEmpty()) Error = TEXT("Unable to create a secure TerrainCore directory.");
        return false;
    }
    return true;
#endif
}

bool ValidateTreeNoReparse(const FString& Directory)
{
    if (!ValidateNoReparsePath(Directory, true)) return false;
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
                if (IsDirectory && !Visit(Entry))
                {
                    Valid = false;
                    return false;
                }
                return true;
            }) && Valid;
    };
    return Visit(Directory) && ValidateNoReparsePath(Directory, true);
}

void DeleteSecureTree(const FString& Parent, const FString& Directory)
{
#if PLATFORM_WINDOWS
    if (IsWithinDirectory(Parent, Directory) && !FPaths::IsSamePath(Parent, Directory))
        SecureDeleteDirectoryTree(Parent, Directory);
#else
    IFileManager& Files = IFileManager::Get();
    if (IsWithinDirectory(Parent, Directory)
        && !FPaths::IsSamePath(Parent, Directory)
        && ValidateNoReparsePath(Parent, true)
        && (!Files.DirectoryExists(*Directory) || ValidateTreeNoReparse(Directory)))
    {
        Files.DeleteDirectory(*Directory, false, true);
    }
#endif
}

bool SafeChildPath(const FString& Directory, const std::string& Relative, FString& Out)
{
    if (!SkiDomain::IsSafeTerrainCorePath(Relative)
        || !ValidateNoReparsePath(Directory, true)) return false;
    TArray<FString> Segments;
    FString(UTF8_TO_TCHAR(Relative.c_str())).ParseIntoArray(Segments, TEXT("/"), true);
    for (const FString& Segment : Segments)
    {
        if (Segment.IsEmpty() || Segment.EndsWith(TEXT(".")) || Segment.EndsWith(TEXT(" "))
            || Segment.Contains(TEXT("<")) || Segment.Contains(TEXT(">"))
            || Segment.Contains(TEXT("\"")) || Segment.Contains(TEXT("|"))
            || Segment.Contains(TEXT("?")) || Segment.Contains(TEXT("*"))) return false;
        for (const TCHAR Character : Segment) if (Character < 32) return false;
        FString Device = Segment;
        int32 Dot = INDEX_NONE;
        if (Device.FindChar(TEXT('.'), Dot)) Device.LeftInline(Dot);
        Device.ToUpperInline();
        const bool Reserved = Device == TEXT("CON") || Device == TEXT("PRN")
            || Device == TEXT("AUX") || Device == TEXT("NUL")
            || (Device.Len() == 4 && (Device.StartsWith(TEXT("COM"))
                || Device.StartsWith(TEXT("LPT"))) && Device[3] >= TEXT('1') && Device[3] <= TEXT('9'));
        if (Reserved) return false;
    }
    FString Root = FPaths::ConvertRelativePathToFull(Directory);
    FPaths::NormalizeDirectoryName(Root);
    Out = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root,
        UTF8_TO_TCHAR(Relative.c_str())));
    FPaths::NormalizeFilename(Out);
    const FString Prefix = Root + TEXT("/");
    if (!Out.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
    return ValidateNoReparsePath(Out, false);
}

bool ValidateDeclaredFiles(const FString& Directory, const TArray<FString>& Declared,
    FString& Error)
{
    if (!ValidateNoReparsePath(Directory, true, &Error)) return false;
    TSet<FString> DeclaredFiles;
    TSet<FString> DeclaredDirectories;
    for (FString Relative : Declared)
    {
        Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
        Relative.ToLowerInline();
        if (Relative.IsEmpty() || DeclaredFiles.Contains(Relative))
        {
            Error = TEXT("TerrainCore package has duplicate declared files.");
            return false;
        }
        DeclaredFiles.Add(Relative);
        FString Parent = FPaths::GetPath(Relative);
        while (!Parent.IsEmpty())
        {
            DeclaredDirectories.Add(Parent);
            Parent = FPaths::GetPath(Parent);
        }
    }
    FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Directory);
    FPaths::NormalizeDirectoryName(NormalizedRoot);
    TSet<FString> Seen;
    IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
    bool Valid = true;
    TFunction<bool(const FString&)> Visit = [&](const FString& Current)
    {
        return Platform.IterateDirectory(*Current,
            [&](const TCHAR* Name, const bool IsDirectory)
            {
                const FString Full(Name);
                if (!IsWithinDirectory(NormalizedRoot, Full)
                    || IFileManager::Get().IsSymlink(*Full))
                {
                    Error = TEXT("TerrainCore package contains an unsafe filesystem entry.");
                    Valid = false;
                    return false;
                }
                FString Relative = Full.Mid(NormalizedRoot.Len());
                Relative.RemoveFromStart(TEXT("/"));
                Relative.RemoveFromStart(TEXT("\\"));
                Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
                Relative.ToLowerInline();
                if (IsDirectory)
                {
                    if (!DeclaredDirectories.Contains(Relative) || !Visit(Full))
                    {
                        if (Error.IsEmpty())
                            Error = TEXT("TerrainCore package contains an undeclared directory.");
                        Valid = false;
                        return false;
                    }
                    return true;
                }
                if (!DeclaredFiles.Contains(Relative) || Seen.Contains(Relative))
                {
                    Error = TEXT("TerrainCore package contains an undeclared or duplicate file.");
                    Valid = false;
                    return false;
                }
                Seen.Add(Relative);
                return true;
            }) && Valid;
    };
    if (!Visit(NormalizedRoot) || Seen.Num() != DeclaredFiles.Num()
        || !ValidateNoReparsePath(Directory, true, &Error))
    {
        if (Error.IsEmpty()) Error = TEXT("TerrainCore package file inventory is incomplete.");
        return false;
    }
    return true;
}

bool EncodedLengthsAreBounded(const SkiDomain::TerrainCoreTileDescriptor& Descriptor)
{
    return Descriptor.HeightRawBytes > 0 && Descriptor.ValidityRawBytes > 0
        && Descriptor.HeightBytes > 0 && Descriptor.ValidityBytes > 0
        && Descriptor.HeightBytes <= SkiDomain::TerrainCoreDeflateBound(Descriptor.HeightRawBytes)
        && Descriptor.ValidityBytes <= SkiDomain::TerrainCoreDeflateBound(Descriptor.ValidityRawBytes)
        && Descriptor.HeightBytes <= SkiDomain::TerrainCoreMaxEncodedTileBytes
        && Descriptor.ValidityBytes <= SkiDomain::TerrainCoreMaxEncodedTileBytes
            - Descriptor.HeightBytes;
}

const SkiDomain::TerrainCoreTileDescriptor* FindTile(
    const SkiDomain::TerrainCoreManifest& Manifest, const uint8 Lod,
    const uint32 X, const uint32 Y)
{
    const auto It = std::find_if(Manifest.Tiles.begin(), Manifest.Tiles.end(),
        [=](const SkiDomain::TerrainCoreTileDescriptor& Tile)
        {
            return Tile.LodIndex == Lod && Tile.TileX == X && Tile.TileY == Y;
        });
    return It == Manifest.Tiles.end() ? nullptr : &*It;
}

bool ReadRange(const FString& Path, const uint64 Offset, const uint64 Bytes,
    TArray<uint8>& Out, FString& Error)
{
    Out.Reset();
    if (Bytes == 0 || Bytes > MAX_int32 || Offset > MAX_int64)
    {
        Error = TEXT("TerrainCore tile range exceeds runtime limits.");
        return false;
    }
    IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
    TUniquePtr<IFileHandle> Handle(Files.OpenRead(*Path));
    if (!Handle || !Handle->Seek(static_cast<int64>(Offset)))
    {
        Error = TEXT("TerrainCore shard range could not be opened.");
        return false;
    }
    Out.SetNumUninitialized(static_cast<int32>(Bytes));
    if (!Handle->Read(Out.GetData(), static_cast<int64>(Bytes)))
    {
        Out.Reset();
        Error = TEXT("TerrainCore shard range is truncated.");
        return false;
    }
    return true;
}

bool SecureShardLength(const FString& Root, const FString& Directory,
    const std::string& Relative, const uint64 ExpectedBytes, FString& Error)
{
#if PLATFORM_WINDOWS
    FScopedTerrainHandle Handle;
    if (!OpenPinnedFileBeneathRoot(Root, Directory,
            UTF8_TO_TCHAR(Relative.c_str()), Handle, Error)) return false;
    LARGE_INTEGER Size{};
    if (!GetFileSizeEx(Handle.Value, &Size) || Size.QuadPart < 0
        || static_cast<uint64>(Size.QuadPart) != ExpectedBytes)
    {
        Error = TEXT("TerrainCore shard length does not match the manifest.");
        return false;
    }
    return true;
#else
    FString Path;
    if (!SafeChildPath(Directory, Relative, Path)
        || FPlatformFileManager::Get().GetPlatformFile().FileSize(*Path)
            != static_cast<int64>(ExpectedBytes))
    {
        Error = TEXT("TerrainCore shard path or length is invalid.");
        return false;
    }
    return true;
#endif
}

bool SecureHashShard(const FString& Root, const FString& Directory,
    const SkiDomain::TerrainCoreShardDescriptor& Shard, FString& OutHash, FString& Error)
{
#if PLATFORM_WINDOWS
    FScopedTerrainHandle Handle;
    if (!OpenPinnedFileBeneathRoot(Root, Directory,
            UTF8_TO_TCHAR(Shard.Path.c_str()), Handle, Error)) return false;
    LARGE_INTEGER Size{};
    if (!GetFileSizeEx(Handle.Value, &Size) || Size.QuadPart < 0
        || static_cast<uint64>(Size.QuadPart) != Shard.Bytes)
    {
        Error = TEXT("TerrainCore shard length does not match the manifest.");
        return false;
    }
    SHA256_CTX Context;
    if (SHA256_Init(&Context) != 1)
    {
        Error = TEXT("TerrainCore shard hash initialization failed.");
        return false;
    }
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(1024 * 1024);
    uint64 Remaining = Shard.Bytes;
    while (Remaining > 0)
    {
        const DWORD Requested = static_cast<DWORD>(
            FMath::Min<uint64>(Remaining, static_cast<uint64>(Buffer.Num())));
        DWORD Read = 0;
        if (!ReadFile(Handle.Value, Buffer.GetData(), Requested, &Read, nullptr)
            || Read != Requested
            || SHA256_Update(&Context, Buffer.GetData(), static_cast<size_t>(Read)) != 1)
        {
            Error = TEXT("TerrainCore shard could not be read for hashing.");
            return false;
        }
        Remaining -= Read;
    }
    LARGE_INTEGER FinalSize{};
    if (!GetFileSizeEx(Handle.Value, &FinalSize) || FinalSize.QuadPart != Size.QuadPart)
    {
        Error = TEXT("TerrainCore shard changed while it was being hashed.");
        return false;
    }
    uint8 Digest[SHA256_DIGEST_LENGTH];
    if (SHA256_Final(Digest, &Context) != 1)
    {
        Error = TEXT("TerrainCore shard hash finalization failed.");
        return false;
    }
    OutHash.Reset();
    OutHash.Reserve(64);
    for (const uint8 Byte : Digest) OutHash += FString::Printf(TEXT("%02x"), Byte);
    return true;
#else
    FString Path;
    return SafeChildPath(Directory, Shard.Path, Path)
        && HashFile(Path, Shard.Bytes, OutHash, Error);
#endif
}

bool SecureReadShardRange(const FString& Root, const FString& Directory,
    const std::string& Relative, const uint64 Offset, const uint64 Bytes,
    TArray<uint8>& Out, FString& Error)
{
#if PLATFORM_WINDOWS
    Out.Reset();
    if (Bytes == 0 || Bytes > MAX_int32 || Offset > MAX_int64
        || Bytes > MAX_uint64 - Offset)
    {
        Error = TEXT("TerrainCore tile range exceeds runtime limits.");
        return false;
    }
    FScopedTerrainHandle Handle;
    if (!OpenPinnedFileBeneathRoot(Root, Directory,
            UTF8_TO_TCHAR(Relative.c_str()), Handle, Error)) return false;
    LARGE_INTEGER Size{};
    LARGE_INTEGER Position{};
    Position.QuadPart = static_cast<LONGLONG>(Offset);
    if (!GetFileSizeEx(Handle.Value, &Size) || Size.QuadPart < 0
        || Offset + Bytes > static_cast<uint64>(Size.QuadPart)
        || !SetFilePointerEx(Handle.Value, Position, nullptr, FILE_BEGIN))
    {
        Error = TEXT("TerrainCore shard range is outside its pinned file.");
        return false;
    }
    Out.SetNumUninitialized(static_cast<int32>(Bytes));
    DWORD Read = 0;
    if (!ReadFile(Handle.Value, Out.GetData(), static_cast<DWORD>(Bytes), &Read, nullptr)
        || Read != static_cast<DWORD>(Bytes))
    {
        Out.Reset();
        Error = TEXT("TerrainCore shard range is truncated.");
        return false;
    }
    LARGE_INTEGER FinalSize{};
    if (!GetFileSizeEx(Handle.Value, &FinalSize) || FinalSize.QuadPart != Size.QuadPart)
    {
        Out.Reset();
        Error = TEXT("TerrainCore shard changed while its range was read.");
        return false;
    }
    return true;
#else
    FString Path;
    return SafeChildPath(Directory, Relative, Path)
        && ReadRange(Path, Offset, Bytes, Out, Error);
#endif
}

FString SerializeEdits(const SkiDomain::TerrainEditSet& Edits, const FString& EditSetId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    if (!EditSetId.IsEmpty()) Root->SetStringField(TEXT("editSetId"), EditSetId);
    Root->SetNumberField(TEXT("schemaVersion"), Edits.SchemaVersion);
    Root->SetStringField(TEXT("terrainCoreId"), UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()));
    Root->SetStringField(TEXT("baseRevision"), LexToString(Edits.BaseRevision));
    Root->SetStringField(TEXT("editRevision"), LexToString(Edits.EditRevision));
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Reserve(static_cast<int32>(Edits.Deltas.size()));
    for (const SkiDomain::TerrainEditDelta& Delta : Edits.Deltas)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("column"), Delta.Column);
        Object->SetNumberField(TEXT("row"), Delta.Row);
        Object->SetNumberField(TEXT("deltaM"), Delta.DeltaM);
        Values.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("deltas"), Values);
    return WriteJson(Root);
}

bool ParseEdits(const FString& Json, FString& OutId, SkiDomain::TerrainEditSet& Out,
    FString& Error)
{
    if (FTCHARToUTF8(Json).Length() > static_cast<int64>(SkiDomain::TerrainCoreMaxManifestBytes))
    {
        Error = TEXT("Terrain edit manifest exceeds its limit.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root
        || !Root->TryGetStringField(TEXT("editSetId"), OutId))
    {
        Error = TEXT("Terrain edit manifest is invalid JSON.");
        return false;
    }
    uint64 Schema = 0, Column = 0, Row = 0;
    FString TerrainId, Base, Revision;
    if (!ReadUint(Root, TEXT("schemaVersion"), MAX_uint32, Schema)
        || !Root->TryGetStringField(TEXT("terrainCoreId"), TerrainId)
        || !Root->TryGetStringField(TEXT("baseRevision"), Base)
        || !Root->TryGetStringField(TEXT("editRevision"), Revision)
        || !LexTryParseString(Out.BaseRevision, *Base)
        || !LexTryParseString(Out.EditRevision, *Revision))
    {
        Error = TEXT("Terrain edit header is invalid.");
        return false;
    }
    Out.SchemaVersion = static_cast<uint32>(Schema);
    Out.TerrainCoreId = TCHAR_TO_UTF8(*TerrainId);
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Root->TryGetArrayField(TEXT("deltas"), Values) || !Values
        || Values->Num() > static_cast<int32>(SkiDomain::TerrainCoreMaxEdits))
    {
        Error = TEXT("Terrain edit deltas are invalid or excessive.");
        return false;
    }
    Out.Deltas.clear();
    Out.Deltas.reserve(Values->Num());
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        const TSharedPtr<FJsonObject> Object = Value ? Value->AsObject() : nullptr;
        double Delta = 0.0;
        if (!ReadUint(Object, TEXT("column"), MAX_uint32, Column)
            || !ReadUint(Object, TEXT("row"), MAX_uint32, Row)
            || !Object->TryGetNumberField(TEXT("deltaM"), Delta)
            || !FMath::IsFinite(Delta) || Delta < -MAX_flt || Delta > MAX_flt)
        {
            Error = TEXT("Terrain edit delta is invalid.");
            return false;
        }
        Out.Deltas.push_back({static_cast<uint32>(Column), static_cast<uint32>(Row),
            static_cast<float>(Delta)});
    }
    if (SerializeEdits(Out, OutId) != Json)
    {
        Error = TEXT("Terrain edit manifest is not canonical.");
        return false;
    }
    return true;
}
}

FString SkiPreparation::SerializeTerrainCoreManifest(
    const SkiDomain::TerrainCoreManifest& Manifest, const bool IncludeContentId)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
    if (IncludeContentId) Root->SetStringField(TEXT("contentId"), UTF8_TO_TCHAR(Manifest.ContentId.c_str()));
    Root->SetStringField(TEXT("generatorVersion"), UTF8_TO_TCHAR(Manifest.GeneratorVersion.c_str()));
    TArray<TSharedPtr<FJsonValue>> Processing;
    for (const std::string& Value : Manifest.ProcessingVersions)
        Processing.Add(MakeShared<FJsonValueString>(UTF8_TO_TCHAR(Value.c_str())));
    Root->SetArrayField(TEXT("processingVersions"), Processing);
    Root->SetObjectField(TEXT("source"), SourceObject(Manifest.Source));
    TArray<TSharedPtr<FJsonValue>> AdditionalSources;
    for (const SkiDomain::TerrainCoreSource& Source : Manifest.AdditionalSources)
        AdditionalSources.Add(MakeShared<FJsonValueObject>(SourceObject(Source)));
    Root->SetArrayField(TEXT("additionalSources"), AdditionalSources);
    TSharedRef<FJsonObject> LocalOrigin = MakeShared<FJsonObject>();
    LocalOrigin->SetNumberField(TEXT("latitudeDeg"), Manifest.LocalOrigin.LatitudeDeg);
    LocalOrigin->SetNumberField(TEXT("longitudeDeg"), Manifest.LocalOrigin.LongitudeDeg);
    LocalOrigin->SetNumberField(TEXT("heightM"), Manifest.LocalOrigin.HeightM);
    Root->SetObjectField(TEXT("localOrigin"), LocalOrigin);
    Root->SetNumberField(TEXT("width"), Manifest.Width);
    Root->SetNumberField(TEXT("height"), Manifest.Height);
    Root->SetNumberField(TEXT("deliveredEastSpacingM"), Manifest.DeliveredEastSpacingM);
    Root->SetNumberField(TEXT("deliveredNorthSpacingM"), Manifest.DeliveredNorthSpacingM);
    Root->SetStringField(TEXT("pixelRegistration"), TEXT("sample-center"));
    Root->SetObjectField(TEXT("sampleCenterBounds"), BoundsObject(Manifest.SampleCenterBounds));
    Root->SetObjectField(TEXT("outerBounds"), BoundsObject(Manifest.OuterBounds));
    Root->SetStringField(TEXT("rowOrientation"), UTF8_TO_TCHAR(Manifest.RowOrientation.c_str()));
    Root->SetStringField(TEXT("heightEncoding"), UTF8_TO_TCHAR(Manifest.HeightEncoding.c_str()));
    Root->SetStringField(TEXT("validityEncoding"), UTF8_TO_TCHAR(Manifest.ValidityEncoding.c_str()));
    TArray<TSharedPtr<FJsonValue>> Shards;
    for (const SkiDomain::TerrainCoreShardDescriptor& Shard : Manifest.Shards)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("path"), UTF8_TO_TCHAR(Shard.Path.c_str()));
        Object->SetStringField(TEXT("sha256"), UTF8_TO_TCHAR(Shard.Sha256.c_str()));
        Object->SetNumberField(TEXT("bytes"), static_cast<double>(Shard.Bytes));
        Shards.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("shards"), Shards);
    TArray<TSharedPtr<FJsonValue>> Tiles;
    Tiles.Reserve(static_cast<int32>(Manifest.Tiles.size()));
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Manifest.Tiles)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("lodIndex"), Tile.LodIndex);
        Object->SetNumberField(TEXT("lodFactor"), Tile.LodFactor);
        Object->SetNumberField(TEXT("tileX"), Tile.TileX);
        Object->SetNumberField(TEXT("tileY"), Tile.TileY);
        Object->SetNumberField(TEXT("startColumn"), Tile.StartColumn);
        Object->SetNumberField(TEXT("startRow"), Tile.StartRow);
        Object->SetNumberField(TEXT("coreWidth"), Tile.CoreWidth);
        Object->SetNumberField(TEXT("coreHeight"), Tile.CoreHeight);
        Object->SetNumberField(TEXT("haloWest"), Tile.HaloWest);
        Object->SetNumberField(TEXT("haloNorth"), Tile.HaloNorth);
        Object->SetNumberField(TEXT("haloEast"), Tile.HaloEast);
        Object->SetNumberField(TEXT("haloSouth"), Tile.HaloSouth);
        Object->SetStringField(TEXT("heightPath"), UTF8_TO_TCHAR(Tile.HeightPath.c_str()));
        Object->SetStringField(TEXT("heightSha256"), UTF8_TO_TCHAR(Tile.HeightSha256.c_str()));
        Object->SetNumberField(TEXT("heightShardIndex"), Tile.HeightShardIndex);
        Object->SetNumberField(TEXT("heightOffset"), static_cast<double>(Tile.HeightOffset));
        Object->SetNumberField(TEXT("heightBytes"), static_cast<double>(Tile.HeightBytes));
        Object->SetNumberField(TEXT("heightRawBytes"), static_cast<double>(Tile.HeightRawBytes));
        Object->SetStringField(TEXT("validityPath"), UTF8_TO_TCHAR(Tile.ValidityPath.c_str()));
        Object->SetStringField(TEXT("validitySha256"), UTF8_TO_TCHAR(Tile.ValiditySha256.c_str()));
        Object->SetNumberField(TEXT("validityShardIndex"), Tile.ValidityShardIndex);
        Object->SetNumberField(TEXT("validityOffset"), static_cast<double>(Tile.ValidityOffset));
        Object->SetNumberField(TEXT("validityBytes"), static_cast<double>(Tile.ValidityBytes));
        Object->SetNumberField(TEXT("validityRawBytes"), static_cast<double>(Tile.ValidityRawBytes));
        Object->SetStringField(TEXT("provenanceId"), UTF8_TO_TCHAR(Tile.ProvenanceId.c_str()));
        Object->SetStringField(TEXT("processingVersion"), UTF8_TO_TCHAR(Tile.ProcessingVersion.c_str()));
        Tiles.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("tiles"), Tiles);
    return WriteJson(Root);
}

bool SkiPreparation::ParseTerrainCoreManifest(const FString& Json,
    SkiDomain::TerrainCoreManifest& OutManifest, FString& OutError)
{
    OutManifest = {};
    OutError.Reset();
    if (FTCHARToUTF8(Json).Length() > static_cast<int64>(SkiDomain::TerrainCoreMaxManifestBytes))
    {
        OutError = TEXT("TerrainCore manifest exceeds 8 MiB.");
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        OutError = TEXT("TerrainCore manifest is not valid JSON.");
        return false;
    }
    uint64 Number = 0;
    FString Registration;
    if (!ReadUint(Root, TEXT("schemaVersion"), MAX_uint32, Number)) goto Invalid;
    OutManifest.SchemaVersion = static_cast<uint32>(Number);
    ReadString(Root, TEXT("contentId"), OutManifest.ContentId);
    if (!ReadString(Root, TEXT("generatorVersion"), OutManifest.GeneratorVersion)) goto Invalid;
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(TEXT("processingVersions"), Values) || !Values
            || Values->Num() <= 0
            || Values->Num() > static_cast<int32>(
                SkiDomain::TerrainCoreMaxProcessingVersions)) goto Invalid;
        TSet<FString> SeenVersions;
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (!Value || !Value->TryGetString(Text) || Text.IsEmpty()
                || SeenVersions.Contains(Text)) goto Invalid;
            SeenVersions.Add(Text);
            OutManifest.ProcessingVersions.emplace_back(TCHAR_TO_UTF8(*Text));
        }
    }
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Root->TryGetObjectField(TEXT("source"), Object) || !Object || !*Object) goto Invalid;
        if (!ReadSource(*Object, OutManifest.Source)) goto Invalid;
    }
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(TEXT("additionalSources"), Values) || !Values
            || Values->Num() > static_cast<int32>(
                SkiDomain::TerrainCoreMaxAdditionalSources)) goto Invalid;
        TSet<FString> SeenSourceIds;
        SeenSourceIds.Add(UTF8_TO_TCHAR(OutManifest.Source.SourceId.c_str()));
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Object = Value ? Value->AsObject() : nullptr;
            SkiDomain::TerrainCoreSource Source;
            if (!ReadSource(Object, Source)) goto Invalid;
            const FString SourceId = UTF8_TO_TCHAR(Source.SourceId.c_str());
            if (SeenSourceIds.Contains(SourceId)) goto Invalid;
            SeenSourceIds.Add(SourceId);
            OutManifest.AdditionalSources.push_back(std::move(Source));
        }
    }
    {
        const TSharedPtr<FJsonObject>* Origin = nullptr;
        if (!Root->TryGetObjectField(TEXT("localOrigin"), Origin) || !Origin || !*Origin
            || !(*Origin)->TryGetNumberField(TEXT("latitudeDeg"),
                OutManifest.LocalOrigin.LatitudeDeg)
            || !(*Origin)->TryGetNumberField(TEXT("longitudeDeg"),
                OutManifest.LocalOrigin.LongitudeDeg)
            || !(*Origin)->TryGetNumberField(TEXT("heightM"), OutManifest.LocalOrigin.HeightM))
        {
            goto Invalid;
        }
    }
    if (!ReadUint(Root, TEXT("width"), MAX_uint32, Number)) goto Invalid;
    OutManifest.Width = static_cast<uint32>(Number);
    if (!ReadUint(Root, TEXT("height"), MAX_uint32, Number)) goto Invalid;
    OutManifest.Height = static_cast<uint32>(Number);
    if (!Root->TryGetNumberField(TEXT("deliveredEastSpacingM"), OutManifest.DeliveredEastSpacingM)
        || !Root->TryGetNumberField(TEXT("deliveredNorthSpacingM"), OutManifest.DeliveredNorthSpacingM)
        || !Root->TryGetStringField(TEXT("pixelRegistration"), Registration)
        || Registration != TEXT("sample-center")) goto Invalid;
    OutManifest.Registration = SkiDomain::PixelRegistration::SampleCenter;
    {
        const TSharedPtr<FJsonObject>* Centers = nullptr;
        const TSharedPtr<FJsonObject>* Outer = nullptr;
        if (!Root->TryGetObjectField(TEXT("sampleCenterBounds"), Centers) || !Centers
            || !Root->TryGetObjectField(TEXT("outerBounds"), Outer) || !Outer
            || !ReadBounds(*Centers, OutManifest.SampleCenterBounds)
            || !ReadBounds(*Outer, OutManifest.OuterBounds)) goto Invalid;
    }
    if (!ReadString(Root, TEXT("rowOrientation"), OutManifest.RowOrientation)
        || !ReadString(Root, TEXT("heightEncoding"), OutManifest.HeightEncoding)
        || !ReadString(Root, TEXT("validityEncoding"), OutManifest.ValidityEncoding)) goto Invalid;
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(TEXT("shards"), Values) || !Values
            || Values->Num() > static_cast<int32>(SkiDomain::TerrainCoreMaxShards)) goto Invalid;
        TSet<FString> NormalizedPaths;
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Object = Value ? Value->AsObject() : nullptr;
            SkiDomain::TerrainCoreShardDescriptor Shard;
            if (!ReadString(Object, TEXT("path"), Shard.Path)
                || !ReadString(Object, TEXT("sha256"), Shard.Sha256)
                || !ReadUint(Object, TEXT("bytes"), SkiDomain::TerrainCoreMaxAssetBytes, Number)) goto Invalid;
            Shard.Bytes = Number;
            FString Normalized = UTF8_TO_TCHAR(Shard.Path.c_str());
            Normalized.ToLowerInline();
            if (NormalizedPaths.Contains(Normalized)) goto Invalid;
            NormalizedPaths.Add(Normalized);
            OutManifest.Shards.push_back(std::move(Shard));
        }
    }
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(TEXT("tiles"), Values) || !Values
            || Values->Num() > static_cast<int32>(SkiDomain::TerrainCoreMaxTiles)) goto Invalid;
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> O = Value ? Value->AsObject() : nullptr;
            SkiDomain::TerrainCoreTileDescriptor Tile;
#define READ_TILE_UINT(Name, Member, MaxValue, Type) \
            if (!ReadUint(O, TEXT(Name), MaxValue, Number)) goto Invalid; Tile.Member = static_cast<Type>(Number)
            READ_TILE_UINT("lodIndex", LodIndex, MAX_uint8, uint8);
            READ_TILE_UINT("lodFactor", LodFactor, MAX_uint32, uint32);
            READ_TILE_UINT("tileX", TileX, MAX_uint32, uint32);
            READ_TILE_UINT("tileY", TileY, MAX_uint32, uint32);
            READ_TILE_UINT("startColumn", StartColumn, MAX_uint32, uint32);
            READ_TILE_UINT("startRow", StartRow, MAX_uint32, uint32);
            READ_TILE_UINT("coreWidth", CoreWidth, MAX_uint16, uint16);
            READ_TILE_UINT("coreHeight", CoreHeight, MAX_uint16, uint16);
            READ_TILE_UINT("haloWest", HaloWest, MAX_uint8, uint8);
            READ_TILE_UINT("haloNorth", HaloNorth, MAX_uint8, uint8);
            READ_TILE_UINT("haloEast", HaloEast, MAX_uint8, uint8);
            READ_TILE_UINT("haloSouth", HaloSouth, MAX_uint8, uint8);
            if (!ReadString(O, TEXT("heightPath"), Tile.HeightPath)
                || !ReadString(O, TEXT("heightSha256"), Tile.HeightSha256)) goto Invalid;
            READ_TILE_UINT("heightShardIndex", HeightShardIndex, MAX_uint32, uint32);
            if (!ReadUint(O, TEXT("heightOffset"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.HeightOffset)
                || !ReadUint(O, TEXT("heightBytes"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.HeightBytes)
                || !ReadUint(O, TEXT("heightRawBytes"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.HeightRawBytes)
                || !ReadString(O, TEXT("validityPath"), Tile.ValidityPath)
                || !ReadString(O, TEXT("validitySha256"), Tile.ValiditySha256)) goto Invalid;
            READ_TILE_UINT("validityShardIndex", ValidityShardIndex, MAX_uint32, uint32);
            if (!ReadUint(O, TEXT("validityOffset"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.ValidityOffset)
                || !ReadUint(O, TEXT("validityBytes"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.ValidityBytes)
                || !ReadUint(O, TEXT("validityRawBytes"), SkiDomain::TerrainCoreMaxAssetBytes, Tile.ValidityRawBytes)
                || !ReadString(O, TEXT("provenanceId"), Tile.ProvenanceId)
                || !ReadString(O, TEXT("processingVersion"), Tile.ProcessingVersion)) goto Invalid;
#undef READ_TILE_UINT
            OutManifest.Tiles.push_back(std::move(Tile));
        }
    }
    if (!SkiDomain::ValidateTerrainCore(OutManifest,
        static_cast<uint64>(FTCHARToUTF8(Json).Length())).Ok())
    {
        OutError = TEXT("TerrainCore manifest failed domain validation.");
        return false;
    }
    if (SerializeTerrainCoreManifest(OutManifest, true) != Json)
    {
        OutError = TEXT("TerrainCore manifest is not canonical.");
        return false;
    }
    return true;

Invalid:
    OutManifest = {};
    OutError = TEXT("TerrainCore manifest has missing or invalid fields.");
    return false;
}

namespace
{
bool OpenAtDirectory(const FString& StoreRoot, const FString& Directory, const FString& ContentId,
    SkiPreparation::TerrainCorePackageIndex& OutIndex, FString& Error)
{
    OutIndex = {};
    Error.Reset();
    if (!IsWithinDirectory(StoreRoot, Directory)
        || !ValidateNoReparsePath(StoreRoot, true, &Error)
        || !ValidateNoReparsePath(Directory, true, &Error))
    {
        if (Error.IsEmpty()) Error = TEXT("TerrainCore package path is unsafe.");
        return false;
    }
    FString Json;
    uint64 ManifestBytes = 0;
    if (!SecureReadUtf8(StoreRoot, Directory, TEXT("terraincore.json"),
            SkiDomain::TerrainCoreMaxManifestBytes, Json, ManifestBytes, Error)
        || !SkiPreparation::ParseTerrainCoreManifest(Json, OutIndex.Manifest, Error)
        || UTF8_TO_TCHAR(OutIndex.Manifest.ContentId.c_str()) != ContentId
        || HashUtf8(SkiPreparation::SerializeTerrainCoreManifest(OutIndex.Manifest, false)) != ContentId)
    {
        if (Error.IsEmpty()) Error = TEXT("TerrainCore content identity is invalid.");
        OutIndex = {};
        return false;
    }
    if (!SkiDomain::ValidateTerrainCore(OutIndex.Manifest,
            ManifestBytes).Ok())
    {
        Error = TEXT("TerrainCore package exceeds installed-size bounds.");
        OutIndex = {};
        return false;
    }
    TArray<FString> Declared{TEXT("terraincore.json")};
    for (const SkiDomain::TerrainCoreShardDescriptor& Shard : OutIndex.Manifest.Shards)
    {
        if (!SecureShardLength(StoreRoot, Directory, Shard.Path, Shard.Bytes, Error))
        {
            Error = TEXT("TerrainCore shard path or length is invalid.");
            OutIndex = {};
            return false;
        }
        Declared.Add(UTF8_TO_TCHAR(Shard.Path.c_str()));
    }
    if (!ValidateDeclaredFiles(Directory, Declared, Error))
    {
        OutIndex = {};
        return false;
    }
    OutIndex.PackageDirectory = Directory;
    return true;
}

bool ReadSelectedTile(const FString& StoreRoot,
    const SkiPreparation::TerrainCorePackageIndex& Index,
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    SkiPreparation::TerrainCoreDecodedTile& OutTile, FString& Error)
{
    OutTile = {};
    Error.Reset();
    if (Descriptor.HeightShardIndex >= Index.Manifest.Shards.size()
        || Descriptor.ValidityShardIndex >= Index.Manifest.Shards.size()
        || !EncodedLengthsAreBounded(Descriptor))
    {
        Error = TEXT("TerrainCore tile references an invalid shard or encoded length.");
        return false;
    }
    const SkiDomain::TerrainCoreShardDescriptor& HeightShard =
        Index.Manifest.Shards[Descriptor.HeightShardIndex];
    const SkiDomain::TerrainCoreShardDescriptor& ValidityShard =
        Index.Manifest.Shards[Descriptor.ValidityShardIndex];
    TArray<uint8> Heights, Validity;
    if (!SecureReadShardRange(StoreRoot, Index.PackageDirectory, HeightShard.Path,
            Descriptor.HeightOffset, Descriptor.HeightBytes, Heights, Error)
        || !SecureReadShardRange(StoreRoot, Index.PackageDirectory, ValidityShard.Path,
            Descriptor.ValidityOffset, Descriptor.ValidityBytes, Validity, Error)
        || SkiPreparation::Sha256(Heights) != UTF8_TO_TCHAR(Descriptor.HeightSha256.c_str())
        || SkiPreparation::Sha256(Validity) != UTF8_TO_TCHAR(Descriptor.ValiditySha256.c_str()))
    {
        if (Error.IsEmpty()) Error = TEXT("TerrainCore tile range hash is invalid.");
        return false;
    }
    return SkiPreparation::DecodeTerrainCoreTile(Descriptor, Heights, Validity, OutTile, Error);
}

bool SameDecodedSample(const SkiPreparation::TerrainCoreDecodedTile& First,
    const int32 FirstIndex, const SkiPreparation::TerrainCoreDecodedTile& Second,
    const int32 SecondIndex)
{
    return First.Validity.IsValidIndex(FirstIndex) && Second.Validity.IsValidIndex(SecondIndex)
        && First.Heights.IsValidIndex(FirstIndex) && Second.Heights.IsValidIndex(SecondIndex)
        && First.Validity[FirstIndex] == Second.Validity[SecondIndex]
        && FMemory::Memcmp(&First.Heights[FirstIndex], &Second.Heights[SecondIndex],
            sizeof(float)) == 0;
}

bool VerifyTileSemantics(const FString& StoreRoot,
    const SkiPreparation::TerrainCorePackageIndex& Index,
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    const SkiPreparation::TerrainCoreDecodedTile& Decoded, FString& Error)
{
    const uint32 FinestTilesX = (Index.Manifest.Width - 1U
        + SkiDomain::TerrainCoreTileCells - 1U) / SkiDomain::TerrainCoreTileCells;
    const uint32 FinestTilesY = (Index.Manifest.Height - 1U
        + SkiDomain::TerrainCoreTileCells - 1U) / SkiDomain::TerrainCoreTileCells;
    TMap<uint32, SkiPreparation::TerrainCoreDecodedTile> FinestRow;
    uint32 CachedTileY = MAX_uint32;
    for (uint32 LocalRow = 0; LocalRow < Decoded.StoredHeight; ++LocalRow)
    {
        const uint32 LodRow = Descriptor.StartRow + LocalRow - Descriptor.HaloNorth;
        const uint32 SourceRow = FMath::Min<uint64>(
            static_cast<uint64>(LodRow) * Descriptor.LodFactor,
            static_cast<uint64>(Index.Manifest.Height - 1U));
        const uint32 TileY = FMath::Min(SourceRow / SkiDomain::TerrainCoreTileCells,
            FinestTilesY - 1U);
        if (TileY != CachedTileY)
        {
            FinestRow.Reset();
            CachedTileY = TileY;
        }
        for (uint32 LocalColumn = 0; LocalColumn < Decoded.StoredWidth; ++LocalColumn)
        {
            const uint32 LodColumn = Descriptor.StartColumn + LocalColumn - Descriptor.HaloWest;
            const uint32 SourceColumn = FMath::Min<uint64>(
                static_cast<uint64>(LodColumn) * Descriptor.LodFactor,
                static_cast<uint64>(Index.Manifest.Width - 1U));
            const uint32 TileX = FMath::Min(SourceColumn / SkiDomain::TerrainCoreTileCells,
                FinestTilesX - 1U);
            SkiPreparation::TerrainCoreDecodedTile* Finest = FinestRow.Find(TileX);
            if (!Finest)
            {
                const SkiDomain::TerrainCoreTileDescriptor* FinestDescriptor = FindTile(
                    Index.Manifest, 0, TileX, TileY);
                SkiPreparation::TerrainCoreDecodedTile Loaded;
                if (!FinestDescriptor || !ReadSelectedTile(
                        StoreRoot, Index, *FinestDescriptor, Loaded, Error))
                {
                    if (Error.IsEmpty())
                        Error = TEXT("TerrainCore semantic verification cannot read finest data.");
                    return false;
                }
                Finest = &FinestRow.Add(TileX, std::move(Loaded));
            }
            const int32 FinestColumn = static_cast<int32>(SourceColumn
                - Finest->Descriptor.StartColumn + Finest->Descriptor.HaloWest);
            const int32 FinestRowIndex = static_cast<int32>(SourceRow
                - Finest->Descriptor.StartRow + Finest->Descriptor.HaloNorth);
            const int32 FinestIndex = FinestRowIndex * static_cast<int32>(Finest->StoredWidth)
                + FinestColumn;
            const int32 ActualIndex = static_cast<int32>(LocalRow * Decoded.StoredWidth
                + LocalColumn);
            if (!SameDecodedSample(Decoded, ActualIndex, *Finest, FinestIndex))
            {
                Error = TEXT("TerrainCore shared edge, halo, or deterministic LOD sample differs.");
                return false;
            }
        }
    }
    return true;
}

bool VerifyAtDirectory(const FString& StoreRoot,
    const SkiPreparation::TerrainCorePackageIndex& Index, FString& Error)
{
    Error.Reset();
    TArray<FString> Declared{TEXT("terraincore.json")};
    for (const SkiDomain::TerrainCoreShardDescriptor& Shard : Index.Manifest.Shards)
        Declared.Add(UTF8_TO_TCHAR(Shard.Path.c_str()));
    if (!ValidateDeclaredFiles(Index.PackageDirectory, Declared, Error)) return false;
    for (const SkiDomain::TerrainCoreShardDescriptor& Shard : Index.Manifest.Shards)
    {
        FString Hash;
        if (!SecureHashShard(StoreRoot, Index.PackageDirectory, Shard, Hash, Error)
            || Hash != UTF8_TO_TCHAR(Shard.Sha256.c_str()))
        {
            if (Error.IsEmpty()) Error = TEXT("TerrainCore shard hash is invalid.");
            return false;
        }
    }
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Index.Manifest.Tiles)
    {
        SkiPreparation::TerrainCoreDecodedTile Decoded;
        if (!ReadSelectedTile(StoreRoot, Index, Tile, Decoded, Error)
            || !VerifyTileSemantics(StoreRoot, Index, Tile, Decoded, Error)) return false;
    }
    if (!ValidateDeclaredFiles(Index.PackageDirectory, Declared, Error)) return false;
    return true;
}
}

SkiPreparation::TerrainCorePackageStore::TerrainCorePackageStore(FString InDataRoot)
    : Root(FPaths::ConvertRelativePathToFull(std::move(InDataRoot)))
{
}

bool SkiPreparation::TerrainCorePackageStore::WriteAndActivate(
    SkiDomain::TerrainCoreManifest Manifest, const SkiDomain::Heightfield& Finest,
    FString& OutPackageDirectory, SkiDomain::TerrainCoreManifest& OutManifest,
    FString& OutError,
    const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration) const
{
    OutPackageDirectory.Reset();
    OutManifest = {};
    OutError.Reset();
    if (!SkiDomain::IsValidHeightfield(Finest)
        || static_cast<uint64>(Finest.Width) * Finest.Height > SkiDomain::TerrainCoreMaxSamples)
    {
        OutError = TEXT("TerrainCore input heightfield is invalid.");
        return false;
    }
    Manifest.SchemaVersion = SkiDomain::TerrainCoreSchema;
    Manifest.ContentId.clear();
    Manifest.Width = Finest.Width;
    Manifest.Height = Finest.Height;
    Manifest.DeliveredEastSpacingM = Finest.EastSpacingM;
    Manifest.DeliveredNorthSpacingM = Finest.NorthSpacingM;
    Manifest.Registration = SkiDomain::PixelRegistration::SampleCenter;
    Manifest.SampleCenterBounds = {Finest.WestM,
        Finest.SampleNorthM(Finest.Height - 1U), Finest.EastM(Finest.Width - 1U), Finest.NorthM};
    if (!SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
        Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
        Manifest.SampleCenterBounds, Manifest.OuterBounds))
    {
        OutError = TEXT("TerrainCore input bounds are inconsistent.");
        return false;
    }
    Manifest.HeightEncoding = "f32le-deflate-fixed-v1";
    Manifest.ValidityEncoding = "bitset-deflate-fixed-v1";
    const TerrainCoreTileSource Source = [&Finest](
        const SkiDomain::TerrainCoreTileDescriptor& Planned,
        TerrainCoreEncodedTile& OutTile, FString& Error)
    {
        return DeriveTerrainCoreTile(Finest, Planned, OutTile, Error);
    };
    return WriteAndActivateFromTiles(std::move(Manifest), Source, OutPackageDirectory,
        OutManifest, OutError, Lease, SessionGeneration, OperationGeneration);
}

bool SkiPreparation::TerrainCorePackageStore::WriteAndActivateFromTiles(
    SkiDomain::TerrainCoreManifest Manifest, const TerrainCoreTileSource& TileSource,
    FString& OutPackageDirectory, SkiDomain::TerrainCoreManifest& OutManifest,
    FString& OutError,
    const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration) const
{
    OutPackageDirectory.Reset();
    OutManifest = {};
    OutError.Reset();
    const auto Current = [&]()
    {
        return !Lease || Lease->IsCurrent(SessionGeneration, OperationGeneration);
    };
    if (!Current() || Manifest.SchemaVersion != SkiDomain::TerrainCoreSchema
        || Manifest.Width < 2 || Manifest.Height < 2
        || Manifest.Registration != SkiDomain::PixelRegistration::SampleCenter
        || Manifest.HeightEncoding != "f32le-deflate-fixed-v1"
        || Manifest.ValidityEncoding != "bitset-deflate-fixed-v1")
    {
        OutError = TEXT("TerrainCore streaming metadata is invalid or the operation is stale.");
        return false;
    }
    Manifest.ContentId.clear();
    SkiDomain::TerrainCoreTilePlan Plan;
    if (!SkiDomain::PlanTerrainCoreTiles(Manifest.Width, Manifest.Height, Plan))
    {
        OutError = TEXT("TerrainCore tile plan exceeds its bounds.");
        return false;
    }
    Manifest.Tiles.clear();
    Manifest.Shards.clear();

    const FString StagingParent = FPaths::Combine(Root, TEXT(".terraincore-staging"));
    const FString Stage = FPaths::Combine(StagingParent,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager& Files = IFileManager::Get();
    if (!EnsureSecureDirectory(Root, OutError)
        || !EnsureSecureDirectory(StagingParent, OutError)
        || !EnsureSecureDirectory(FPaths::Combine(Stage, TEXT("shards")), OutError))
    {
        if (OutError.IsEmpty()) OutError = TEXT("Unable to create TerrainCore staging.");
        return false;
    }
    const auto Cleanup = [&]() { DeleteSecureTree(StagingParent, Stage); };
    FSecureTerrainWriter ShardHandle;
    FString ShardPath;
    uint64 ShardBytes = 0;
    uint32 ShardIndex = 0;
    const auto FinishShard = [&]() -> bool
    {
        if (!ShardHandle) return true;
        ShardHandle.Close();
        if (!ValidateNoReparsePath(ShardPath, true, &OutError)) return false;
        FString Hash;
        if (!HashFile(ShardPath, ShardBytes, Hash, OutError)) return false;
        SkiDomain::TerrainCoreShardDescriptor& Shard = Manifest.Shards.back();
        Shard.Bytes = ShardBytes;
        Shard.Sha256 = TCHAR_TO_UTF8(*Hash);
        return true;
    };
    const auto StartShard = [&]() -> bool
    {
        if (Manifest.Shards.size() >= SkiDomain::TerrainCoreMaxShards)
        {
            OutError = TEXT("TerrainCore package requires too many shards.");
            return false;
        }
        const FString Relative = FString::Printf(TEXT("shards/%04u.tcs"), ShardIndex++);
        ShardPath = FPaths::Combine(Stage, Relative);
        if (!ValidateNoReparsePath(Stage, true, &OutError)
            || !ValidateNoReparsePath(ShardPath, false, &OutError)) return false;
        if (!ShardHandle.Open(Root, ShardPath, OutError)
            || !ValidateNoReparsePath(ShardPath, true, &OutError))
        {
            OutError = TEXT("Unable to open a TerrainCore staging shard.");
            return false;
        }
        Manifest.Shards.push_back({TCHAR_TO_UTF8(*Relative), {}, 0});
        ShardBytes = 0;
        return true;
    };
    if (!StartShard()) { Cleanup(); return false; }
    for (const SkiDomain::TerrainCoreTileDescriptor& Geometry : Plan.Tiles)
    {
        if (!Current())
        {
            OutError = TEXT("TerrainCore operation became stale during derivation.");
            ShardHandle.Close(); Cleanup(); return false;
        }
        TerrainCoreEncodedTile Encoded;
        if (!TileSource(Geometry, Encoded, OutError))
        {
            ShardHandle.Close(); Cleanup(); return false;
        }
        if (Encoded.Descriptor.LodIndex != Geometry.LodIndex
            || Encoded.Descriptor.LodFactor != Geometry.LodFactor
            || Encoded.Descriptor.TileX != Geometry.TileX
            || Encoded.Descriptor.TileY != Geometry.TileY
            || Encoded.Descriptor.StartColumn != Geometry.StartColumn
            || Encoded.Descriptor.StartRow != Geometry.StartRow
            || Encoded.Descriptor.CoreWidth != Geometry.CoreWidth
            || Encoded.Descriptor.CoreHeight != Geometry.CoreHeight
            || Encoded.Descriptor.HaloWest != Geometry.HaloWest
            || Encoded.Descriptor.HaloNorth != Geometry.HaloNorth
            || Encoded.Descriptor.HaloEast != Geometry.HaloEast
            || Encoded.Descriptor.HaloSouth != Geometry.HaloSouth)
        {
            OutError = TEXT("TerrainCore tile source returned a different planned tile.");
            ShardHandle.Close(); Cleanup(); return false;
        }
        const uint64 Combined = static_cast<uint64>(Encoded.CompressedHeights.Num())
            + Encoded.CompressedValidity.Num();
        if (!EncodedLengthsAreBounded(Encoded.Descriptor)
            || Encoded.Descriptor.HeightBytes
                != static_cast<uint64>(Encoded.CompressedHeights.Num())
            || Encoded.Descriptor.ValidityBytes
                != static_cast<uint64>(Encoded.CompressedValidity.Num())
            || Combined > SkiDomain::TerrainCoreMaxEncodedTileBytes)
        {
            OutError = TEXT("TerrainCore encoded tile exceeds its codec or tile limit.");
            ShardHandle.Close(); Cleanup(); return false;
        }
        if (ShardBytes > 0 && (ShardBytes >= TerrainCoreShardTargetBytes
            || Combined > TerrainCoreShardTargetBytes - ShardBytes))
        {
            if (!FinishShard() || !StartShard()) { ShardHandle.Close(); Cleanup(); return false; }
        }
        SkiDomain::TerrainCoreTileDescriptor& Tile = Encoded.Descriptor;
        Tile.HeightShardIndex = static_cast<uint32>(Manifest.Shards.size() - 1U);
        Tile.HeightPath = Manifest.Shards.back().Path;
        Tile.HeightOffset = ShardBytes;
        if (!ShardHandle.Write(Encoded.CompressedHeights.GetData(),
            Encoded.CompressedHeights.Num()))
        {
            OutError = TEXT("Unable to write TerrainCore height tile.");
            ShardHandle.Close(); Cleanup(); return false;
        }
        ShardBytes += Tile.HeightBytes;
        Tile.ValidityShardIndex = Tile.HeightShardIndex;
        Tile.ValidityPath = Tile.HeightPath;
        Tile.ValidityOffset = ShardBytes;
        if (!ShardHandle.Write(Encoded.CompressedValidity.GetData(),
            Encoded.CompressedValidity.Num()))
        {
            OutError = TEXT("Unable to write TerrainCore validity tile.");
            ShardHandle.Close(); Cleanup(); return false;
        }
        ShardBytes += Tile.ValidityBytes;
        if (Tile.ProvenanceId.empty()) Tile.ProvenanceId = Manifest.Source.SourceId;
        if (Tile.ProcessingVersion.empty()) Tile.ProcessingVersion = "terraincore-derivation-v1";
        Manifest.Tiles.push_back(std::move(Tile));
    }
    if (!FinishShard()) { Cleanup(); return false; }
    const FString Unsigned = SerializeTerrainCoreManifest(Manifest, false);
    Manifest.ContentId = TCHAR_TO_UTF8(*HashUtf8(Unsigned));
    const SkiDomain::TerrainCoreValidation Validation = SkiDomain::ValidateTerrainCore(Manifest);
    if (!Validation.Ok())
    {
        OutError = FString::Printf(TEXT("Generated TerrainCore manifest failed validation (%d at %llu)."),
            static_cast<int32>(Validation.Error), static_cast<uint64>(Validation.Index));
        Cleanup(); return false;
    }
    const FString Json = SerializeTerrainCoreManifest(Manifest, true);
    const uint64 JsonBytes = static_cast<uint64>(FTCHARToUTF8(Json).Length());
    const FString ManifestPath = FPaths::Combine(Stage, TEXT("terraincore.json"));
    if (!SkiDomain::ValidateTerrainCore(Manifest, JsonBytes).Ok()
        || JsonBytes > SkiDomain::TerrainCoreMaxManifestBytes
        || !ValidateNoReparsePath(ManifestPath, false, &OutError)
        || !SecureWriteUtf8(Root, ManifestPath, Json, OutError))
    {
        OutError = TEXT("Unable to write the TerrainCore manifest.");
        Cleanup(); return false;
    }
    TerrainCorePackageIndex Staged;
    const FString Id = UTF8_TO_TCHAR(Manifest.ContentId.c_str());
    if (!OpenAtDirectory(Root, Stage, Id, Staged, OutError)
        || !VerifyAtDirectory(Root, Staged, OutError))
    {
        OutError = TEXT("Staged TerrainCore verification failed: ") + OutError;
        Cleanup(); return false;
    }
    const FString Packages = FPaths::Combine(Root, TEXT("TerrainCore"));
    const FString Target = FPaths::Combine(Packages, Id);
    if (!EnsureSecureDirectory(Packages, OutError))
    {
        OutError = TEXT("Unable to create the TerrainCore package directory.");
        Cleanup(); return false;
    }
    if (Files.DirectoryExists(*Target))
    {
        if (!Current())
        {
            OutError = TEXT("TerrainCore operation became stale before existing-target verification.");
            Cleanup(); return false;
        }
        TerrainCorePackageIndex Existing;
        if (!OpenAtDirectory(Root, Target, Id, Existing, OutError)
            || !VerifyAtDirectory(Root, Existing, OutError) || !Current())
        {
            if (OutError.IsEmpty())
                OutError = TEXT("TerrainCore operation became stale during existing-target verification.");
            Cleanup(); return false;
        }
        Cleanup();
    }
    else
    {
        bool Moved = false;
        const auto Activate = [&]()
        {
            if (ValidateNoReparsePath(Root, true, &OutError)
                && ValidateNoReparsePath(Packages, true, &OutError)
                && ValidateNoReparsePath(Stage, true, &OutError)
                && ValidateTreeNoReparse(Stage)
                && !Files.DirectoryExists(*Target) && !Files.FileExists(*Target))
            {
#if PLATFORM_WINDOWS
                Moved = SecureMoveDirectory(Root, Stage, Packages, Id, OutError);
#else
                Moved = Files.Move(*Target, *Stage, false, false, true, true)
                    && ValidateNoReparsePath(Target, true, &OutError);
#endif
            }
        };
        if (Lease && !Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Activate))
        {
            OutError = TEXT("TerrainCore operation became stale before activation.");
            Cleanup(); return false;
        }
        if (!Lease) Activate();
        if (!Moved)
        {
            if (OutError.IsEmpty()) OutError = TEXT("Unable to atomically activate TerrainCore.");
            Cleanup(); return false;
        }
    }
    TerrainCorePackageIndex Final;
    if (!OpenAtDirectory(Root, Target, Id, Final, OutError)
        || !VerifyAtDirectory(Root, Final, OutError)) return false;
    bool Published = false;
    const auto Publish = [&]()
    {
        OutPackageDirectory = Target;
        OutManifest = Manifest;
        Published = true;
    };
    if (Lease && !Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Publish))
    {
        OutError = TEXT("TerrainCore operation became stale before result publication.");
        return false;
    }
    if (!Lease) Publish();
    return Published;
}

bool SkiPreparation::TerrainCorePackageStore::Open(const FString& ContentId,
    TerrainCorePackageIndex& OutIndex, FString& OutError) const
{
    OutIndex = {};
    OutError.Reset();
    if (!IsCanonicalId(ContentId))
    {
        OutError = TEXT("TerrainCore content ID is invalid.");
        return false;
    }
    return OpenAtDirectory(Root, FPaths::Combine(Root, TEXT("TerrainCore"), ContentId),
        ContentId, OutIndex, OutError);
}

bool SkiPreparation::TerrainCorePackageStore::ReadTile(
    const TerrainCorePackageIndex& Index, const uint8 LodIndex,
    const uint32 TileX, const uint32 TileY, TerrainCoreDecodedTile& OutTile,
    FString& OutError) const
{
    OutTile = {};
    OutError.Reset();
    const SkiDomain::TerrainCoreTileDescriptor* Tile = FindTile(Index.Manifest,
        LodIndex, TileX, TileY);
    if (!Tile)
    {
        OutError = TEXT("TerrainCore tile does not exist.");
        return false;
    }
    return ReadSelectedTile(Root, Index, *Tile, OutTile, OutError);
}

bool SkiPreparation::TerrainCorePackageStore::Verify(
    const TerrainCorePackageIndex& Index, FString& OutError) const
{
    OutError.Reset();
    return VerifyAtDirectory(Root, Index, OutError);
}

bool SkiPreparation::TerrainCorePackageStore::WriteEditSetAndActivate(
    const SkiDomain::TerrainEditSet& Edits, const uint32 Width, const uint32 Height,
    FString& OutEditSetId, FString& OutError,
    const TSharedPtr<PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration) const
{
    OutEditSetId.Reset(); OutError.Reset();
    TerrainCorePackageIndex Base;
    if (!Open(UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Base, OutError)
        || Base.Manifest.Width != Width || Base.Manifest.Height != Height)
    {
        if (OutError.IsEmpty()) OutError = TEXT("Terrain edit dimensions do not match its TerrainCore.");
        return false;
    }
    if (!SkiDomain::ValidateTerrainEditSet(Edits, Width, Height).Ok())
    {
        OutError = TEXT("Terrain edit set is invalid.");
        return false;
    }
    const FString Unsigned = SerializeEdits(Edits, {});
    const FString Id = HashUtf8(Unsigned);
    const FString StagingParent = FPaths::Combine(Root, TEXT(".terrainedit-staging"));
    const FString Stage = FPaths::Combine(StagingParent,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager& Files = IFileManager::Get();
    const auto Cleanup = [&]() { DeleteSecureTree(StagingParent, Stage); };
    const FString StagedEditPath = FPaths::Combine(Stage, TEXT("edit.json"));
    if (!EnsureSecureDirectory(Root, OutError)
        || !EnsureSecureDirectory(StagingParent, OutError)
        || !EnsureSecureDirectory(Stage, OutError)
        || !ValidateNoReparsePath(StagedEditPath, false, &OutError)
        || !SecureWriteUtf8(Root, StagedEditPath, SerializeEdits(Edits, Id), OutError))
    {
        OutError = TEXT("Unable to write staged terrain edits.");
        Cleanup();
        return false;
    }
    {
        FString StagedJson, StagedId, StagedError;
        uint64 StagedBytes = 0;
        SkiDomain::TerrainEditSet StagedEdits;
        if (!ValidateDeclaredFiles(Stage, {TEXT("edit.json")}, StagedError)
            || !SecureReadUtf8(Root, Stage, TEXT("edit.json"),
                SkiDomain::TerrainCoreMaxManifestBytes, StagedJson, StagedBytes, StagedError)
            || !ParseEdits(StagedJson, StagedId, StagedEdits, StagedError)
            || StagedId != Id
            || HashUtf8(SerializeEdits(StagedEdits, {})) != Id
            || !SkiDomain::ValidateTerrainEditSet(StagedEdits, Width, Height).Ok())
        {
            OutError = TEXT("Staged terrain edit verification failed: ") + StagedError;
            Cleanup();
            return false;
        }
    }
    if (Lease && !Lease->IsCurrent(SessionGeneration, OperationGeneration))
    {
        OutError = TEXT("Terrain edit operation is stale.");
        Cleanup();
        return false;
    }
    const FString Parent = FPaths::Combine(Root, TEXT("TerrainEdits"),
        UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()));
    const FString Target = FPaths::Combine(Parent, Id);
    if (!EnsureSecureDirectory(Parent, OutError))
    {
        Cleanup();
        return false;
    }
    if (Files.DirectoryExists(*Target))
    {
        if (Lease && !Lease->IsCurrent(SessionGeneration, OperationGeneration))
        {
            OutError = TEXT("Terrain edit operation became stale before existing-target verification.");
            Cleanup();
            return false;
        }
        SkiDomain::TerrainEditSet Existing;
        if (!LoadEditSet(UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id,
            Width, Height, Existing, OutError)
            || (Lease && !Lease->IsCurrent(SessionGeneration, OperationGeneration)))
        {
            if (OutError.IsEmpty())
                OutError = TEXT("Terrain edit operation became stale during existing-target verification.");
            Cleanup();
            return false;
        }
        Cleanup();
    }
    else
    {
        bool Moved = false;
        const auto Activate = [&]()
        {
            if (ValidateNoReparsePath(Root, true, &OutError)
                && ValidateNoReparsePath(Parent, true, &OutError)
                && ValidateNoReparsePath(Stage, true, &OutError)
                && ValidateTreeNoReparse(Stage)
                && !Files.DirectoryExists(*Target) && !Files.FileExists(*Target))
            {
#if PLATFORM_WINDOWS
                Moved = SecureMoveDirectory(Root, Stage, Parent, Id, OutError);
#else
                Moved = Files.Move(*Target, *Stage, false, false, true, true)
                    && ValidateNoReparsePath(Target, true, &OutError);
#endif
            }
        };
        if (Lease && !Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Activate))
        {
            OutError = TEXT("Terrain edit operation became stale before activation.");
            Cleanup();
            return false;
        }
        if (!Lease) Activate();
        if (!Moved)
        {
            if (OutError.IsEmpty())
                OutError = TEXT("Unable to atomically activate terrain edits.");
            Cleanup();
            return false;
        }
    }
    SkiDomain::TerrainEditSet Verified;
    if (!LoadEditSet(UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id,
        Width, Height, Verified, OutError)) return false;
    bool Published = false;
    const auto Publish = [&]() { OutEditSetId = Id; Published = true; };
    if (Lease && !Lease->RunIfCurrent(SessionGeneration, OperationGeneration, Publish))
    {
        OutError = TEXT("Terrain edit operation became stale before result publication.");
        return false;
    }
    if (!Lease) Publish();
    return Published;
}

bool SkiPreparation::TerrainCorePackageStore::LoadEditSet(
    const FString& TerrainCoreId, const FString& EditSetId, const uint32 Width,
    const uint32 Height, SkiDomain::TerrainEditSet& OutEdits, FString& OutError) const
{
    OutEdits = {}; OutError.Reset();
    if (!IsCanonicalId(TerrainCoreId) || !IsCanonicalId(EditSetId))
    {
        OutError = TEXT("Terrain edit identity is invalid.");
        return false;
    }
    TerrainCorePackageIndex Base;
    if (!Open(TerrainCoreId, Base, OutError)
        || Base.Manifest.Width != Width || Base.Manifest.Height != Height)
    {
        if (OutError.IsEmpty()) OutError = TEXT("Terrain edit dimensions do not match its TerrainCore.");
        return false;
    }
    const FString Directory = FPaths::Combine(Root, TEXT("TerrainEdits"), TerrainCoreId,
        EditSetId);
    const FString Path = FPaths::Combine(Directory, TEXT("edit.json"));
    if (!IsWithinDirectory(Root, Directory)
        || !ValidateDeclaredFiles(Directory, {TEXT("edit.json")}, OutError)
        || !ValidateNoReparsePath(Path, true, &OutError))
    {
        OutEdits = {};
        return false;
    }
    FString Json, StoredId;
    uint64 Size = 0;
    if (!SecureReadUtf8(Root, Directory, TEXT("edit.json"),
            SkiDomain::TerrainCoreMaxManifestBytes, Json, Size, OutError)
        || !ParseEdits(Json, StoredId, OutEdits, OutError)
        || StoredId != EditSetId
        || UTF8_TO_TCHAR(OutEdits.TerrainCoreId.c_str()) != TerrainCoreId
        || HashUtf8(SerializeEdits(OutEdits, {})) != EditSetId
        || !SkiDomain::ValidateTerrainEditSet(OutEdits, Width, Height).Ok())
    {
        if (OutError.IsEmpty()) OutError = TEXT("Terrain edit content or identity is invalid.");
        OutEdits = {};
        return false;
    }
    return true;
}
