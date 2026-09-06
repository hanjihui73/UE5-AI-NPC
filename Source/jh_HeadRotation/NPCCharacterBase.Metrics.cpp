#include "NPCCharacterBase.h"
#include "NPCPerceptionComponent.h"
#include "NPCGazeDecisionComponent.h"
#include "NPCGazeMotorComponent.h"
#include "Kismet/GameplayStatics.h"
#include "AIController.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/CharacterMovementComponent.h" 
#include "Components/CapsuleComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "MassActorSubsystem.h"
#include "Kismet/KismetMathLibrary.h"
#include "NPCDetourCrowdAIController.h"
#include "SafetyStimulusComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Components/BoxComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"


namespace
{
    constexpr int32 GazeSourceCount = 5;

    struct FPersonaGazeAggregate
    {
        int32 NPCCount = 0;
        int32 EventCounts[GazeSourceCount] = { 0, 0, 0, 0, 0 };
        double Durations[GazeSourceCount] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
        int32 CandidateExposureCounts[GazeSourceCount] = { 0, 0, 0, 0, 0 };
        int32 CandidateSelectedCounts[GazeSourceCount] = { 0, 0, 0, 0, 0 };
        int32 SocialOriginEventCounts[GazeSourceCount] = { 0, 0, 0, 0, 0 };
        double SocialOriginDurations[GazeSourceCount] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
        double SocialObserverSeconds = 0.0;
        double SocialSeconds = 0.0;
    };

    int32 ActiveMeasuredNPCs = 0;
    int32 SessionNPCCount = 0;
    FString SessionId;
    FPersonaGazeAggregate SessionPersonaAggregates[3];

    int32 PersonaToIndex(EPersonaType Persona)
    {
        switch (Persona)
        {
        case EPersonaType::CAUTIOUS: return 0;
        case EPersonaType::RUSHER: return 1;
        case EPersonaType::SIGHTSEER: return 2;
        }
        return 0;
    }

    FString PersonaFileName(int32 PersonaIndex)
    {
        switch (PersonaIndex)
        {
        case 0: return TEXT("Cautious.csv");
        case 1: return TEXT("Rusher.csv");
        case 2: return TEXT("Sightseer.csv");
        }
        return TEXT("Unknown.csv");
    }

    double EventRate(const FPersonaGazeAggregate& Aggregate, int32 SourceIndex)
    {
        int32 TotalEvents = 0;
        for (int32 Index = 1; Index < GazeSourceCount; ++Index)
        {
            TotalEvents += Aggregate.EventCounts[Index];
        }
        return TotalEvents > 0
            ? 100.0 * Aggregate.EventCounts[SourceIndex] / TotalEvents
            : 0.0;
    }

    double AverageDuration(const FPersonaGazeAggregate& Aggregate, int32 SourceIndex)
    {
        return Aggregate.EventCounts[SourceIndex] > 0
            ? Aggregate.Durations[SourceIndex] / Aggregate.EventCounts[SourceIndex]
            : 0.0;
    }

    double ConditionalSelectionRate(const FPersonaGazeAggregate& Aggregate, int32 SourceIndex)
    {
        return Aggregate.CandidateExposureCounts[SourceIndex] > 0
            ? 100.0 * Aggregate.CandidateSelectedCounts[SourceIndex] /
                Aggregate.CandidateExposureCounts[SourceIndex]
            : 0.0;
    }

    double SocialOriginRate(const FPersonaGazeAggregate& Aggregate, int32 OriginIndex)
    {
        int32 TotalSocialEvents = 0;
        for (int32 Index = 1; Index < GazeSourceCount; ++Index)
        {
            TotalSocialEvents += Aggregate.SocialOriginEventCounts[Index];
        }
        return TotalSocialEvents > 0
            ? 100.0 * Aggregate.SocialOriginEventCounts[OriginIndex] / TotalSocialEvents
            : 0.0;
    }

    double SocialOriginAverageDuration(const FPersonaGazeAggregate& Aggregate, int32 OriginIndex)
    {
        return Aggregate.SocialOriginEventCounts[OriginIndex] > 0
            ? Aggregate.SocialOriginDurations[OriginIndex] /
                Aggregate.SocialOriginEventCounts[OriginIndex]
            : 0.0;
    }

