#include "NPCGazeDecisionComponent.h"
#include "NPCCharacterBase.h"
#include "NPCPerceptionComponent.h"
#include "NPCGazeMotorComponent.h"
#include "Kismet/GameplayStatics.h"
#include "AIController.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "SafetyStimulusComponent.h"
#include "TimerManager.h"


float UNPCGazeDecisionComponent::GetObserverResponseWeight(const ANPCCharacterBase& NPC, FName ObservedDriver) const
{
    if (ObservedDriver.IsNone() || ObservedDriver == FName(TEXT("Habit")))
    {
        return 0.0f;
    }

    switch (NPC.MyPersona)
    {
    case EPersonaType::CAUTIOUS:
        if (ObservedDriver == FName(TEXT("Safety"))) return 1.0f;
        if (ObservedDriver == FName(TEXT("Stimulus"))) return 0.65f;
        if (ObservedDriver == FName(TEXT("Info"))) return 0.40f;
        if (ObservedDriver == FName(TEXT("Social"))) return 0.35f;
        return 0.25f;

    case EPersonaType::RUSHER:
        // A rusher only gives a limited response to an especially relevant risk cue.
        return ObservedDriver == FName(TEXT("Safety")) ? 0.30f : 0.12f;

    case EPersonaType::SIGHTSEER:
        // A sightseer is curious about what another NPC is attending to,
        // regardless of the source NPC's original motivation.
        return 0.95f;
    }

    return 0.0f;
}

FName UNPCGazeDecisionComponent::GetObserverResponseDriver(const ANPCCharacterBase& NPC, FName ObservedDriver) const
{
    switch (NPC.MyPersona)
    {
    case EPersonaType::SIGHTSEER:
        return FName(TEXT("Interest"));
    case EPersonaType::CAUTIOUS:
        return (ObservedDriver == FName(TEXT("Safety")) || ObservedDriver == FName(TEXT("Stimulus")))
            ? FName(TEXT("Safety")) : FName(TEXT("Info"));
    case EPersonaType::RUSHER:
        return ObservedDriver == FName(TEXT("Safety")) ? FName(TEXT("Safety")) : FName(TEXT("Habit"));
    }

    return NAME_None;
}

float UNPCGazeDecisionComponent::GetSocialObservationDelay(const ANPCCharacterBase& NPC) const
{
    float BaseDelay = NPC.CautiousSocialObservationDelay;
    switch (NPC.MyPersona)
    {
    case EPersonaType::SIGHTSEER:
        BaseDelay = NPC.SightseerSocialObservationDelay;
        break;
    case EPersonaType::RUSHER:
        BaseDelay = NPC.RusherSocialObservationDelay;
        break;
    case EPersonaType::CAUTIOUS:
    default:
        break;
    }

    return FMath::Max(0.0f,
        BaseDelay + NPC.PersonalSocialObservationDelayOffset * NPC.SocialObservationDelayRandomRange);
}

