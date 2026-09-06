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


// Navigation remains on the character; Eyes/Brain/Body only request reactions.

void ANPCCharacterBase::UpdateIntermediateWaypointRecovery(float DeltaTime)
{
    if (bHasReachedFinalDestination ||
        !TargetPoints.IsValidIndex(CurrentMoveIndex))
    {
        IntermediateWaypointStuckTime = 0.0f;
        return;
    }

    // Sound and safety deliberately stop movement. Do not treat those reactions
    // as a navigation failure.
    const bool bReactivePause = bIsPausedByDanger ||
        CurrentGazeSource == EGazeSource::Safety ||
        CurrentGazeSource == EGazeSource::Sound;
    const float Speed2D = GetVelocity().Size2D();
    AAIController* AIC = Cast<AAIController>(GetController());
    if (Speed2D >= 20.0f ||
        (AIC && AIC->GetMoveStatus() == EPathFollowingStatus::Moving))
    {
        bIntermediateWaypointMovementStarted = true;
        IntermediateWaypointStuckTime = 0.0f;
        return;
    }

    // A stationary NPC that has never started this waypoint may simply be
    // waiting for the scan/server plan. Do not make the recovery system start it.
    if (bReactivePause || !bIntermediateWaypointMovementStarted)
    {
        IntermediateWaypointStuckTime = 0.0f;
        return;
    }

    IntermediateWaypointStuckTime += DeltaTime;
    const float CurrentTime = GetWorld()->GetTimeSeconds();
    if (IntermediateWaypointStuckTime < 1.25f ||
        CurrentTime < NextIntermediateWaypointRetryTime)
    {
        return;
    }

    const float PreviousScale = ActivePathLateralOffsetScale;
    if (ActivePathLateralOffsetScale > 0.5f)
    {
        ActivePathLateralOffsetScale = 0.5f;
    }
    else if (ActivePathLateralOffsetScale > 0.0f)
    {
        ActivePathLateralOffsetScale = 0.0f;
    }

    // The Blueprint event asks for the current step again. GetNextStep then
    // rebuilds the same waypoint using the reduced lateral offset.
    RequestNextStep();
    UE_LOG(LogTemp, Warning,
        TEXT("[%s] Intermediate waypoint stalled; retrying index %d (lane scale %.2f -> %.2f)."),
        *GetName(), CurrentMoveIndex, PreviousScale, ActivePathLateralOffsetScale);

    IntermediateWaypointStuckTime = 0.0f;
    NextIntermediateWaypointRetryTime = CurrentTime + 1.5f;
}

void ANPCCharacterBase::EnterFinalDestinationState()
{
    bHasReachedFinalDestination = true;
    bIsLooking = false;
    LookAtLocation = FVector::ZeroVector;
    CurrentGazeSource = EGazeSource::None;
    CurrentGazeState = EGazeState::Normal;
    CurrentGazeIndex = 0;
    bIsSoundDetected = false;
    bIsDangerDetected = false;
    bIsPausedByDanger = false;
    bIsApplyingSocialGaze = false;
    CurrentSocialInfluenceSource = nullptr;
    CurrentSocialInfluenceScore = 0.0f;
    VisibleSocialNPCs.Reset();
    CurrentServerTarget = TEXT("forward");
    CurrentServerDriver = TEXT("Habit");
    LastAppliedTarget = TEXT("forward");
    UpdateObservableBehavior(TEXT("forward"), GetActorLocation());

    if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
    {
        if (bSoundBodyTurnActive)
        {
            MoveComp->bOrientRotationToMovement = bSavedOrientRotationToMovement;
            MoveComp->bUseControllerDesiredRotation = bSavedUseControllerDesiredRotation;
        }
        MoveComp->StopMovementImmediately();
    }
    bSoundBodyTurnActive = false;

    if (AAIController* AIC = Cast<AAIController>(GetController()))
    {
        AIC->StopMovement();
        AIC->ClearFocus(EAIFocusPriority::Gameplay);
    }
}