    void AppendCsvLine(const FString& FilePath, const FString& Header, const FString& Line)
    {
        const bool bNeedsHeader = !IFileManager::Get().FileExists(*FilePath);
        const FString TextToAppend = (bNeedsHeader ? Header : FString()) + Line;
        FFileHelper::SaveStringToFile(
            TextToAppend,
            *FilePath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(),
            FILEWRITE_Append);
    }

    void SaveGazeMeasurementSession()
    {
        // Keep legacy files untouched. All future sessions are written to only
        // two consolidated CSV files in this subfolder.
        const FString OutputDirectory = FPaths::Combine(
            FPaths::ProjectSavedDir(), TEXT("CrowdMetrics"), TEXT("Consolidated"));
        IFileManager::Get().MakeDirectory(*OutputDirectory, true);

        const FString MetricsHeader =
            TEXT("Session,NPCCount,Persona,PersonaCount,")
            TEXT("ServerExposures,ServerSelections,ServerConditionalRate,ServerEvents,ServerSelectionRate,ServerAvgDuration,")
            TEXT("SafetyExposures,SafetySelections,SafetyConditionalRate,SafetyEvents,SafetySelectionRate,SafetyAvgDuration,")
            TEXT("SoundExposures,SoundSelections,SoundConditionalRate,SoundEvents,SoundSelectionRate,SoundAvgDuration,")
            TEXT("SocialExposures,SocialSelections,SocialConditionalRate,SocialEvents,SocialSelectionRate,SocialAvgDuration,")
            TEXT("AverageSocialObservers,")
            TEXT("SocialSafetyCount,SocialSafetyRate,SocialSafetyAvgDuration,")
            TEXT("SocialSoundCount,SocialSoundRate,SocialSoundAvgDuration,")
            TEXT("SocialInterestCount,SocialInterestRate,SocialInterestAvgDuration,")
            TEXT("SocialInformationCount,SocialInformationRate,SocialInformationAvgDuration\n");

        for (int32 PersonaIndex = 0; PersonaIndex < 3; ++PersonaIndex)
        {
            const FPersonaGazeAggregate& Aggregate = SessionPersonaAggregates[PersonaIndex];
            const double AverageSocialObservers = Aggregate.SocialSeconds > 0.0
                ? Aggregate.SocialObserverSeconds / Aggregate.SocialSeconds
                : 0.0;
            const FString Line = FString::Printf(
                TEXT("%s,%d,%s,%d,")
                TEXT("%d,%d,%.3f,%d,%.3f,%.3f,")
                TEXT("%d,%d,%.3f,%d,%.3f,%.3f,")
                TEXT("%d,%d,%.3f,%d,%.3f,%.3f,")
                TEXT("%d,%d,%.3f,%d,%.3f,%.3f,%.3f,")
                TEXT("%d,%.3f,%.3f,%d,%.3f,%.3f,%d,%.3f,%.3f,%d,%.3f,%.3f\n"),
                *SessionId, SessionNPCCount,
                *FPaths::GetBaseFilename(PersonaFileName(PersonaIndex)), Aggregate.NPCCount,
                Aggregate.CandidateExposureCounts[1], Aggregate.CandidateSelectedCounts[1],
                ConditionalSelectionRate(Aggregate, 1), Aggregate.EventCounts[1],
                EventRate(Aggregate, 1), AverageDuration(Aggregate, 1),
                Aggregate.CandidateExposureCounts[2], Aggregate.CandidateSelectedCounts[2],
                ConditionalSelectionRate(Aggregate, 2), Aggregate.EventCounts[2],
                EventRate(Aggregate, 2), AverageDuration(Aggregate, 2),
                Aggregate.CandidateExposureCounts[3], Aggregate.CandidateSelectedCounts[3],
                ConditionalSelectionRate(Aggregate, 3), Aggregate.EventCounts[3],
                EventRate(Aggregate, 3), AverageDuration(Aggregate, 3),
                Aggregate.CandidateExposureCounts[4], Aggregate.CandidateSelectedCounts[4],
                ConditionalSelectionRate(Aggregate, 4), Aggregate.EventCounts[4],
                EventRate(Aggregate, 4), AverageDuration(Aggregate, 4), AverageSocialObservers,
                Aggregate.SocialOriginEventCounts[1], SocialOriginRate(Aggregate, 1),
                SocialOriginAverageDuration(Aggregate, 1),
                Aggregate.SocialOriginEventCounts[2], SocialOriginRate(Aggregate, 2),
                SocialOriginAverageDuration(Aggregate, 2),
                Aggregate.SocialOriginEventCounts[3], SocialOriginRate(Aggregate, 3),
                SocialOriginAverageDuration(Aggregate, 3),
                Aggregate.SocialOriginEventCounts[4], SocialOriginRate(Aggregate, 4),
                SocialOriginAverageDuration(Aggregate, 4));
            AppendCsvLine(
                FPaths::Combine(OutputDirectory, TEXT("PersonaMetrics.csv")),
                MetricsHeader,
                Line);
        }

        const FPersonaGazeAggregate& Cautious = SessionPersonaAggregates[0];
        const FPersonaGazeAggregate& Rusher = SessionPersonaAggregates[1];
        const FPersonaGazeAggregate& Sightseer = SessionPersonaAggregates[2];
        const double CautiousSafetyConditional = ConditionalSelectionRate(Cautious, 2);
        const double RusherSafetyConditional = ConditionalSelectionRate(Rusher, 2);
        const double SightseerSafetyConditional = ConditionalSelectionRate(Sightseer, 2);
        const double CautiousSoundConditional = ConditionalSelectionRate(Cautious, 3);
        const double RusherSoundConditional = ConditionalSelectionRate(Rusher, 3);
        const double SightseerSoundConditional = ConditionalSelectionRate(Sightseer, 3);
        const double CautiousSocialConditional = ConditionalSelectionRate(Cautious, 4);
        const double RusherSocialConditional = ConditionalSelectionRate(Rusher, 4);
        const double SightseerSocialConditional = ConditionalSelectionRate(Sightseer, 4);
        const double CautiousServerConditional = ConditionalSelectionRate(Cautious, 1);
        const double RusherServerConditional = ConditionalSelectionRate(Rusher, 1);
        const double SightseerServerConditional = ConditionalSelectionRate(Sightseer, 1);

        AppendCsvLine(
            FPaths::Combine(OutputDirectory, TEXT("PersonaComparison.csv")),
            TEXT("Session,NPCCount,CautiousCount,SightseerCount,RusherCount,")
            TEXT("CautiousSafetyRate,SightseerSafetyRate,RusherSafetyRate,")
            TEXT("CautiousSoundRate,SightseerSoundRate,RusherSoundRate,")
            TEXT("CautiousSocialRate,SightseerSocialRate,RusherSocialRate,")
            TEXT("CautiousServerRate,SightseerServerRate,RusherServerRate,")
            TEXT("CautiousSafetyGapVsBestOther,SightseerSoundGapVsBestOther,")
            TEXT("SightseerSocialGapVsBestOther,RusherServerGapVsBestOther,")
            TEXT("CautiousSocialSafetyRate,SightseerSocialInterestRate,RusherSocialInterestRate\n"),
            FString::Printf(
                TEXT("%s,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,")
                TEXT("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n"),
                *SessionId, SessionNPCCount, Cautious.NPCCount, Sightseer.NPCCount, Rusher.NPCCount,
                CautiousSafetyConditional, SightseerSafetyConditional, RusherSafetyConditional,
                CautiousSoundConditional, SightseerSoundConditional, RusherSoundConditional,
                CautiousSocialConditional, SightseerSocialConditional, RusherSocialConditional,
                CautiousServerConditional, SightseerServerConditional, RusherServerConditional,
                CautiousSafetyConditional - FMath::Max(SightseerSafetyConditional, RusherSafetyConditional),
                SightseerSoundConditional - FMath::Max(CautiousSoundConditional, RusherSoundConditional),
                SightseerSocialConditional - FMath::Max(CautiousSocialConditional, RusherSocialConditional),
                RusherServerConditional - FMath::Max(CautiousServerConditional, SightseerServerConditional),
                SocialOriginRate(Cautious, 1),
                SocialOriginRate(Sightseer, 3),
                SocialOriginRate(Rusher, 3)));

        UE_LOG(LogTemp, Log, TEXT("Crowd gaze metrics saved to %s"), *OutputDirectory);
    }
}


