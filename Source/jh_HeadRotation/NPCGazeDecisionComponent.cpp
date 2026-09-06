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

UNPCGazeDecisionComponent::UNPCGazeDecisionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UNPCGazeDecisionComponent::HandleNPCBehavior(ANPCCharacterBase& NPC) const
{
    UpdatePlanMilestone(NPC);
    const FNPCPersonaGazeSettings Settings = GetPersonaSettings(NPC);
    const float FinalLookAtDuration = Settings.HoldTime * NPC.PersonalHoldTimeModifier;
    if (NPC.CurrentLookAtTimer > 0.0f)
    {
        NPC.CurrentLookAtTimer -= NPC.GetWorld()->GetDeltaSeconds();
    }

    TArray<FThreatSource> Candidates;
    NPC.DebugServerGMMWeight = 0.0f;
    NPC.DebugSocialGMMWeight = 0.0f;
    AppendServerCandidate(NPC, Candidates, Settings.Information);
    const bool bHasSafetyCandidate = AppendSafetyCandidates(NPC, Candidates, Settings.Safety, Settings.HoldTime);
    AppendSoundCandidate(NPC, Candidates, Settings.Stimulus, Settings.HoldTime);
    AppendSocialCandidate(NPC, Candidates, Settings.Information, Settings.Social);

    const FString Target = ApplySelectedGaze(NPC, Candidates, bHasSafetyCandidate, FinalLookAtDuration);
    NPC.GazeMotorComponent->UpdateReactiveMovement(NPC);
    PublishGazeAndDebug(NPC, Target);
}

float UNPCGazeDecisionComponent::CalculateGaussianScore(ANPCCharacterBase& NPC, FVector TestLocation, FThreatSource Source) const
{
    float DistanceSq = FVector::DistSquared(TestLocation, Source.Location);
    float SafeSpread = FMath::Max(Source.Spread, 1.0f);
    float Exponent = -DistanceSq / (2.0f * FMath::Square(SafeSpread));
    return Source.Intensity * FMath::Exp(Exponent);
}

FVector UNPCGazeDecisionComponent::CalculateGMMBestPeak(ANPCCharacterBase& NPC, const TArray<FThreatSource>& Threats) const
{
    if (Threats.Num() == 0) return NPC.GetActorLocation() + NPC.GetActorForwardVector() * 500.0f;

    FVector BestLocation = Threats[0].Location;
    float MaxTotalScore = -1.0f;

    for (const FThreatSource& Primary : Threats)
    {
        float TotalScoreAtThisPoint = 0.0f;
        for (const FThreatSource& Other : Threats)
        {
            TotalScoreAtThisPoint += NPC.CalculateGaussianScore(Primary.Location, Other);
        }

        if (TotalScoreAtThisPoint > MaxTotalScore)
        {
            MaxTotalScore = TotalScoreAtThisPoint;
            BestLocation = Primary.Location;
        }
    }
    return BestLocation;
}

void UNPCGazeDecisionComponent::UpdatePlanMilestone(ANPCCharacterBase& NPC) const
{
    // --- [1] 기존 로직: 이정표 도달 및 다음 타겟 갱신 (유지) ---
    if (NPC.CurrentGazeState == EGazeState::Normal && NPC.SharedGazeMilestones.IsValidIndex(NPC.CurrentGazeIndex))
    {
        float DistanceToMilestone = FVector::Dist(NPC.GetActorLocation(), NPC.SharedGazeMilestones[NPC.CurrentGazeIndex]->GetActorLocation());

        if (DistanceToMilestone < 250.0f)
        {
            if (NPC.LocalPlanList.IsValidIndex(NPC.CurrentGazeIndex))
            {
                NPC.CurrentServerTarget = NPC.LocalPlanList[NPC.CurrentGazeIndex].TargetObject;
                NPC.CurrentServerDriver = NPC.LocalPlanList[NPC.CurrentGazeIndex].Driver;
                NPC.CurrentServerPlanIndex = NPC.CurrentGazeIndex;
                // UpdateLookAtTarget은 아래에서 GMM 결과에 따라 통합 처리되므로 일단 이름만 갱신
            }
            NPC.CurrentGazeIndex++;
        }
    }
}