void UNPCGazeDecisionComponent::EvaluateSocialInfluence(ANPCCharacterBase& NPC) const
{
    NPC.DebugBestSocialObserverCount = 0;
    NPC.DebugBestSocialScore = 0.0f;
    NPC.DebugSocialObservationElapsed = 0.0f;
    NPC.DebugSocialObservationRequired = 0.0f;
    NPC.DebugSocialState = TEXT("Evaluating");
    NPC.CurrentSocialInfluenceScore = 0.0f;
    NPC.CurrentSocialInfluenceSource = nullptr;
    NPC.CurrentSocialInfluenceBehavior = ESocialObservableBehavior::None;
    NPC.CurrentSocialInfluenceTargetName = TEXT("none");
    NPC.CurrentSocialInfluenceTargetLocation = FVector::ZeroVector;
    NPC.CurrentSocialInfluenceSequence = 0;
    NPC.CurrentSocialResponseDriver = NAME_None;
    NPC.CurrentSocialObserverCount = 0;
    NPC.CurrentSocialLockRemaining = 0.0f;

    if (!NPC.bEnableSocialObservation || !NPC.GetWorld())
    {
        NPC.DebugSocialState = TEXT("Disabled");
        NPC.ReleaseSocialGazeLock(false);
        return;
    }

    const float Now = NPC.GetWorld()->GetTimeSeconds();
    // Once curiosity has been triggered, keep looking at the remembered place
    // for the full hold duration. The source NPC no longer needs to remain
    // visible or keep looking at it.
    const bool bHasStoredSocialLock = NPC.SocialLockEndTime > Now &&
        !NPC.LockedSocialTargetLocation.IsNearlyZero();

    if (bHasStoredSocialLock)
    {
        NPC.CurrentSocialInfluenceSource = IsValid(NPC.LockedSocialInfluenceSource)
            ? NPC.LockedSocialInfluenceSource : nullptr;
        NPC.CurrentSocialInfluenceTargetName = NPC.LockedSocialTargetName;
        NPC.CurrentSocialInfluenceTargetLocation = NPC.LockedSocialTargetLocation;
        NPC.CurrentSocialResponseDriver = NPC.LockedSocialResponseDriver;
        NPC.CurrentSocialInfluenceScore = NPC.LockedSocialInfluenceScore;
        NPC.CurrentSocialObserverCount = NPC.LockedSocialObserverCount;
        NPC.CurrentSocialInfluenceBehavior = NPC.LockedSocialInfluenceBehavior;
        NPC.CurrentSocialInfluenceSequence = NPC.LockedSocialInfluenceSequence;
        NPC.CurrentSocialLockRemaining = NPC.SocialLockEndTime - Now;
        NPC.DebugBestSocialObserverCount = NPC.LockedSocialObserverCount;
        NPC.DebugBestSocialScore = NPC.LockedSocialInfluenceScore;
        NPC.DebugSocialState = TEXT("Locked");
    }
    else if (NPC.SocialLockEndTime > 0.0f)
    {
        NPC.ReleaseSocialGazeLock(true);
    }

    if (NPC.VisibleSocialNPCs.Num() == 0 || Now < NPC.NextSocialSelectionAllowedTime)
    {
        NPC.DebugSocialState = NPC.VisibleSocialNPCs.Num() == 0
            ? TEXT("NoVisibleNPC") : TEXT("ReselectionCooldown");
        NPC.PendingSocialInfluenceSource = nullptr;
        NPC.PendingSocialInfluenceSequence = 0;
        NPC.PendingSocialInfluenceStartTime = 0.0f;
        return;
    }

    const float SafeRadius = FMath::Max(NPC.SocialObservationRadius, 1.0f);

    ANPCCharacterBase* BestSource = nullptr;
    float BestScore = 0.0f;
    int32 BestObserverCount = 0;

    for (ANPCCharacterBase* SourceNPC : NPC.VisibleSocialNPCs)
    {
        if (!IsValid(SourceNPC) || SourceNPC->CurrentObservableBehavior == ESocialObservableBehavior::None)
        {
            continue;
        }

        const float Distance = FVector::Dist2D(NPC.GetActorLocation(), SourceNPC->GetActorLocation());
        const float DistanceWeight = 1.0f - FMath::Clamp(Distance / SafeRadius, 0.0f, 1.0f);
        const float ObserverResponseWeight = NPC.GetObserverResponseWeight(SourceNPC->ObservableDriver);
        const float BehaviorAge = FMath::Max(0.0f, Now - SourceNPC->ObservableBehaviorChangedTime);
        const float PersistenceRatio = FMath::Clamp(
            BehaviorAge / FMath::Max(NPC.SocialPersistenceBuildTime, 0.1f), 0.0f, 1.0f);
        const float PersistenceMultiplier =
            1.0f + PersistenceRatio * NPC.SocialPersistenceMaximumBonus;

        int32 MatchingObserverCount = 0;
        for (ANPCCharacterBase* SupportingNPC : NPC.VisibleSocialNPCs)
        {
            if (!IsValid(SupportingNPC))
            {
                continue;
            }

            FVector SupportingTargetLocation = FVector::ZeroVector;
            if (SupportingNPC->CurrentObservableBehavior != ESocialObservableBehavior::None)
            {
                SupportingTargetLocation = SupportingNPC->ObservableTargetLocation;
            }
            else if (SupportingNPC->bIsApplyingSocialGaze)
            {
                // A copied gaze cannot become a new source, but it still counts
                // as one person visibly looking at the shared place.
                SupportingTargetLocation = SupportingNPC->CurrentSocialInfluenceTargetLocation;
            }

            if (SupportingTargetLocation.IsNearlyZero() ||
                SourceNPC->ObservableTargetLocation.IsNearlyZero())
            {
                continue;
            }

            const bool bTargetsNearby = FVector::DistSquared(
                SourceNPC->ObservableTargetLocation, SupportingTargetLocation) <=
                FMath::Square(NPC.SocialTargetMergeRadius);

            FVector SourceDirection =
                SourceNPC->ObservableTargetLocation - SourceNPC->GetActorLocation();
            FVector SupportingDirection =
                SupportingTargetLocation - SupportingNPC->GetActorLocation();
            SourceDirection.Z = 0.0f;
            SupportingDirection.Z = 0.0f;
            const float MinimumDirectionDot =
                FMath::Cos(FMath::DegreesToRadians(NPC.SocialTargetMergeAngle));
            const bool bDirectionsMatch = !SourceDirection.IsNearlyZero() &&
                !SupportingDirection.IsNearlyZero() &&
                FVector::DotProduct(SourceDirection.GetSafeNormal(), SupportingDirection.GetSafeNormal()) >=
                    MinimumDirectionDot;

            if (bTargetsNearby || bDirectionsMatch)
            {
                ++MatchingObserverCount;
            }
        }

        const float GroupStrengthMultiplier = 1.0f +
            (FMath::Max(0, MatchingObserverCount - 1) * NPC.SocialGroupStrengthPerObserver);

        const float SocialScore = FMath::Clamp(
            ObserverResponseWeight * SourceNPC->ObservableBehaviorSalience * DistanceWeight *
                GroupStrengthMultiplier * PersistenceMultiplier,
            0.0f,
            1.0f);

        if (SocialScore >= NPC.MinimumSocialInfluenceScore && SocialScore > BestScore)
        {
            BestScore = SocialScore;
            BestSource = SourceNPC;
            BestObserverCount = MatchingObserverCount;
        }
    }

    const bool bHasActiveLock = bHasStoredSocialLock;
    NPC.DebugBestSocialObserverCount = BestObserverCount;
    NPC.DebugBestSocialScore = BestScore;
    if (!IsValid(BestSource))
    {
        NPC.DebugSocialState = TEXT("NoValidCueOrBelowScore");
    }
    bool bBestMatchesLock = false;
    if (bHasActiveLock && IsValid(BestSource) &&
        !NPC.LockedSocialTargetLocation.IsNearlyZero() &&
        !BestSource->ObservableTargetLocation.IsNearlyZero())
    {
        const bool bSameSourceBehavior = BestSource == NPC.LockedSocialInfluenceSource &&
            BestSource->ObservableBehaviorSequence == NPC.LockedSocialInfluenceSequence;
        const bool bNearbyLockedTarget = FVector::DistSquared(
            NPC.LockedSocialTargetLocation, BestSource->ObservableTargetLocation) <=
            FMath::Square(NPC.SocialTargetMergeRadius);

        // If the same NPC publishes a new behavior sequence, it is always a new
        // cue even when its new target happens to be spatially close.
        bBestMatchesLock = BestSource == NPC.LockedSocialInfluenceSource
            ? bSameSourceBehavior
            : bNearbyLockedTarget;
    }
    // Once a social gaze is committed, ordinary social cues cannot replace it.
    // Safety and sound may still interrupt it later in HandleNPCBehavior.
    const bool bCanReplaceLock = !bHasActiveLock || bBestMatchesLock;

    // Require the same source and the same published gaze behavior to remain
    // stable before it can affect this NPC. Existing locks continue immediately.
    bool bObservationDelaySatisfied = bBestMatchesLock;
    if (IsValid(BestSource) && bCanReplaceLock && !bBestMatchesLock)
    {
        const bool bSamePendingCue = NPC.PendingSocialInfluenceSource == BestSource &&
            NPC.PendingSocialInfluenceSequence == BestSource->ObservableBehaviorSequence;

        if (!bSamePendingCue)
        {
            NPC.PendingSocialInfluenceSource = BestSource;
            NPC.PendingSocialInfluenceSequence = BestSource->ObservableBehaviorSequence;
            NPC.PendingSocialInfluenceStartTime = Now;
        }

        const float GroupDelayDivisor = 1.0f +
            FMath::Max(0, BestObserverCount - 1) * NPC.SocialGroupObservationDelayReduction;
        const float RequiredObservationDelay = FMath::Max(
            NPC.MinimumSocialObservationDelay,
            NPC.GetSocialObservationDelay() / GroupDelayDivisor);
        NPC.DebugSocialObservationElapsed = Now - NPC.PendingSocialInfluenceStartTime;
        NPC.DebugSocialObservationRequired = RequiredObservationDelay;
        NPC.DebugSocialState = TEXT("WaitingObservationDelay");
        bObservationDelaySatisfied =
            (Now - NPC.PendingSocialInfluenceStartTime) >= RequiredObservationDelay;
    }
    else if (!IsValid(BestSource) || !bCanReplaceLock)
    {
        NPC.PendingSocialInfluenceSource = nullptr;
        NPC.PendingSocialInfluenceSequence = 0;
        NPC.PendingSocialInfluenceStartTime = 0.0f;
    }

    if (IsValid(BestSource) && bCanReplaceLock && bObservationDelaySatisfied)
    {
        NPC.CurrentSocialInfluenceScore = BestScore;
        NPC.CurrentSocialInfluenceSource = BestSource;
        NPC.CurrentSocialInfluenceBehavior = BestSource->CurrentObservableBehavior;
        NPC.CurrentSocialInfluenceTargetName = BestSource->ObservableTargetName;
        NPC.CurrentSocialInfluenceTargetLocation = BestSource->ObservableTargetLocation;
        NPC.CurrentSocialInfluenceSequence = BestSource->ObservableBehaviorSequence;
        NPC.CurrentSocialResponseDriver = NPC.GetObserverResponseDriver(BestSource->ObservableDriver);
        NPC.CurrentSocialObserverCount = BestObserverCount;
        NPC.DebugSocialState = bBestMatchesLock ? TEXT("Locked") : TEXT("CandidateReady");

        NPC.PendingSocialInfluenceSource = nullptr;
        NPC.PendingSocialInfluenceSequence = 0;
        NPC.PendingSocialInfluenceStartTime = 0.0f;
    }

    // For the one NPC selected for inspection in Details, outline only the NPC
    // that is currently influencing it. A debug capsule works immediately and
    // does not require a custom-depth post-process material.
    if (NPC.bVisualizeSocialObservation && IsValid(NPC.CurrentSocialInfluenceSource))
    {
        const float DebugLifeTime = NPC.SocialObservationInterval * 1.5f;
        const UCapsuleComponent* SourceCapsule = NPC.CurrentSocialInfluenceSource->GetCapsuleComponent();
        const float CapsuleHalfHeight = SourceCapsule
            ? SourceCapsule->GetScaledCapsuleHalfHeight() + 8.0f
            : 100.0f;
        const float CapsuleRadius = SourceCapsule
            ? SourceCapsule->GetScaledCapsuleRadius() + 8.0f
            : 50.0f;

        DrawDebugCapsule(
            NPC.GetWorld(),
            NPC.CurrentSocialInfluenceSource->GetActorLocation(),
            CapsuleHalfHeight,
            CapsuleRadius,
            NPC.CurrentSocialInfluenceSource->GetActorQuat(),
            FColor::Orange,
            false,
            DebugLifeTime,
            0,
            4.0f);
    }
}