bool ANPCCharacterBase::TryBuildDestinationEntryPoint(
    UBoxComponent* DestinationBox, FVector& OutEntryPoint) const
{
    if (!DestinationBox)
    {
        return false;
    }

    UNavigationSystemV1* NavigationSystem =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavigationSystem)
    {
        return false;
    }

    const FTransform BoxTransform = DestinationBox->GetComponentTransform();
    const FVector BoxExtent = DestinationBox->GetScaledBoxExtent();
    const FVector LocalAgentPosition =
        BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
    const float CapsuleRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    const float SafeHalfWidth = FMath::Max(0.0f, BoxExtent.Y - CapsuleRadius - 25.0f);
    const float SafeHalfDepth = FMath::Max(0.0f, BoxExtent.X - CapsuleRadius - 25.0f);
    const float EntrySign = !FMath::IsNearlyZero(DestinationEntrySign)
        ? DestinationEntrySign
        : (LocalAgentPosition.X <= 0.0f ? -1.0f : 1.0f);

    const FVector LocalEntryPoint(
        EntrySign * SafeHalfDepth,
        FMath::Clamp(LocalAgentPosition.Y, -SafeHalfWidth, SafeHalfWidth),
        0.0f);
    const FVector RequestedEntryPoint = BoxTransform.TransformPositionNoScale(LocalEntryPoint);
    FNavLocation ProjectedEntryPoint;
    if (!NavigationSystem->ProjectPointToNavigation(
        RequestedEntryPoint, ProjectedEntryPoint, FVector(100.0f, 100.0f, 250.0f)))
    {
        return false;
    }

    OutEntryPoint = ProjectedEntryPoint.Location;
    return true;
}

