#include "SkiPreparation/SelectorProtocol.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool SkiPreparation::ValidateSelectorMessage(const FString& Json, const FString& ExpectedToken,
    const uint64 ExpectedGeneration, Request& OutRequest, FString& OutError)
{
    if (Json.Len() <= 0 || Json.Len() > 4096 || ExpectedToken.IsEmpty())
    {
        OutError = TEXT("Selector message size or token is invalid.");
        return false;
    }
    TSharedPtr<FJsonObject> Object;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object
        || Object->Values.Num() != 8)
    {
        OutError = TEXT("Selector message must contain exactly the approved fields.");
        return false;
    }
    static const TSet<FString> Allowed{TEXT("token"), TEXT("generation"), TEXT("name"), TEXT("profile"),
        TEXT("west"), TEXT("south"), TEXT("east"), TEXT("north")};
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Object->Values)
    {
        if (!Allowed.Contains(Entry.Key))
        {
            OutError = TEXT("Selector message contains an unapproved field.");
            return false;
        }
    }
    FString Token;
    FString Profile;
    double Generation = 0.0;
    Request Candidate;
    if (!Object->TryGetStringField(TEXT("token"), Token) || Token != ExpectedToken
        || !Object->TryGetNumberField(TEXT("generation"), Generation)
        || Generation != static_cast<double>(ExpectedGeneration)
        || !Object->TryGetStringField(TEXT("name"), Candidate.Name)
        || !Object->TryGetStringField(TEXT("profile"), Profile)
        || !Object->TryGetNumberField(TEXT("west"), Candidate.Bounds.WestDeg)
        || !Object->TryGetNumberField(TEXT("south"), Candidate.Bounds.SouthDeg)
        || !Object->TryGetNumberField(TEXT("east"), Candidate.Bounds.EastDeg)
        || !Object->TryGetNumberField(TEXT("north"), Candidate.Bounds.NorthDeg))
    {
        OutError = TEXT("Selector token, generation, or request fields are invalid.");
        return false;
    }
    if (Profile == TEXT("medium")) Candidate.Profile = SourceProfile::Medium;
    else
    {
        OutError = TEXT("Selector profile must be medium. Verified lidar High is offered only after P1A coverage preflight.");
        return false;
    }
    Candidate.SessionGeneration = ExpectedGeneration;
    Candidate.OperationGeneration = ExpectedGeneration;
    if (!ValidateRequest(Candidate, OutError)) return false;
    OutRequest = std::move(Candidate);
    return true;
}