FNPCPersonaGazeSettings UNPCGazeDecisionComponent::GetPersonaSettings(const ANPCCharacterBase& NPC) const
{
    FNPCPersonaGazeSettings Settings;

    // =========================================================================
    // 5가지 시선 동기 구조 (Gaze Motives) 기반 파라미터 설정
    // =========================================================================
    switch (NPC.MyPersona)
    {
    case EPersonaType::CAUTIOUS:
        // [신중형 프로필]: 안전 동기 극대화, 자극 민감, 경로 정보 의존 낮음, 시선 유지 김
        Settings.Information = 20.0f;
        Settings.Safety = 160.0f;
        Settings.Stimulus = 100.0f;
        Settings.Social = 10.0f;
        Settings.HoldTime = 2.5f;
        break;

    case EPersonaType::RUSHER:
        // [조급형 프로필]: 경로 정보 동기 압도적, 안전/자극 동기 최소화, 시선 유지 짧음(휙휙 바꿈)
        Settings.Information = 120.0f;
        Settings.Safety = 60.0f;
        Settings.Stimulus = 20.0f;
        Settings.Social = 30.0f;
        Settings.HoldTime = 1.5f;
        break;

    case EPersonaType::SIGHTSEER:
        // [구경꾼 프로필]: 주변 자극/소리에 극도로 반응, 안전 평범, 경로 정보 평범, 시선 유지 중간
        Settings.Information = 40.0f;
        Settings.Safety = 80.0f;
        Settings.Stimulus = 150.0f;
        Settings.Social = 30.0f; // 주변 환경이나 타인을 두리번거리는 성향
        Settings.HoldTime = 1.8f;
        break;
    }

    return Settings;
}

void UNPCGazeDecisionComponent::AppendServerCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Information) const
{
    // A. 서버 플랜 (현재 보고 있어야 할 타겟) = 정보/경로 동기 반영
    if (!NPC.CurrentServerTarget.IsEmpty() && !NPC.CurrentServerTarget.Equals(TEXT("forward"), ESearchCase::IgnoreCase))
    {
        // 기존 UpdateLookAtTarget 로직을 활용해 타겟 액터의 위치를 산으로 추가
        FVector TargetGazeLocation;
        if (NPC.GetCurrentPlanGazeLocation(TargetGazeLocation))
        {
            // 획득한 진짜 시선 좌표(TargetGazeLocation)를 GMM 맵에 등록
            NPC.DebugServerGMMWeight = Motive_Information;
            ThreatMap.Add(FThreatSource(TargetGazeLocation, Motive_Information, 1000.0f, EGazeSource::Server));

        }
    }
}