bool ANPCCharacterBase::TryChooseAlternativeDestination(
    UBoxComponent* DestinationBox, FVector& OutDestination)
{
    if (!DestinationBox)
    {
        return false;
    }

    UNavigationSystemV1* NavigationSystem =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavigationSystem)
    {
        return false;
    }

    TArray<AActor*> OtherNPCActors;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), StaticClass(), OtherNPCActors);

    const FTransform BoxTransform = DestinationBox->GetComponentTransform();
    const FVector BoxExtent = DestinationBox->GetScaledBoxExtent();
    const float CapsuleRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    const float UseAreaScale = FMath::Clamp(DestinationUseAreaScale, 0.25f, 1.0f);
    const float SafeHalfWidth = FMath::Max(
        0.0f, BoxExtent.Y * UseAreaScale - CapsuleRadius - 25.0f);
    const float SafeHalfDepth = FMath::Max(
        0.0f, BoxExtent.X * UseAreaScale - CapsuleRadius - 25.0f);
    const float EntrySign = !FMath::IsNearlyZero(DestinationEntrySign)
        ? DestinationEntrySign
        : 1.0f;
    if (PersonalDestinationSpacingScale <= 0.0f)
    {
        FRandomStream SpacingRandom(GetUniqueID());
        PersonalDestinationSpacingScale = SpacingRandom.FRandRange(2.0f, 2.6f);
    }
    const float RequiredSpacing = CapsuleRadius * PersonalDestinationSpacingScale;
    const FVector PathStart = bHasAssignedDestinationEntry
        ? IndividualDestinationEntryPoint
        : GetActorLocation();
    const FVector LocalEntryPoint = BoxTransform.InverseTransformPositionNoScale(PathStart);

    FRandomStream RandomStream(HashCombine(GetUniqueID(), ++DestinationSelectionAttempt));
    const float SearchHalfWidths[] =
    {
        FMath::Min(150.0f, SafeHalfWidth),
        FMath::Min(300.0f, SafeHalfWidth),
        SafeHalfWidth * 2.0f
    };
    const float MaximumTurnAngles[] = { 35.0f, 55.0f, 180.0f };
    const int32 AttemptsPerStage = 32;

    for (int32 SearchStage = 0; SearchStage < UE_ARRAY_COUNT(SearchHalfWidths); ++SearchStage)
    {
        bool bFoundStageCandidate = false;
        float BestStageScore = TNumericLimits<float>::Max();
        FVector BestStageDestination = FVector::ZeroVector;

        for (int32 Attempt = 0; Attempt < AttemptsPerStage; ++Attempt)
        {
            const float SearchHalfWidth = SearchHalfWidths[SearchStage];
            FVector LocalCandidate(
                RandomStream.FRandRange(-SafeHalfDepth, SafeHalfDepth),
                FMath::Clamp(
                    LocalEntryPoint.Y + RandomStream.FRandRange(-SearchHalfWidth, SearchHalfWidth),
                    -SafeHalfWidth, SafeHalfWidth),
                0.0f);

            // Keep the nearest 25% of the entrance clear for incoming agents.
            if (EntrySign * LocalCandidate.X > SafeHalfDepth * 0.5f)
            {
                continue;
            }

            const FVector LocalTravel = LocalCandidate - LocalEntryPoint;
            const float ForwardDistance = -EntrySign * LocalTravel.X;
            const float LateralDistance = FMath::Abs(LocalTravel.Y);
            if (ForwardDistance <= 0.0f)
            {
                continue;
            }
            const float TurnAngle = FMath::RadiansToDegrees(
                FMath::Atan2(LateralDistance, FMath::Max(ForwardDistance, 1.0f)));
            if (TurnAngle > MaximumTurnAngles[SearchStage])
            {
                continue;
            }

            const FVector RequestedCandidate = BoxTransform.TransformPositionNoScale(LocalCandidate);
            FNavLocation ProjectedCandidate;
            if (!NavigationSystem->ProjectPointToNavigation(
                RequestedCandidate, ProjectedCandidate, FVector(100.0f, 100.0f, 250.0f)))
            {
                continue;
            }

            const FVector Candidate = ProjectedCandidate.Location;
            const FVector ProjectedLocal = BoxTransform.InverseTransformPositionNoScale(Candidate);
            if (FMath::Abs(ProjectedLocal.X) > SafeHalfDepth ||
                FMath::Abs(ProjectedLocal.Y) > SafeHalfWidth ||
                EntrySign * ProjectedLocal.X > SafeHalfDepth * 0.5f)
            {
                continue;
            }

            UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
                GetWorld(), PathStart, Candidate, this);
            if (!Path || !Path->IsValid() || Path->IsPartial())
            {
                continue;
            }

            bool bPositionOccupied = false;
            float NearestNPCDistance = TNumericLimits<float>::Max();
            for (AActor* OtherActor : OtherNPCActors)
            {
                const ANPCCharacterBase* OtherNPC = Cast<ANPCCharacterBase>(OtherActor);
                if (!OtherNPC || OtherNPC == this)
                {
                    continue;
                }

                const float ActorDistance =
                    FVector::Dist2D(OtherNPC->GetActorLocation(), Candidate);
                NearestNPCDistance = FMath::Min(NearestNPCDistance, ActorDistance);
                if (ActorDistance < RequiredSpacing ||
                    (OtherNPC->bHasAssignedDestination &&
                        FVector::Dist2D(OtherNPC->IndividualDestination, Candidate) < RequiredSpacing))
                {
                    bPositionOccupied = true;
                    break;
                }
            }

            if (bPositionOccupied)
            {
                continue;
            }

            const float CandidateScore =
                FVector::Dist2D(PathStart, Candidate) +
                LateralDistance * 3.0f +
                TurnAngle * 5.0f -
                FMath::Min(NearestNPCDistance, 600.0f) * 0.2f +
                RandomStream.FRandRange(0.0f, 80.0f);
            if (CandidateScore < BestStageScore)
            {
                BestStageScore = CandidateScore;
                BestStageDestination = Candidate;
                bFoundStageCandidate = true;
            }
        }

        if (bFoundStageCandidate)
        {
            OutDestination = BestStageDestination;
            return true;
        }
    }

    return false;
}