bool UNPCGazeDecisionComponent::AreSocialTargetsEquivalent(const ANPCCharacterBase& NPC, const ANPCCharacterBase* FirstNPC, const ANPCCharacterBase* SecondNPC) const
{
    if (!IsValid(FirstNPC) || !IsValid(SecondNPC) ||
        FirstNPC->ObservableTargetLocation.IsNearlyZero() ||
        SecondNPC->ObservableTargetLocation.IsNearlyZero())
    {
        return false;
    }

    if (FVector::DistSquared(FirstNPC->ObservableTargetLocation, SecondNPC->ObservableTargetLocation) <=
        FMath::Square(NPC.SocialTargetMergeRadius))
    {
        return true;
    }

    FVector FirstDirection = FirstNPC->ObservableTargetLocation - FirstNPC->GetActorLocation();
    FVector SecondDirection = SecondNPC->ObservableTargetLocation - SecondNPC->GetActorLocation();
    FirstDirection.Z = 0.0f;
    SecondDirection.Z = 0.0f;
    const float MinimumDot = FMath::Cos(FMath::DegreesToRadians(NPC.SocialTargetMergeAngle));
    return FVector::DotProduct(FirstDirection.GetSafeNormal(), SecondDirection.GetSafeNormal()) >= MinimumDot;
}