bool UNPCGazeDecisionComponent::AppendSafetyCandidates(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Safety, float Motive_HoldTime) const
{
    // B. 범용 물리 Safety 자극. 차량 태그나 AIController의 bIsDanger 대신
    // 컴포넌트가 붙은 모든 대상을 이 NPC 기준으로 평가한다.
    TArray<USafetyStimulusComponent*> SafetyStimuli;
    NPC.PerceptionComponent->CollectSafetyStimuli(NPC, SafetyStimuli);

    bool bHasSafetyCandidate = false;
    const FVector ObserverLocation = NPC.GetActorLocation();
    const FVector ObserverVelocity = NPC.GetVelocity();

    for (USafetyStimulusComponent* Stimulus : SafetyStimuli)
    {
        AActor* StimulusActor = Stimulus ? Stimulus->GetOwner() : nullptr;
        if (!IsValid(StimulusActor) || StimulusActor == (&NPC))
        {
            continue;
        }

        const FVector StimulusLocation = Stimulus->GetStimulusLocation();
        const FVector ToObserver = ObserverLocation - StimulusLocation;
        const float Distance = ToObserver.Size();
        const float DetectionRadius = FMath::Max(NPC.SafetyDetectionRadius, 1.0f);
        if (Distance > DetectionRadius)
        {
            continue;
        }

        const FVector StimulusVelocity = Stimulus->GetStimulusVelocity();
        const float DangerousSpeed = FMath::Max(NPC.SafetyDangerousSpeed, 1.0f);
        const float ApproachSpeed = FVector::DotProduct(
            StimulusVelocity, ToObserver.GetSafeNormal());
        const float ProximityRisk = 1.0f - FMath::Clamp(Distance / DetectionRadius, 0.0f, 1.0f);
        const float ApproachRisk = FMath::Clamp(ApproachSpeed / DangerousSpeed, 0.0f, 1.0f);
        const float SpeedRisk = FMath::Clamp(StimulusVelocity.Size() / DangerousSpeed, 0.0f, 1.0f);

        // 앞으로 몇 초 동안 두 대상이 가장 가까워지는 거리를 예측한다.
        const FVector RelativePosition = StimulusLocation - ObserverLocation;
        const FVector RelativeVelocity = StimulusVelocity - ObserverVelocity;
        const float RelativeSpeedSq = RelativeVelocity.SizeSquared();
        float TimeToClosest = 0.0f;
        if (RelativeSpeedSq > KINDA_SMALL_NUMBER)
        {
            TimeToClosest = FMath::Clamp(
                -FVector::DotProduct(RelativePosition, RelativeVelocity) / RelativeSpeedSq,
                0.0f,
                NPC.SafetyPredictionHorizon);
        }
        const float ClosestDistance = (RelativePosition + RelativeVelocity * TimeToClosest).Size();
        const float CombinedCollisionRadius = Stimulus->GetCollisionRadius() +
            (NPC.GetCapsuleComponent() ? NPC.GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.0f);
        const float CollisionRisk = 1.0f - FMath::Clamp(
            ClosestDistance / FMath::Max(CombinedCollisionRadius, 1.0f), 0.0f, 1.0f);

        const float DangerScore = FMath::Clamp(
            ProximityRisk * 0.25f + ApproachRisk * 0.30f + SpeedRisk * 0.10f +
            CollisionRisk * 0.20f + Stimulus->GetMassFactor() * 0.05f +
            Stimulus->BaseDanger * 0.10f,
            0.0f, 1.0f);
        if (DangerScore < NPC.MinimumSafetyDangerScore)
        {
            continue;
        }

        bHasSafetyCandidate = true;
        // Non-linear salience keeps close/imminent threats competitive while
        // allowing the same-persona NPC farther away to retain its server gaze.
        const float CalculatedSafetyWeight = FMath::Pow(DangerScore, 1.5f) * FMath::Max(
            0.0f, Motive_Safety + NPC.PersonalThreatWeightBonus);
        float SafetyWeight = NPC.bForceSafetyWinnerForDebug
            ? NPC.ForcedSafetyGMMWeight
            : CalculatedSafetyWeight;
        const bool bSameObservedTarget = NPC.ActiveSafetyTarget.Get() == StimulusActor;
        const float SafetyObservationTime = NPC.GetWorld()->GetTimeSeconds() - NPC.VehicleDangerStartTime;
        if (!NPC.bForceSafetyWinnerForDebug && bSameObservedTarget &&
            ApproachSpeed <= 0.0f && SafetyObservationTime >= Motive_HoldTime)
        {
            const float TimePastHold = SafetyObservationTime - Motive_HoldTime;
            const float DecayAlpha = FMath::Clamp(TimePastHold / 2.0f, 0.0f, 1.0f);
            SafetyWeight *= FMath::Lerp(1.0f, 0.1f, DecayAlpha);
        }

        if (NPC.bEnableSafetyDebugLogging && NPC.GetWorld()->GetTimeSeconds() >= NPC.NextSafetyDebugLogTime)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[%s][SafetyDebug] Actor=%s Component=%s Distance=%.1f Velocity=%.1f Approach=%.1f Danger=%.3f CalculatedWeight=%.1f ForcedWeight=%.1f"),
                *NPC.GetName(), *GetNameSafe(StimulusActor), *Stimulus->GetTrackedComponentName(),
                Distance, StimulusVelocity.Size(),
                ApproachSpeed, DangerScore, CalculatedSafetyWeight, SafetyWeight);
        }
        ThreatMap.Add(FThreatSource(
            StimulusLocation,
            SafetyWeight,
            Stimulus->GetGaussianSpread(),
            EGazeSource::Safety,
            StimulusActor));

        if (NPC.bVisualizeGaussianField)
        {
            const float Radius = FMath::Clamp(SafetyWeight * 1.5f, 20.0f, 300.0f);
            DrawDebugSphere(NPC.GetWorld(), StimulusLocation, Radius, 16, FColor::Red, false,
                NPC.GaussianFieldRefreshInterval * 1.5f, 0, 2.0f);
        }
    }

    NPC.bIsDangerDetected = bHasSafetyCandidate;
    if (NPC.bEnableSafetyDebugLogging && NPC.GetWorld()->GetTimeSeconds() >= NPC.NextSafetyDebugLogTime)
    {
        if (!bHasSafetyCandidate)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[%s][SafetyDebug] No candidate passed range/threshold checks"), *NPC.GetName());
        }
        NPC.NextSafetyDebugLogTime = NPC.GetWorld()->GetTimeSeconds() + 1.0f;
    }

    return bHasSafetyCandidate;
}