bool ANPCCharacterBase::StartFinalAdvance(UBoxComponent* DestinationBox)
{
    if (bHasStartedFinalAdvance || !DestinationBox)
    {
        return false;
    }

    UNavigationSystemV1* NavigationSystem =
        FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
    if (!NavigationSystem)
    {
        return false;
    }

    const FTransform BoxTransform = DestinationBox->GetComponentTransform();
    const FVector BoxExtent = DestinationBox->GetScaledBoxExtent();
    const float CapsuleRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    const float UseAreaScale = FMath::Clamp(DestinationUseAreaScale, 0.25f, 1.0f);
    const float SafeHalfDepth = FMath::Max(
        0.0f, BoxExtent.X * UseAreaScale - CapsuleRadius - 25.0f);
    const float SafeHalfWidth = FMath::Max(
        0.0f, BoxExtent.Y * UseAreaScale - CapsuleRadius - 25.0f);
    const float EntrySign = !FMath::IsNearlyZero(DestinationEntrySign)
        ? DestinationEntrySign
        : 1.0f;

    const FVector LocalPrimaryDestination =
        BoxTransform.InverseTransformPositionNoScale(IndividualDestination);
    FVector LocalFinalDestination = LocalPrimaryDestination;
    LocalFinalDestination.X = FMath::Clamp(
        LocalPrimaryDestination.X - EntrySign * 200.0f,
        -SafeHalfDepth,
        SafeHalfDepth);
    LocalFinalDestination.Y = FMath::Clamp(
        LocalPrimaryDestination.Y,
        -SafeHalfWidth,
        SafeHalfWidth);

    const float AvailableAdvance =
        FMath::Abs(LocalFinalDestination.X - LocalPrimaryDestination.X);
    if (AvailableAdvance < 50.0f)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[%s] DestinationBox has no room for the final 200 cm advance."),
            *GetName());
        return false;
    }

    const FVector RequestedFinalDestination =
        BoxTransform.TransformPositionNoScale(LocalFinalDestination);
    FNavLocation ProjectedFinalDestination;
    if (!NavigationSystem->ProjectPointToNavigation(
        RequestedFinalDestination,
        ProjectedFinalDestination,
        FVector(100.0f, 100.0f, 250.0f)))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[%s] Could not project the final 200 cm advance to NavMesh."),
            *GetName());
        return false;
    }

    IndividualDestination = ProjectedFinalDestination.Location;
    bHasStartedFinalAdvance = true;
    DestinationStuckTime = 0.0f;

    if (AAIController* AIC = Cast<AAIController>(GetController()))
    {
        AIC->MoveToLocation(IndividualDestination, 75.0f, false, true, true, false, nullptr, true);
    }

    UE_LOG(LogTemp, Log,
        TEXT("[%s] Primary destination reached; advancing %.0f cm before stopping."),
        *GetName(), AvailableAdvance);
    return true;
}

