#include "SkiPreparation/S1mXmlReader.h"

namespace
{
using namespace SkiPreparation;

constexpr uint64 MaximumXmlBytes = 8ULL * 1024ULL * 1024ULL;
constexpr int32 MaximumXmlDepth = 64;
constexpr int32 MaximumArtifactIdentityLength = 512;

bool IsAsciiWhitespace(const uint8 Character)
{
    return Character == ' ' || Character == '\t' || Character == '\r' || Character == '\n';
}

bool MatchesAscii(const TArray<uint8>& Bytes, const int32 Position, const char* Text,
    const bool bIgnoreAsciiCase = false)
{
    int32 Index = 0;
    for (; Text[Index] != '\0'; ++Index)
    {
        if (Position + Index >= Bytes.Num()) return false;
        uint8 Actual = Bytes[Position + Index];
        uint8 Expected = static_cast<uint8>(Text[Index]);
        if (bIgnoreAsciiCase)
        {
            if (Actual >= 'a' && Actual <= 'z') Actual = static_cast<uint8>(Actual - 'a' + 'A');
            if (Expected >= 'a' && Expected <= 'z') Expected = static_cast<uint8>(Expected - 'a' + 'A');
        }
        if (Actual != Expected) return false;
    }
    return true;
}

bool IsNumericEntity(const TArray<uint8>& Bytes, const int32 Begin, const int32 End)
{
    if (Begin >= End || Bytes[Begin] != '#') return false;
    int32 Index = Begin + 1;
    bool bHex = false;
    if (Index < End && (Bytes[Index] == 'x' || Bytes[Index] == 'X'))
    {
        bHex = true;
        ++Index;
    }
    if (Index == End) return false;
    for (; Index < End; ++Index)
    {
        const uint8 Character = Bytes[Index];
        const bool bDigit = Character >= '0' && Character <= '9';
        const bool bHexDigit = (Character >= 'a' && Character <= 'f')
            || (Character >= 'A' && Character <= 'F');
        if (!bDigit && !(bHex && bHexDigit)) return false;
    }
    return true;
}

bool IsPredefinedEntity(const TArray<uint8>& Bytes, const int32 Begin, const int32 End)
{
    static const char* Names[] = {"amp", "lt", "gt", "apos", "quot"};
    for (const char* Name : Names)
    {
        int32 Length = 0;
        while (Name[Length] != '\0') ++Length;
        if (End - Begin != Length) continue;
        bool bMatches = true;
        for (int32 Index = 0; Index < Length; ++Index)
            bMatches = bMatches && Bytes[Begin + Index] == static_cast<uint8>(Name[Index]);
        if (bMatches) return true;
    }
    return false;
}

bool CheckEntityReference(const TArray<uint8>& Bytes, int32& Position,
    FString& OutFailureCode, FString& OutFailureDetail)
{
    const int32 Begin = Position + 1;
    int32 End = Begin;
    while (End < Bytes.Num() && Bytes[End] != ';')
    {
        if (Bytes[End] == '&' || Bytes[End] == '<' || IsAsciiWhitespace(Bytes[End]))
        {
            OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
            OutFailureDetail = TEXT("XML contains a malformed entity reference.");
            return false;
        }
        ++End;
    }
    if (End >= Bytes.Num())
    {
        OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
        OutFailureDetail = TEXT("XML contains an unterminated entity reference.");
        return false;
    }

    if (!IsPredefinedEntity(Bytes, Begin, End) && !IsNumericEntity(Bytes, Begin, End))
    {
        OutFailureCode = TEXT("S1M_XML_ENTITY_UNSUPPORTED");
        OutFailureDetail = TEXT("XML uses a named entity outside the five XML predefined entities; entity resolution is not supported.");
        return false;
    }
    Position = End;
    return true;
}

bool SkipUntil(const TArray<uint8>& Bytes, int32& Position, const char* Terminator)
{
    const int32 Start = Position;
    for (int32 Candidate = Start; Candidate < Bytes.Num(); ++Candidate)
    {
        if (MatchesAscii(Bytes, Candidate, Terminator))
        {
            int32 Length = 0;
            while (Terminator[Length] != '\0') ++Length;
            Position = Candidate + Length;
            return true;
        }
    }
    return false;
}

bool PreflightXmlStructure(const TArray<uint8>& Bytes, FString& OutFailureCode,
    FString& OutFailureDetail)
{
    int32 Position = 0;
    if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
        Position = 3;

    int32 Depth = 0;
    for (; Position < Bytes.Num(); ++Position)
    {
        const uint8 Character = Bytes[Position];
        if (Character == 0)
        {
            OutFailureCode = TEXT("S1M_XML_ENCODING_UNSUPPORTED");
            OutFailureDetail = TEXT("XML contains NUL bytes; only ASCII-compatible encodings can be safely preflighted.");
            return false;
        }

        if (Character == '&')
        {
            if (!CheckEntityReference(Bytes, Position, OutFailureCode, OutFailureDetail)) return false;
            continue;
        }
        if (Character != '<') continue;

        if (MatchesAscii(Bytes, Position, "<!--"))
        {
            Position += 4;
            if (!SkipUntil(Bytes, Position, "-->"))
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML contains an unterminated comment.");
                return false;
            }
            --Position;
            continue;
        }
        if (MatchesAscii(Bytes, Position, "<![CDATA["))
        {
            Position += 9;
            if (!SkipUntil(Bytes, Position, "]]>"))
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML contains an unterminated CDATA section.");
                return false;
            }
            --Position;
            continue;
        }
        if (MatchesAscii(Bytes, Position, "<?"))
        {
            Position += 2;
            if (!SkipUntil(Bytes, Position, "?>"))
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML contains an unterminated processing instruction.");
                return false;
            }
            --Position;
            continue;
        }
        if (MatchesAscii(Bytes, Position, "<!", true))
        {
            OutFailureCode = TEXT("S1M_XML_DTD_FORBIDDEN");
            OutFailureDetail = TEXT("XML declarations and DTDs are rejected by the bounded sidecar reader.");
            return false;
        }

        int32 NamePosition = Position + 1;
        const bool bClosing = NamePosition < Bytes.Num() && Bytes[NamePosition] == '/';
        if (bClosing) ++NamePosition;
        const int32 NameBegin = NamePosition;
        while (NamePosition < Bytes.Num() && !IsAsciiWhitespace(Bytes[NamePosition])
            && Bytes[NamePosition] != '/' && Bytes[NamePosition] != '>')
        {
            const uint8 NameCharacter = Bytes[NamePosition];
            if (NameCharacter == '<' || NameCharacter == '=' || NameCharacter == '\'' || NameCharacter == '"')
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML contains an invalid element name.");
                return false;
            }
            ++NamePosition;
        }
        if (NamePosition == NameBegin)
        {
            OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
            OutFailureDetail = TEXT("XML contains an element without a name.");
            return false;
        }

        uint8 Quote = 0;
        int32 TagEnd = NamePosition;
        for (; TagEnd < Bytes.Num(); ++TagEnd)
        {
            const uint8 TagCharacter = Bytes[TagEnd];
            if (Quote != 0)
            {
                if (TagCharacter == Quote) Quote = 0;
                else if (TagCharacter == '&')
                {
                    int32 EntityPosition = TagEnd;
                    if (!CheckEntityReference(Bytes, EntityPosition, OutFailureCode, OutFailureDetail)) return false;
                    TagEnd = EntityPosition;
                }
                continue;
            }
            if (TagCharacter == '\'' || TagCharacter == '"')
            {
                Quote = TagCharacter;
                continue;
            }
            if (TagCharacter == '>') break;
            if (TagCharacter == '<')
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML begins a new element before closing the current tag.");
                return false;
            }
        }
        if (TagEnd >= Bytes.Num() || Quote != 0)
        {
            OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
            OutFailureDetail = TEXT("XML contains an unterminated element tag or quoted attribute.");
            return false;
        }

        int32 LastContent = TagEnd - 1;
        while (LastContent > NamePosition && IsAsciiWhitespace(Bytes[LastContent])) --LastContent;
        const bool bSelfClosing = !bClosing && Bytes[LastContent] == '/';
        if (bClosing)
        {
            if (Depth <= 0)
            {
                OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
                OutFailureDetail = TEXT("XML closes an element when no element is open.");
                return false;
            }
            --Depth;
        }
        else if (!bSelfClosing)
        {
            ++Depth;
            if (Depth > MaximumXmlDepth)
            {
                OutFailureCode = TEXT("S1M_XML_DEPTH_LIMIT");
                OutFailureDetail = FString::Printf(TEXT("XML nesting exceeds the fixed depth limit of %d."), MaximumXmlDepth);
                return false;
            }
        }
        Position = TagEnd;
    }

    if (Depth != 0)
    {
        OutFailureCode = TEXT("S1M_XML_STRUCTURE_INVALID");
        OutFailureDetail = TEXT("XML has unclosed elements.");
        return false;
    }
    return true;
}