float UNPCGazeDecisionComponent::GetSocialBaseHoldDuration(const ANPCCharacterBase& NPC) const
{
    switch (NPC.MyPersona)
    {
    case EPersonaType::CAUTIOUS: return 1.5f;
    case EPersonaType::SIGHTSEER: return 3.0f;
    case EPersonaType::RUSHER: return 1.0f;
    }
    return 1.5f;
}

float UNPCGazeDecisionComponent::GetSocialObserverHoldBonus(const ANPCCharacterBase& NPC) const
{
    switch (NPC.MyPersona)
    {
    case EPersonaType::CAUTIOUS: return 0.15f;
    case EPersonaType::SIGHTSEER: return 0.35f;
    case EPersonaType::RUSHER: return 0.10f;
    }
    return 0.15f;
}

float UNPCGazeDecisionComponent::GetSocialMaximumHoldDuration(const ANPCCharacterBase& NPC) const
{
    switch (NPC.MyPersona)
    {
    case EPersonaType::CAUTIOUS: return 3.0f;
    case EPersonaType::SIGHTSEER: return 6.0f;
    case EPersonaType::RUSHER: return 2.0f;
    }
    return 3.0f;
}

void UNPCGazeDecisionComponent::ActivateOrExtendSocialGazeLock(ANPCCharacterBase& NPC) const
{
    if (!NPC.GetWorld() || !IsValid(NPC.CurrentSocialInfluenceSource))
    {
        return;
    }

    const float Now = NPC.GetWorld()->GetTimeSeconds();
    const bool bSameSourceChangedBehavior =
        NPC.LockedSocialInfluenceSource == NPC.CurrentSocialInfluenceSource &&
        NPC.LockedSocialInfluenceSequence != NPC.CurrentSocialInfluenceSequence;
    const bool bSameTarget = IsValid(NPC.LockedSocialInfluenceSource) &&
        !bSameSourceChangedBehavior &&
        !NPC.LockedSocialTargetLocation.IsNearlyZero() &&
        !NPC.CurrentSocialInfluenceTargetLocation.IsNearlyZero() &&
        FVector::DistSquared(NPC.LockedSocialTargetLocation, NPC.CurrentSocialInfluenceTargetLocation) <=
            FMath::Square(NPC.SocialTargetMergeRadius);

    if (!bSameTarget || NPC.SocialLockEndTime <= Now)
    {
        NPC.SocialLockStartTime = Now;
        NPC.LockedSocialObserverCount = 0;
    }

    NPC.LockedSocialInfluenceSource = NPC.CurrentSocialInfluenceSource;
    NPC.LockedSocialTargetLocation = NPC.CurrentSocialInfluenceTargetLocation;
    NPC.LockedSocialTargetName = NPC.CurrentSocialInfluenceTargetName;
    NPC.LockedSocialResponseDriver = NPC.CurrentSocialResponseDriver;
    NPC.LockedSocialInfluenceBehavior = NPC.CurrentSocialInfluenceBehavior;
    NPC.LockedSocialInfluenceScore = NPC.CurrentSocialInfluenceScore;
    NPC.LockedSocialInfluenceSequence = NPC.CurrentSocialInfluenceSequence;

    if (!bSameTarget || NPC.CurrentSocialObserverCount > NPC.LockedSocialObserverCount)
    {
        NPC.LockedSocialObserverCount = FMath::Max(1, NPC.CurrentSocialObserverCount);
        const float HoldDuration = FMath::Min(
            NPC.GetSocialBaseHoldDuration() +
                (NPC.LockedSocialObserverCount - 1) * NPC.GetSocialObserverHoldBonus() *
                    NPC.SocialGroupHoldBonusMultiplier,
            NPC.GetSocialMaximumHoldDuration());
        NPC.SocialLockEndTime = FMath::Max(NPC.SocialLockEndTime, NPC.SocialLockStartTime + HoldDuration);
    }

    NPC.CurrentSocialLockRemaining = FMath::Max(0.0f, NPC.SocialLockEndTime - Now);
}