void UNPCGazeDecisionComponent::AppendSoundCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Stimulus, float Motive_HoldTime) const
{
    // C. 소리 감지 (로직 유지)
    if (NPC.bIsSoundDetected)
    {
        // 소리가 최초 발생한 시점으로부터 현재 몇 초가 흘렀는지 계산
        float ElapsedTime = NPC.GetWorld()->GetTimeSeconds() - NPC.SoundDetectedTime;

        // [공식 1] 반응 지연 시간 계산 (성격별 딜레이 다르게 찢기)
        float BaseDelay = FMath::Max(0.02f, 0.5f - (Motive_Stimulus / 350.0f));
        float CurrentReactionDelay = BaseDelay + FMath::Abs(NPC.PersonalHoldTimeModifier * 0.4f);

        // [공식 2] 소리 유지 시간 계산 (성격별 바라보는 시간 다르게 찢기)
        float CurrentDecayDuration = FMath::Max(0.4f, Motive_HoldTime + (NPC.PersonalHoldTimeModifier * 0.6f));

        // 1단계: 딜레이 대기 중
        if (ElapsedTime < CurrentReactionDelay)
        {
            // 통과
        }
        // 2단계: 소리 주시 구간
        else if (ElapsedTime >= CurrentReactionDelay && ElapsedTime < (CurrentReactionDelay + CurrentDecayDuration))
        {
            float Progress = (ElapsedTime - CurrentReactionDelay) / CurrentDecayDuration;
            float DynamicStimulusWeight = Motive_Stimulus * (1.0f - Progress);
            FVector RandomizedSoundLocation = NPC.SensedSoundLocation + NPC.PersonalSoundLookOffset;

            // 오차가 적용된 나만의 시선 좌표를 GMM 계산기에 주입
            ThreatMap.Add(FThreatSource(RandomizedSoundLocation, DynamicStimulusWeight, 400.0f, EGazeSource::Sound));

            if (NPC.bVisualizeGaussianField)
            {
                const float Radius = FMath::Clamp(DynamicStimulusWeight * 1.5f, 20.0f, 300.0f);
                DrawDebugSphere(NPC.GetWorld(), RandomizedSoundLocation, Radius, 16, FColor::Yellow, false,
                    NPC.GaussianFieldRefreshInterval * 1.5f, 0, 2.0f);
            }

        }
        // 3단계: 흥미 종료
        else
        {
            NPC.bIsSoundDetected = false;
        }
    }
}

void UNPCGazeDecisionComponent::AppendSocialCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Information, float Motive_Social) const
{
    // D. Social cue. The observer's already-personalized response score becomes
    // a regular GMM candidate, so it competes with the existing server gaze.
    if (NPC.CurrentSocialInfluenceSource &&
        NPC.CurrentSocialInfluenceScore >= NPC.MinimumSocialInfluenceScore &&
        !NPC.CurrentSocialInfluenceTargetLocation.IsNearlyZero())
    {
        const bool bSocialLocked = NPC.GetWorld()->GetTimeSeconds() < NPC.SocialLockEndTime;
        const float SocialHoldBonus = bSocialLocked ? 35.0f : 0.0f;
        const int32 ContributingObserverCount = FMath::Clamp(
            NPC.CurrentSocialObserverCount, 1, FMath::Max(1, NPC.MaximumSocialGMMContributorCount));
        const float GroupSocialMotive = Motive_Social * ContributingObserverCount;
        float SocialCandidateWeight =
            GroupSocialMotive + (NPC.CurrentSocialInfluenceScore * 150.0f) + SocialHoldBonus;

        // Temporary Sightseer test rule: once at least three visible NPCs are
        // looking at the same place and the observation delay has completed,
        // make Social decisively stronger than the regular server-plan gaze.
        // Active safety or sound cues keep their normal priority.
        const bool bHasReactiveCandidate = ThreatMap.ContainsByPredicate(
            [](const FThreatSource& Candidate)
            {
                return Candidate.Source == EGazeSource::Safety ||
                    Candidate.Source == EGazeSource::Sound;
            });
        if (NPC.MyPersona == EPersonaType::SIGHTSEER &&
            NPC.CurrentSocialObserverCount >= 3 &&
            !bHasReactiveCandidate)
        {
            SocialCandidateWeight = FMath::Max(
                SocialCandidateWeight, Motive_Information + 50.0f);
        }
        NPC.DebugSocialGMMWeight = SocialCandidateWeight;

        ThreatMap.Add(FThreatSource(
            NPC.CurrentSocialInfluenceTargetLocation,
            SocialCandidateWeight,
            700.0f,
            EGazeSource::Social));
        if (NPC.bVisualizeGaussianField)
        {
            const float Radius = FMath::Clamp(SocialCandidateWeight * 1.5f, 20.0f, 300.0f);
            DrawDebugSphere(NPC.GetWorld(), NPC.CurrentSocialInfluenceTargetLocation, Radius, 16,
                FColor::Green, false, NPC.GaussianFieldRefreshInterval * 1.5f, 0, 2.0f);
        }
    }
}