void ANPCCharacterBase::UpdateFinalDestinationNavigation(float DeltaTime)
{
    if (bHasReachedFinalDestination || !DestPoint ||
        CurrentMoveIndex < TargetPoints.Num())
    {
        DestinationStuckTime = 0.0f;
        return;
    }

    UBoxComponent* DestinationBox = DestPoint->FindComponentByClass<UBoxComponent>();
    if (!DestinationBox)
    {
        return;
    }

    const float CurrentTime = GetWorld()->GetTimeSeconds();
    if (!bHasAssignedDestinationEntry)
    {
        const FTransform BoxTransform = DestinationBox->GetComponentTransform();
        const FVector LocalPosition =
            BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
        if (FMath::IsNearlyZero(DestinationEntrySign))
        {
            DestinationEntrySign = LocalPosition.X <= 0.0f ? -1.0f : 1.0f;
        }

        if (TryBuildDestinationEntryPoint(DestinationBox, IndividualDestinationEntryPoint))
        {
            bHasAssignedDestinationEntry = true;
        }
        else
        {
            return;
        }
    }

    if (!bHasAssignedDestination)
    {
        if (CurrentTime >= NextDestinationRepathTime)
        {
            const FTransform BoxTransform = DestinationBox->GetComponentTransform();
            const FVector LocalPosition =
                BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
            if (FMath::IsNearlyZero(DestinationEntrySign))
            {
                DestinationEntrySign = LocalPosition.X <= 0.0f ? -1.0f : 1.0f;
            }

            FVector AvailableSlot;
            if (TryChooseAlternativeDestination(DestinationBox, AvailableSlot))
            {
                IndividualDestination = AvailableSlot;
                bHasAssignedDestination = true;
                if (AAIController* AIC = Cast<AAIController>(GetController()))
                {
                    const FVector FirstDestination = bHasReachedDestinationEntry
                        ? IndividualDestination
                        : IndividualDestinationEntryPoint;
                    AIC->MoveToLocation(FirstDestination, 75.0f, false, true, true, false, nullptr, true);
                }
            }
            NextDestinationRepathTime = CurrentTime + 1.0f;
        }
        return;
    }

    if (!bHasReachedDestinationEntry)
    {
        const float DistanceToEntry =
            FVector::Dist2D(GetActorLocation(), IndividualDestinationEntryPoint);
        if (DistanceToEntry <= 120.0f)
        {
            bHasReachedDestinationEntry = true;
            if (AAIController* AIC = Cast<AAIController>(GetController()))
            {
                AIC->MoveToLocation(IndividualDestination, 75.0f, false, true, true, false, nullptr, true);
            }
        }
        return;
    }

    const FTransform BoxTransform = DestinationBox->GetComponentTransform();
    const FVector BoxExtent = DestinationBox->GetScaledBoxExtent();
    const FVector LocalPosition =
        BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
    const bool bInsideBox =
        FMath::Abs(LocalPosition.X) <= BoxExtent.X &&
        FMath::Abs(LocalPosition.Y) <= BoxExtent.Y &&
        FMath::Abs(LocalPosition.Z) <=
            BoxExtent.Z + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

    if (bInsideBox)
    {
        bHasEnteredDestinationBox = true;
    }

    const bool bReactivePause =
        CurrentGazeSource == EGazeSource::Safety || CurrentGazeSource == EGazeSource::Sound;
    const float Speed2D = GetVelocity().Size2D();
    const float RemainingDistance = FVector::Dist2D(GetActorLocation(), IndividualDestination);
    if (bHasEnteredDestinationBox && RemainingDistance <= 100.0f && Speed2D < 30.0f)
    {
        if (!bHasStartedFinalAdvance && StartFinalAdvance(DestinationBox))
        {
            return;
        }
        EnterFinalDestinationState();
        DestinationStuckTime = 0.0f;
        return;
    }
    if (!bReactivePause && RemainingDistance > 150.0f && Speed2D < 20.0f)
    {
        DestinationStuckTime += DeltaTime;
    }
    else
    {
        DestinationStuckTime = 0.0f;
    }

    if (DestinationStuckTime < 1.0f || CurrentTime < NextDestinationRepathTime)
    {
        return;
    }

    if (bHasStartedFinalAdvance)
    {
        if (AAIController* AIC = Cast<AAIController>(GetController()))
        {
            AIC->MoveToLocation(IndividualDestination, 75.0f, false, true, true, false, nullptr, true);
        }
        DestinationStuckTime = 0.0f;
        NextDestinationRepathTime = CurrentTime + 1.5f;
        return;
    }

    FVector AlternativeDestination;
    if (TryChooseAlternativeDestination(DestinationBox, AlternativeDestination) &&
        !AlternativeDestination.Equals(IndividualDestination, 50.0f))
    {
        IndividualDestination = AlternativeDestination;
        if (AAIController* AIC = Cast<AAIController>(GetController()))
        {
            AIC->MoveToLocation(IndividualDestination, 75.0f, false, true, true, false, nullptr, true);
        }
        UE_LOG(LogTemp, Log, TEXT("[%s] Destination blocked; reassigned inside Box to %s"),
            *GetName(), *IndividualDestination.ToString());
    }

    DestinationStuckTime = 0.0f;
    NextDestinationRepathTime = CurrentTime + 1.5f;
}

FVector ANPCCharacterBase::GetNextMoveLocation(const TArray<AActor*>& InTargetPoints)
{
    if (InTargetPoints.Num() == 0) return GetActorLocation();

    if (CurrentMoveIndex >= InTargetPoints.Num())
    {
        // The route is complete. Do not wrap to waypoint zero, since that sends
        // an agent that reached the end all the way back to the route start.
        return GetActorLocation();
    }

    FVector NextPos = InTargetPoints[CurrentMoveIndex]->GetActorLocation();
    CurrentMoveIndex++;

    return NextPos;
}