void UNPCGazeDecisionComponent::ReleaseSocialGazeLock(ANPCCharacterBase& NPC, bool bApplyCooldown) const
{
    const float Now = NPC.GetWorld() ? NPC.GetWorld()->GetTimeSeconds() : 0.0f;
    if (bApplyCooldown)
    {
        NPC.NextSocialSelectionAllowedTime = FMath::Max(
            NPC.NextSocialSelectionAllowedTime, Now + NPC.SocialReselectionCooldown);
    }

    NPC.LockedSocialInfluenceSource = nullptr;
    NPC.LockedSocialTargetLocation = FVector::ZeroVector;
    NPC.LockedSocialTargetName = TEXT("none");
    NPC.LockedSocialResponseDriver = NAME_None;
    NPC.LockedSocialInfluenceBehavior = ESocialObservableBehavior::None;
    NPC.LockedSocialInfluenceScore = 0.0f;
    NPC.LockedSocialInfluenceSequence = 0;
    NPC.LockedSocialObserverCount = 0;
    NPC.SocialLockStartTime = 0.0f;
    NPC.SocialLockEndTime = 0.0f;
    NPC.CurrentSocialLockRemaining = 0.0f;
}

void UNPCGazeDecisionComponent::StartSocialGazeLogic(ANPCCharacterBase& NPC) const
{
    //리더 확인 상태로바꾸기
    if (NPC.CurrentGazeState != EGazeState::Normal) return;
    NPC.CurrentGazeState = EGazeState::CheckLeader;

    // 모든 팔로워가 0.5초 ~ 1.0초 사이의 서로 다른 시간에 리더의 타겟을 보도록 설정
    float RandomDelay = FMath::FRandRange(0.5f, 1.0f);

    //0.5-1.0초 뒤에(랜덤하게) 리더가 보는 곳 같이 보기
    NPC.GetWorldTimerManager().SetTimer(NPC.GazeTimerHandle, (&NPC), &ANPCCharacterBase::SwitchToFollowTarget, RandomDelay, false);
}

void UNPCGazeDecisionComponent::SwitchToFollowTarget(ANPCCharacterBase& NPC) const
{
    NPC.CurrentGazeState = EGazeState::FollowTarget;
    // 2초 동안 보고 원래대로 복귀
    NPC.GetWorldTimerManager().SetTimer(NPC.GazeTimerHandle, (&NPC), &ANPCCharacterBase::RestoreNormalGaze, 2.0f, false);
}

void UNPCGazeDecisionComponent::RestoreNormalGaze(ANPCCharacterBase& NPC) const
{
    //시선 상태 복구
    NPC.CurrentGazeState = EGazeState::Normal;
}