FString UNPCGazeDecisionComponent::ApplySelectedGaze(ANPCCharacterBase& NPC, const TArray<FThreatSource>& ThreatMap, bool bHasSafetyCandidate, float FinalLookAtDuration) const
{
    FString TargetToLook;
    // --- [4] 최종 시선 위치 결정 ---
    FVector GMMTarget = NPC.CalculateGMMBestPeak(ThreatMap); // 지금 가던 길, 차, 소리 중 누가 제일 점수가 높은지 출력
    EGazeSource WinningSource = EGazeSource::None;
    AActor* WinningTargetActor = nullptr;
    float StrongestMatchingIntensity = -1.0f;
    for (const FThreatSource& Candidate : ThreatMap)
    {
        if (Candidate.Location.Equals(GMMTarget, 1.0f) && Candidate.Intensity > StrongestMatchingIntensity)
        {
            WinningSource = Candidate.Source;
            WinningTargetActor = Candidate.TargetActor;
            StrongestMatchingIntensity = Candidate.Intensity;
        }
    }

    NPC.UpdateGazeCandidateMeasurement(ThreatMap, WinningSource);

    if (NPC.bEnableSafetyDebugLogging &&
        NPC.GetWorld()->GetTimeSeconds() >= NPC.NextSafetyWinnerDebugLogTime)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[%s][SafetyDebug] Winner=%d Target=%s"),
            *NPC.GetName(), static_cast<int32>(WinningSource), *GetNameSafe(WinningTargetActor));
        NPC.NextSafetyWinnerDebugLogTime = NPC.GetWorld()->GetTimeSeconds() + 1.0f;
    }

    if (NPC.bVisualizeGaussianField)
    {
        NPC.DrawGaussianFieldHeatmap(ThreatMap);
    }

    {
        // 특별한 잠금 상태가 아니면 GMM 봉우리를 실시간 추적
        const EGazeSource PreviousGazeSource = NPC.CurrentGazeSource;
        NPC.CurrentGazeSource = WinningSource;
        NPC.LookAtLocation = GMMTarget;

        if (NPC.CurrentGazeSource == EGazeSource::Safety)
        {
            if (NPC.ActiveSafetyTarget.Get() != WinningTargetActor)
            {
                NPC.VehicleDangerStartTime = NPC.GetWorld()->GetTimeSeconds();
            }
            NPC.ActiveSafetyTarget = WinningTargetActor;
        }
        else if (!bHasSafetyCandidate)
        {
            NPC.ActiveSafetyTarget.Reset();
        }
        NPC.bIsApplyingSocialGaze = NPC.CurrentGazeSource == EGazeSource::Social;

        if (NPC.bIsApplyingSocialGaze)
        {
            NPC.ActivateOrExtendSocialGazeLock();
        }
        else if (PreviousGazeSource == EGazeSource::Social &&
            (NPC.CurrentGazeSource == EGazeSource::Safety || NPC.CurrentGazeSource == EGazeSource::Sound))
        {
            // A threat or a strong sound may interrupt a social hold. Server gaze
            // does not tear down the lock, preventing harmless target flicker.
            NPC.ReleaseSocialGazeLock(true);
        }

        if (NPC.CurrentGazeSource == EGazeSource::Safety)
        {
            TargetToLook = WinningTargetActor
                ? FString::Printf(TEXT("Safety:%s"), *WinningTargetActor->GetName())
                : TEXT("SafetyStimulus");
        }
        else if (NPC.CurrentGazeSource == EGazeSource::Sound)
        {
            TargetToLook = TEXT("SoundSource");
        }
        else if (NPC.bIsApplyingSocialGaze)
        {
            TargetToLook = FString::Printf(TEXT("Social:%s"), *NPC.CurrentSocialInfluenceTargetName);
        }
        else if (NPC.CurrentGazeSource == EGazeSource::Server)
        {
            TargetToLook = NPC.CurrentServerTarget;
        }
        else
        {
            TargetToLook = TEXT("forward");
        }

        // 새로운 대상으로 시선이 전환되는 시점에 성격별 유지 타이머 부여
        if (NPC.CurrentGazeSource != PreviousGazeSource || TargetToLook != NPC.LastAppliedTarget)
        {
            NPC.CurrentLookAtTimer = FinalLookAtDuration;
        }
    }
    NPC.bIsLooking = true;

    return TargetToLook;
}