void ANPCCharacterBase::RegisterGazeMeasurement()
{
    ResetGazeMeasurement();
    if (ActiveMeasuredNPCs == 0)
    {
        SessionNPCCount = 0;
        SessionId = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        for (FPersonaGazeAggregate& Aggregate : SessionPersonaAggregates)
        {
            Aggregate = FPersonaGazeAggregate();
        }
    }
    ++ActiveMeasuredNPCs;
    ++SessionNPCCount;
    bGazeMeasurementRegistered = true;
}

void ANPCCharacterBase::ResetGazeMeasurement()
{
    MeasuredGazeSource = EGazeSource::None;
    MeasuredGazeTarget = TEXT("none");
    MeasuredGazeStartTime = 0.0f;
    SocialObserverSeconds = 0.0f;
    MeasuredSocialSeconds = 0.0f;
    MeasuredSocialOrigin = 0;
    for (int32 Index = 0; Index < GazeSourceCount; ++Index)
    {
        GazeMeasurementEventCounts[Index] = 0;
        GazeMeasurementDurations[Index] = 0.0f;
        GazeCandidateExposureCounts[Index] = 0;
        GazeCandidateSelectedCounts[Index] = 0;
        bGazeCandidateActive[Index] = false;
        bGazeCandidateSelectedDuringExposure[Index] = false;
        SocialOriginEventCounts[Index] = 0;
        SocialOriginDurations[Index] = 0.0f;
    }
}