FVector ANPCCharacterBase::GetNextStep(const TArray<FPlanItem>& PlanList, const TArray<AActor*>& PathPoints)
{
    if (bHasReachedFinalDestination)
    {
        return GetActorLocation();
    }

    if (DestPoint == nullptr)
    {
        UE_LOG(LogTemp, Error, TEXT("[%s] Destpoint is not setted! Stay."), *GetName());
        return GetActorLocation(); 
    }
    // 갈 곳이 없으면 제자리
    if (PathPoints.Num() == 0 && !DestPoint) return GetActorLocation();

    // 서버 플랜(PathPoints)에 따라 이동 위치 결정 
    // Use a personal Box entry instead of the final shared waypoint so that
    // parallel spawn lanes do not collapse into one central point.
    if (PathPoints.Num() > 0 && CurrentMoveIndex == PathPoints.Num() - 1)
    {
        CurrentMoveIndex = PathPoints.Num();
    }

    if (PathPoints.IsValidIndex(CurrentMoveIndex))
    {
        // 이제 직접 UpdateLookAtTarget을 부르지 않고, 타겟 이름만 기억합니다.
        if (PlanList.IsValidIndex(CurrentMoveIndex))
        {
            CurrentServerTarget = PlanList[CurrentMoveIndex].TargetObject;
            CurrentServerDriver = PlanList[CurrentMoveIndex].Driver;
            CurrentServerPlanIndex = CurrentMoveIndex;
        }

        AActor* CurrentTargetActor = PathPoints[CurrentMoveIndex];
        if (!IsValid(CurrentTargetActor))
        {
            CurrentMoveIndex++;
            return GetNextStep(PlanList, PathPoints);
        }

        FVector NextTargetLoc = CurrentTargetActor->GetActorLocation();
        FVector PathForward = FVector::ZeroVector;
        if (CurrentMoveIndex > 0 && IsValid(PathPoints[CurrentMoveIndex - 1]))
        {
            PathForward = (CurrentTargetActor->GetActorLocation()
                - PathPoints[CurrentMoveIndex - 1]->GetActorLocation()).GetSafeNormal2D();
        }
        else if (PathPoints.IsValidIndex(CurrentMoveIndex + 1)
            && IsValid(PathPoints[CurrentMoveIndex + 1]))
        {
            PathForward = (PathPoints[CurrentMoveIndex + 1]->GetActorLocation()
                - CurrentTargetActor->GetActorLocation()).GetSafeNormal2D();
        }

        if (bHasPathLateralOffset && !PathForward.IsNearlyZero())
        {
            const FVector PathRight = FVector::CrossProduct(FVector::UpVector, PathForward);
            NextTargetLoc += PathRight *
                (PathLateralOffset * ActivePathLateralOffsetScale);

            if (UNavigationSystemV1* NavigationSystem =
                FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
            {
                FNavLocation ProjectedPersonalWaypoint;
                if (NavigationSystem->ProjectPointToNavigation(
                    NextTargetLoc, ProjectedPersonalWaypoint, FVector(100.0f, 100.0f, 250.0f)))
                {
                    NextTargetLoc = ProjectedPersonalWaypoint.Location;
                }
            }
        }

        const float DistanceToTarget = FVector::Dist2D(GetActorLocation(), NextTargetLoc);
        // 중간 경로에서도 살짝 옆으로 분산 (반경 50cm)
        //NextTargetLoc.X += FMath::FRandRange(-50.0f, 50.0f);
        //NextTargetLoc.Y += FMath::FRandRange(-50.0f, 50.0f);

        // TD_Dest 도착 확인
        //if (CurrentTargetActor->GetName().Contains(TEXT("TD_Dest"), ESearchCase::IgnoreCase))
        //{
            //if (DistanceToDest < 250.0f)
            //{
                //AAIController* AIC = Cast<AAIController>(GetController());
                //if (AIC) AIC->StopMovement();
                //CurrentServerTarget = TEXT("forward"); // 다 왔으면 정면 보기
                //return GetActorLocation();
            //}
            //return CurrentTargetActor->GetActorLocation();
        //}

        // Advance only after reaching this waypoint. The old code skipped every
        // waypoint and sent all spawned NPCs directly to the final destination.
        const bool bPassedWaypoint = !PathForward.IsNearlyZero()
            && FVector::DotProduct(GetActorLocation() - NextTargetLoc, PathForward) >= 0.0f;
        if (DistanceToTarget <= 200.0f || bPassedWaypoint)
        {
            CurrentMoveIndex++;
            // Try the NPC's normal personal lane again on the next segment.
            // If that segment is also narrow, the recovery watchdog will reduce
            // it again without permanently removing crowd separation.
            ActivePathLateralOffsetScale = 1.0f;
            IntermediateWaypointStuckTime = 0.0f;
            bIntermediateWaypointMovementStarted = false;
            return GetNextStep(PlanList, PathPoints);
        }
        else
        {
            return NextTargetLoc;
        }
    }

    // 바로 목적지로 향하도록
    if (DestPoint)
    {
        // [수정] 목적지 근처에서도 시선은 서버 플랜의 마지막 타겟 혹은 정면 유지
        if (PlanList.Num() > 0 && PlanList.IsValidIndex(PlanList.Num() - 1))
        {
            CurrentServerTarget = PlanList.Last().TargetObject;
            CurrentServerDriver = PlanList.Last().Driver;
            CurrentServerPlanIndex = PlanList.Num() - 1;
        }
        else
        {
            CurrentServerTarget = TEXT("forward");
            CurrentServerDriver = TEXT("Habit");
            CurrentServerPlanIndex = INDEX_NONE;
        }
        // 목적지 도착 시 겹치지 않게 '개별 랜덤 목적지' 사용
        // Box-only destination: preserve each NPC's lateral approach line.

        UBoxComponent* DestinationBox = DestPoint->FindComponentByClass<UBoxComponent>();
        if (!DestinationBox)
        {
            if (!bLoggedMissingDestinationBox)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("[%s] DestPoint [%s] requires a BoxComponent."),
                    *GetName(), *GetNameSafe(DestPoint));
                bLoggedMissingDestinationBox = true;
            }
            return GetActorLocation();
        }

        if (!bHasAssignedDestination)
        {
            const FTransform BoxTransform = DestinationBox->GetComponentTransform();
            const FVector LocalAgentPosition =
                BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
            DestinationEntrySign = LocalAgentPosition.X <= 0.0f ? -1.0f : 1.0f;
            if (TryBuildDestinationEntryPoint(DestinationBox, IndividualDestinationEntryPoint))
            {
                bHasAssignedDestinationEntry = true;
            }
            if (!bHasAssignedDestinationEntry)
            {
                UE_LOG(LogTemp, Warning,
                    TEXT("[%s] Could not build a personal entry point for Box [%s]."),
                    *GetName(), *GetNameSafe(DestinationBox));
                return GetActorLocation();
            }
            if (TryChooseAlternativeDestination(DestinationBox, IndividualDestination))
            {
                bHasAssignedDestination = true;
            }

            if (!bHasAssignedDestination)
            {
                if (!bLoggedMissingDestinationBox)
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[%s] No free random NavMesh destination is currently available inside Box [%s]."),
                        *GetName(), *GetNameSafe(DestinationBox));
                    bLoggedMissingDestinationBox = true;
                }
                return GetActorLocation();
            }
            bLoggedMissingDestinationBox = false;
        }

        const FTransform BoxTransform = DestinationBox->GetComponentTransform();
        const FVector BoxExtent = DestinationBox->GetScaledBoxExtent();
        const FVector LocalPosition =
            BoxTransform.InverseTransformPositionNoScale(GetActorLocation());
        const bool bInsideDestinationBox =
            FMath::Abs(LocalPosition.X) <= BoxExtent.X &&
            FMath::Abs(LocalPosition.Y) <= BoxExtent.Y &&
            FMath::Abs(LocalPosition.Z) <=
                BoxExtent.Z + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if (!bHasReachedDestinationEntry && bHasAssignedDestinationEntry)
        {
            const float DistToEntry =
                FVector::Dist2D(GetActorLocation(), IndividualDestinationEntryPoint);
            if (DistToEntry <= 120.0f || bInsideDestinationBox)
            {
                bHasReachedDestinationEntry = true;
            }
            else
            {
                return IndividualDestinationEntryPoint;
            }
        }
        if (bInsideDestinationBox)
        {
            bHasEnteredDestinationBox = true;
            const float DistToSlot = FVector::Dist2D(GetActorLocation(), IndividualDestination);
            if (DistToSlot <= 100.0f && GetVelocity().Size2D() < 30.0f)
            {
                if (!bHasStartedFinalAdvance && StartFinalAdvance(DestinationBox))
                {
                    return IndividualDestination;
                }
                AAIController* AIC = Cast<AAIController>(GetController());
                if (AIC) AIC->StopMovement();
                GetCharacterMovement()->StopMovementImmediately();
                EnterFinalDestinationState();
                return GetActorLocation();
            }
        }
        return IndividualDestination;
    }
    // 마지막 지점 반환
    return PathPoints.Num() > 0 ? PathPoints.Last()->GetActorLocation() : GetActorLocation();
}