void UNPCGazeDecisionComponent::PublishGazeAndDebug(ANPCCharacterBase& NPC, const FString& TargetToLook) const
{
    // --- [7] 상태 업데이트 및 로그 (기존 유지) ---
    if (TargetToLook != NPC.LastAppliedTarget)
    {
        NPC.LastAppliedTarget = TargetToLook;
        UE_LOG(LogTemp, Log, TEXT("[%s] Change State: %s"), *NPC.GetName(), *TargetToLook);
    }

    NPC.UpdateObservableBehavior(TargetToLook, NPC.LookAtLocation);

    if (NPC.DebugSocialGMMWeight > 0.0f)
    {
        if (NPC.CurrentGazeSource == EGazeSource::Social)
        {
            NPC.DebugSocialState = TEXT("Selected");
        }
        else if (NPC.CurrentGazeSource == EGazeSource::Safety || NPC.CurrentGazeSource == EGazeSource::Sound)
        {
            NPC.DebugSocialState = TEXT("InterruptedBySafetyOrSound");
        }
        else
        {
            NPC.DebugSocialState = TEXT("LostGMMCompetition");
        }
    }

    if (NPC.bVisualizeSocialObservation && NPC.GetWorld()->GetTimeSeconds() >= NPC.NextSocialDebugLogTime)
    {
        NPC.NextSocialDebugLogTime = NPC.GetWorld()->GetTimeSeconds() + 1.0f;
        UE_LOG(LogTemp, Log,
            TEXT("[%s][SocialDebug] State=%s Visible=%d SameTarget=%d Source=%s Score=%.3f Observe=%.2f/%.2f SocialGMM=%.1f ServerGMM=%.1f Winner=%d"),
            *NPC.GetName(), *NPC.DebugSocialState, NPC.VisibleSocialNPCs.Num(),
            NPC.DebugBestSocialObserverCount, *GetNameSafe(NPC.CurrentSocialInfluenceSource),
            NPC.DebugBestSocialScore, NPC.DebugSocialObservationElapsed,
            NPC.DebugSocialObservationRequired, NPC.DebugSocialGMMWeight,
            NPC.DebugServerGMMWeight, static_cast<int32>(NPC.CurrentGazeSource));
    }

    const bool bHasDebugGaze = NPC.CurrentGazeSource != EGazeSource::None;

    if (NPC.bIsLooking && bHasDebugGaze)
    {
        FColor GazeLineColor = FColor::Blue;

        if (NPC.CurrentGazeSource == EGazeSource::Safety)
        {
            GazeLineColor = FColor::Red;
        }
        else if (NPC.CurrentGazeSource == EGazeSource::Social)
        {
            GazeLineColor = FColor::Green;
        }
        else if (NPC.CurrentGazeSource == EGazeSource::Sound)
        {
            GazeLineColor = FColor::Yellow;
        }

        DrawDebugLine(NPC.GetWorld(), NPC.GetActorLocation() + FVector(0.0f, 0.0f, 60.0f),
            NPC.LookAtLocation, GazeLineColor, false, 0.05f, 0, 3.0f);
        DrawDebugSphere(NPC.GetWorld(), NPC.LookAtLocation, 18.0f, 12,
            GazeLineColor, false, 0.05f, 0, 2.0f);
    }
}