void ANPCCharacterBase::UpdateGazeMeasurement(float DeltaTime)
{
    if (!GetWorld())
    {
        return;
    }

    FString CurrentTarget = TEXT("none");
    switch (CurrentGazeSource)
    {
    case EGazeSource::Server:
        CurrentTarget = CurrentServerTarget;
        break;
    case EGazeSource::Safety:
        CurrentTarget = TEXT("Threat");
        break;
    case EGazeSource::Sound:
        CurrentTarget = TEXT("SoundSource");
        break;
    case EGazeSource::Social:
        CurrentTarget = CurrentSocialInfluenceTargetName;
        break;
    default:
        break;
    }
    const int32 CurrentSocialOrigin = CurrentGazeSource == EGazeSource::Social
        ? GetCurrentSocialMeasurementOrigin()
        : 0;
    if (CurrentGazeSource != MeasuredGazeSource ||
        CurrentTarget != MeasuredGazeTarget ||
        CurrentSocialOrigin != MeasuredSocialOrigin)
    {
        FinishCurrentGazeMeasurement();
        MeasuredGazeSource = CurrentGazeSource;
        MeasuredGazeTarget = CurrentTarget;
        MeasuredSocialOrigin = CurrentSocialOrigin;
        MeasuredGazeStartTime = GetWorld()->GetTimeSeconds();
    }

    if (MeasuredGazeSource == EGazeSource::Social)
    {
        const float SafeDeltaTime = FMath::Max(0.0f, DeltaTime);
        SocialObserverSeconds += FMath::Max(1, CurrentSocialObserverCount) * SafeDeltaTime;
        MeasuredSocialSeconds += SafeDeltaTime;
    }
}

void ANPCCharacterBase::FinishCurrentGazeMeasurement()
{
    if (!GetWorld() || MeasuredGazeSource == EGazeSource::None)
    {
        return;
    }

    const int32 SourceIndex = static_cast<int32>(MeasuredGazeSource);
    if (SourceIndex > 0 && SourceIndex < GazeSourceCount)
    {
        const float EventDuration = FMath::Max(
            0.0f, GetWorld()->GetTimeSeconds() - MeasuredGazeStartTime);
        ++GazeMeasurementEventCounts[SourceIndex];
        GazeMeasurementDurations[SourceIndex] += EventDuration;
        if (MeasuredGazeSource == EGazeSource::Social &&
            MeasuredSocialOrigin > 0 && MeasuredSocialOrigin < GazeSourceCount)
        {
            ++SocialOriginEventCounts[MeasuredSocialOrigin];
            SocialOriginDurations[MeasuredSocialOrigin] += EventDuration;
        }
    }
    MeasuredGazeSource = EGazeSource::None;
    MeasuredGazeTarget = TEXT("none");
    MeasuredGazeStartTime = 0.0f;
    MeasuredSocialOrigin = 0;
}