void SetFailure(FString& OutCode, FString& OutDetail, const TCHAR* Code, const TCHAR* Detail)
{
    OutCode = Code;
    OutDetail = Detail;
}
}

namespace SkiPreparation
{
bool PreflightS1mXmlSidecar(const TArray<uint8>& XmlBytes,
    const FS1mLineageArtifactEvidence& Artifact, const FS1mLineageLimits& Limits,
    FS1mXmlSidecarEvidence& OutEvidence, FString& OutFailureCode, FString& OutFailureDetail)
{
    OutEvidence = FS1mXmlSidecarEvidence();
    OutFailureCode.Reset();
    OutFailureDetail.Reset();

    if (Limits.MaxXmlBytes == 0 || Limits.MaxXmlBytes > MaximumXmlBytes
        || Limits.MaxStringLength <= 0 || Limits.MaxStringLength > MaximumArtifactIdentityLength)
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_XML_LIMITS_INVALID"),
            TEXT("XML limits must remain within the fixed byte and identity bounds."));
        return false;
    }
    if (XmlBytes.IsEmpty() || static_cast<uint64>(XmlBytes.Num()) > Limits.MaxXmlBytes)
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_XML_SIZE_INVALID"),
            TEXT("XML input is empty or exceeds the configured byte cap."));
        return false;
    }
    if (!Artifact.bExactSizeProven || Artifact.ObjectBytes == 0
        || Artifact.ObjectBytes != static_cast<uint64>(XmlBytes.Num()))
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_XML_ARTIFACT_SIZE_UNPROVEN"),
            TEXT("Caller-provided exact object size is absent or does not match the raw XML buffer."));
        return false;
    }
    if (Artifact.Product != TEXT("S1M") || Artifact.TileId.IsEmpty()
        || Artifact.PublicationDate.IsEmpty() || Artifact.TileId.Len() > Limits.MaxStringLength
        || Artifact.PublicationDate.Len() > Limits.MaxStringLength)
    {
        SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_XML_ARTIFACT_IDENTITY_UNPROVEN"),
            TEXT("Caller-provided artifact identity is incomplete or outside the configured string bound."));
        return false;
    }

    OutEvidence.Artifact = Artifact;
    if (!PreflightXmlStructure(XmlBytes, OutFailureCode, OutFailureDetail)) return false;

    SetFailure(OutFailureCode, OutFailureDetail, TEXT("S1M_XML_SCHEMA_UNPINNED"),
        TEXT("Raw XML passed the safety preflight, but no authoritative per-tile S1M schema and real fixture are pinned; all semantic lineage evidence remains unproven."));
    return false;
}
}