void ANPCCharacterBase::UpdateGazeCandidateMeasurement(
    const TArray<FThreatSource>& Candidates, EGazeSource WinningSource)
{
    bool bPresent[GazeSourceCount] = { false, false, false, false, false };
    for (const FThreatSource& Candidate : Candidates)
    {
        const int32 SourceIndex = static_cast<int32>(Candidate.Source);
        if (SourceIndex > 0 && SourceIndex < GazeSourceCount && Candidate.Intensity > 0.0f)
        {
            bPresent[SourceIndex] = true;
        }
    }

    const int32 WinningIndex = static_cast<int32>(WinningSource);
    for (int32 SourceIndex = 1; SourceIndex < GazeSourceCount; ++SourceIndex)
    {
        if (bPresent[SourceIndex] && !bGazeCandidateActive[SourceIndex])
        {
            bGazeCandidateActive[SourceIndex] = true;
            bGazeCandidateSelectedDuringExposure[SourceIndex] = false;
        }

        if (bPresent[SourceIndex] && WinningIndex == SourceIndex)
        {
            bGazeCandidateSelectedDuringExposure[SourceIndex] = true;
        }

        if (!bPresent[SourceIndex] && bGazeCandidateActive[SourceIndex])
        {
            ++GazeCandidateExposureCounts[SourceIndex];
            if (bGazeCandidateSelectedDuringExposure[SourceIndex])
            {
                ++GazeCandidateSelectedCounts[SourceIndex];
            }
            bGazeCandidateActive[SourceIndex] = false;
            bGazeCandidateSelectedDuringExposure[SourceIndex] = false;
        }
    }
}

void ANPCCharacterBase::FinishGazeCandidateMeasurements()
{
    for (int32 SourceIndex = 1; SourceIndex < GazeSourceCount; ++SourceIndex)
    {
        if (bGazeCandidateActive[SourceIndex])
        {
            ++GazeCandidateExposureCounts[SourceIndex];
            if (bGazeCandidateSelectedDuringExposure[SourceIndex])
            {
                ++GazeCandidateSelectedCounts[SourceIndex];
            }
            bGazeCandidateActive[SourceIndex] = false;
            bGazeCandidateSelectedDuringExposure[SourceIndex] = false;
        }
    }
}

void ANPCCharacterBase::SubmitGazeMeasurement()
{
    if (!bGazeMeasurementRegistered)
    {
        return;
    }

    FPersonaGazeAggregate& Aggregate = SessionPersonaAggregates[PersonaToIndex(MyPersona)];
    ++Aggregate.NPCCount;
    for (int32 Index = 0; Index < GazeSourceCount; ++Index)
    {
        Aggregate.EventCounts[Index] += GazeMeasurementEventCounts[Index];
        Aggregate.Durations[Index] += GazeMeasurementDurations[Index];
        Aggregate.CandidateExposureCounts[Index] += GazeCandidateExposureCounts[Index];
        Aggregate.CandidateSelectedCounts[Index] += GazeCandidateSelectedCounts[Index];
        Aggregate.SocialOriginEventCounts[Index] += SocialOriginEventCounts[Index];
        Aggregate.SocialOriginDurations[Index] += SocialOriginDurations[Index];
    }
    Aggregate.SocialObserverSeconds += SocialObserverSeconds;
    Aggregate.SocialSeconds += MeasuredSocialSeconds;

    bGazeMeasurementRegistered = false;
    ActiveMeasuredNPCs = FMath::Max(0, ActiveMeasuredNPCs - 1);
    if (ActiveMeasuredNPCs == 0)
    {
        SaveGazeMeasurementSession();
    }
}

int32 ANPCCharacterBase::GetCurrentSocialMeasurementOrigin() const
{
    FName OriginDriver = NAME_None;
    if (IsValid(CurrentSocialInfluenceSource))
    {
        OriginDriver = CurrentSocialInfluenceSource->ObservableDriver;
    }
    if (OriginDriver.IsNone())
    {
        OriginDriver = CurrentSocialResponseDriver;
    }

    if (OriginDriver == FName(TEXT("Safety")))
    {
        return 1; // SocialSafety
    }
    if (OriginDriver == FName(TEXT("Stimulus")))
    {
        return 2; // SocialSound
    }
    if (OriginDriver == FName(TEXT("Info")) || OriginDriver == FName(TEXT("Habit")))
    {
        return 4; // SocialInformation
    }
    return 3; // SocialInterest and any generic social cue
}

void ANPCCharacterBase::DrawGaussianFieldHeatmap(const TArray<FThreatSource>& Threats)
{
    if (!bVisualizeGaussianField || Threats.Num() == 0 || !GetWorld())
    {
        return;
    }

    const float Now = GetWorld()->GetTimeSeconds();
    if (Now < NextGaussianFieldDrawTime)
    {
        return;
    }
    NextGaussianFieldDrawTime = Now + GaussianFieldRefreshInterval;

    const float SafeCellSize = FMath::Max(GaussianFieldCellSize, 50.0f);
    const float SafeExtent = FMath::Max(GaussianFieldExtent, SafeCellSize);
    const FVector ActorLocation = GetActorLocation();
    const float GroundZ = ActorLocation.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 8.0f;

    struct FGaussianFieldSample
    {
        FVector Location;
        float Score;
    };

    TArray<FGaussianFieldSample> Samples;
    float MaxScore = 0.0f;

    for (float X = -SafeExtent; X <= SafeExtent; X += SafeCellSize)
    {
        for (float Y = -SafeExtent; Y <= SafeExtent; Y += SafeCellSize)
        {
            const FVector SampleLocation(ActorLocation.X + X, ActorLocation.Y + Y, GroundZ);
            float CombinedScore = 0.0f;

            for (const FThreatSource& Source : Threats)
            {
                CombinedScore += CalculateGaussianScore(SampleLocation, Source);
            }

            Samples.Add({ SampleLocation, CombinedScore });
            MaxScore = FMath::Max(MaxScore, CombinedScore);
        }
    }

    if (MaxScore <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    const FVector TileExtent(SafeCellSize * 0.47f, SafeCellSize * 0.47f, 3.0f);
    const float LifeTime = GaussianFieldRefreshInterval * 1.5f;

    for (const FGaussianFieldSample& Sample : Samples)
    {
        const float NormalizedScore = Sample.Score / MaxScore;
        if (NormalizedScore < 0.02f)
        {
            continue;
        }

        DrawDebugSolidBox(GetWorld(), Sample.Location, TileExtent,
            GetGaussianHeatColor(NormalizedScore), false, LifeTime, 0);
    }

    DrawDebugString(GetWorld(), ActorLocation + FVector(0.0f, 0.0f, 140.0f),
        FString::Printf(TEXT("Gaussian Field: %s"), *GetName()), nullptr, FColor::White,
        LifeTime, false, 1.1f);
}

FColor ANPCCharacterBase::GetGaussianHeatColor(float NormalizedScore) const
{
    const float T = FMath::Clamp(NormalizedScore, 0.0f, 1.0f);

    if (T < 0.25f)
    {
        return FLinearColor::LerpUsingHSV(FLinearColor::Blue, FLinearColor(0.0f, 1.0f, 1.0f), T / 0.25f).ToFColor(false);
    }
    if (T < 0.5f)
    {
        return FLinearColor::LerpUsingHSV(FLinearColor(0.0f, 1.0f, 1.0f), FLinearColor::Green, (T - 0.25f) / 0.25f).ToFColor(false);
    }
    if (T < 0.75f)
    {
        return FLinearColor::LerpUsingHSV(FLinearColor::Green, FLinearColor::Yellow, (T - 0.5f) / 0.25f).ToFColor(false);
    }

    return FLinearColor::LerpUsingHSV(FLinearColor::Yellow, FLinearColor::Red, (T - 0.75f) / 0.25f).ToFColor(false);
}

