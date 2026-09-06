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

ANPCCharacterBase::ANPCCharacterBase()
{
    PrimaryActorTick.bCanEverTick = true;

    PerceptionComponent = CreateDefaultSubobject<UNPCPerceptionComponent>(TEXT("Eyes"));
    GazeDecisionComponent = CreateDefaultSubobject<UNPCGazeDecisionComponent>(TEXT("Brain"));
    GazeMotorComponent = CreateDefaultSubobject<UNPCGazeMotorComponent>(TEXT("Body"));
    LookAtLocation = FVector::ZeroVector;
    bIsLooking = false;
    // [ADDED] 기본값 초기화
    InitializationType = ENPCInitializationType::Reference;
    CurrentGazeState = EGazeState::Normal;
    bLastLeaderDangerState = false;

    CurrentGazeIndex = 0;
    PersonalSocialObservationDelayOffset = FMath::FRandRange(-1.0f, 1.0f);

    PersonalHoldTimeModifier = FMath::FRandRange(-0.4f, 0.4f);   // 원래 주시 시간에서 ±0.4초 오프셋
    // Keep same-persona Safety tests controlled. Distance and motion, rather
    // than a hidden random bonus, should explain differences between NPCs.
    PersonalThreatWeightBonus = 0.0f;
    CurrentLookAtTimer = 0.0f; // 매 틱 깎아 나갈 타이머 초기화

    //소리 근원지 볼 때 개체별로 시선을 흩뜨릴 무작위 좌표
    PersonalSoundLookOffset = FVector(
        FMath::FRandRange(-80.0f, 80.0f),
        FMath::FRandRange(-80.0f, 80.0f),
        FMath::FRandRange(-40.0f, 40.0f)
    );

    AIControllerClass = ANPCDetourCrowdAIController::StaticClass();

    if (UCharacterMovementComponent* MoveComp = GetCharacterMovement())
    {
        // 1. 엔진 내장 RVO 회피 기능을 강제로 키기
        MoveComp->bUseRVOAvoidance = false;

        MoveComp->bOrientRotationToMovement = true;

        // 회피 반응 강도. 낮을수록 덜 밀리고, 높을수록 강하게 옆으로 피함
        MoveComp->bUseControllerDesiredRotation = false;
        MoveComp->RotationRate = FRotator(0.0f, 360.0f, 0.0f);

    }
}

void ANPCCharacterBase::InitializeFromMassSpawner(int32 SpawnIndex, AActor* InDestPoint, const TArray<AActor*>& InTargetPoints)
{
    const bool bWasAlreadyInitialized = bMassRouteInitialized;
    // Mass Spawner로 스폰된 개체는 0번 인덱스든 아니든 무조건 'Follower'로 시작합니다.
    InitializationType = ENPCInitializationType::Spawned;
    bIsReferenceNPC = false;

    ReferenceNPC = Cast<ANPCCharacterBase>(InDestPoint);

    if (!bWasAlreadyInitialized)
    {
        IndividualDestination = FVector::ZeroVector;
        IndividualDestinationEntryPoint = FVector::ZeroVector;
        bHasAssignedDestination = false;
        bHasAssignedDestinationEntry = false;
        bHasReachedDestinationEntry = false;
        bHasStartedFinalAdvance = false;
        bHasEnteredDestinationBox = false;
        bLoggedMissingDestinationBox = false;
        DestinationEntrySign = 0.0f;
        DestinationSelectionAttempt = 0;
        PersonalDestinationSpacingScale = 0.0f;
        bHasReachedFinalDestination = false;
        DestinationStuckTime = 0.0f;
        NextDestinationRepathTime = 0.0f;
    }

    if (ReferenceNPC)
    {
        DestPoint = ReferenceNPC->DestPoint;
        const TArray<AActor*>& RoutePoints = InTargetPoints.Num() > 0
            ? InTargetPoints
            : ReferenceNPC->TargetPoints;
        if (RoutePoints.Num() > 0)
        {
            TargetPoints = RoutePoints;
            SetSharedPathPoints(RoutePoints);
            bMassRouteInitialized = true;
        }
    }
    else
    {
        DestPoint = nullptr;

        UE_LOG(LogTemp, Error,
            TEXT("[%s] Mass NPC initialization failed: Leader is null"),
            *GetName());
    }
}

void ANPCCharacterBase::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogTemp, Warning, TEXT("[%s] Persona = %d"),
        *GetName(),
        (int32)MyPersona);

    RegisterGazeMeasurement();

    // 에디터에서 Reference NPC로 지정된 기준 캐릭터라면,
    // 아래의 팔로워용 리더 탐색 및 복사 로직을 건너뛰기
    if (bIsReferenceNPC || InitializationType == ENPCInitializationType::Reference)
    {
        // 진짜 리더는 본인이 가진 오리지널 TargetPoints를 공유 시선 이정표로 안착시킵니다.
        SetSharedPathPoints(TargetPoints);
        return;
    }

    GetWorldTimerManager().SetTimer(GazeTimerHandle, [this]()
        {
            if (bMassRouteInitialized)
            {
                return;
            }

            if (UWorld* World = GetWorld())
            {
                UMassActorSubsystem* MassActorSubsystem = World->GetSubsystem<UMassActorSubsystem>();
                if (MassActorSubsystem)
                {
                    FMassEntityHandle EntityHandle = MassActorSubsystem->GetEntityHandleFromActor(this);

                    if (EntityHandle.IsValid())
                    {
                        int32 SpawnIndex = EntityHandle.Index;
                        AActor* ReferenceActor = nullptr;

                        // Mass로 스폰된 나(팔로워)는 월드 전체에서 디테일 창으로 설정된 '진짜 리더'를 찾기
                        InitializationType = ENPCInitializationType::Spawned;

                        TArray<AActor*> FoundNPCs;
                        UGameplayStatics::GetAllActorsOfClass(World, ANPCCharacterBase::StaticClass(), FoundNPCs);
                        for (AActor* NPC : FoundNPCs)
                        {
                            ANPCCharacterBase* OtherNPC = Cast<ANPCCharacterBase>(NPC);
                            if (OtherNPC && OtherNPC != this) // [ADD] 자기 자신은 리더 후보에서 제외
                            {
                                // 에디터(디테일 창)에서 직접 배치하고 Leader로 설정한 개체를 최우선 순위로 채택
                                if (OtherNPC->bIsReferenceNPC || OtherNPC->InitializationType == ENPCInitializationType::Reference)
                                {
                                    ReferenceActor = OtherNPC;
                                    break;
                                }
                            }
                        }

                        // 이제 InitializeFromMassSpawner의 두 번째 인자로 찾아낸 진짜 리더를 넘겨주기
                        if (ANPCCharacterBase* FoundReferenceNPC = Cast<ANPCCharacterBase>(ReferenceActor))
                        {
                            InitializeFromMassSpawner(
                                SpawnIndex, ReferenceActor, FoundReferenceNPC->TargetPoints);
                            // 1) 리더가 스포이드로 찍은 경로(TargetPoints) 복사
                            if (FoundReferenceNPC->TargetPoints.Num() > 0)
                            {
                                this->TargetPoints = FoundReferenceNPC->TargetPoints;
                                SetSharedPathPoints(FoundReferenceNPC->TargetPoints);
                            }

                            // 2) 리더의 첫 목적지 액터/위치를 내 목적지로 강제 복사
                            if (FoundReferenceNPC->DestPoint)
                            {
                                this->DestPoint = FoundReferenceNPC->DestPoint;
                            }

                            // 리더와 싱크로율 맞추기 함수 호출
                            SyncScenarioDataFromReference();

                            UE_LOG(LogTemp, Log, TEXT("[%s] [지연 주입] C++ 강제 주입 성공! Reference NPC(%s)의 TargetPoints와 DestPoint를 복사했습니다."), *GetName(), *FoundReferenceNPC->GetName());
                        }
                        else {
                            UE_LOG(LogTemp, Error, TEXT("[%s] [지연 주입 실패] 레벨에서 bIsReferenceNPC가 true인 NPC를 찾지 못했습니다! DestPoint가 비어있을 수 있습니다."), *GetName());
                        }
                    }
                }
            }
        }, 2.0f, false); // 0.2초 뒤 딱 한 번 실행
}

// 외부에서 경로 데이터를 주입받는 함수 (GoalManager 등에서 호출)
void ANPCCharacterBase::SetSharedPathPoints(const TArray<AActor*>& InPathPoints)
{
    SharedGazeMilestones = InPathPoints;
    CurrentGazeIndex = 0; // 데이터가 새로 들어오면 시선 단계도 초기화

    // Spawned agents can begin anywhere along the route. Starting every agent
    // at waypoint zero makes agents that spawned farther ahead turn around.
    // Join the closest route segment at its forward endpoint instead.
    CurrentMoveIndex = 0;
    PathLateralOffset = 0.0f;
    ActivePathLateralOffsetScale = 1.0f;
    IntermediateWaypointStuckTime = 0.0f;
    NextIntermediateWaypointRetryTime = 0.0f;
    bIntermediateWaypointMovementStarted = false;
    bHasPathLateralOffset = false;
    if (InPathPoints.Num() > 1)
    {
        const FVector CurrentLocation = GetActorLocation();
        float BestDistanceSquared = TNumericLimits<float>::Max();

        for (int32 SegmentIndex = 0; SegmentIndex + 1 < InPathPoints.Num(); ++SegmentIndex)
        {
            const AActor* SegmentStart = InPathPoints[SegmentIndex];
            const AActor* SegmentEnd = InPathPoints[SegmentIndex + 1];
            if (!IsValid(SegmentStart) || !IsValid(SegmentEnd))
            {
                continue;
            }

            FVector Start = SegmentStart->GetActorLocation();
            FVector End = SegmentEnd->GetActorLocation();
            FVector Point = CurrentLocation;
            Start.Z = End.Z = Point.Z = 0.0f;

            const float DistanceSquared = FMath::PointDistToSegmentSquared(Point, Start, End);
            if (DistanceSquared < BestDistanceSquared)
            {
                BestDistanceSquared = DistanceSquared;
                CurrentMoveIndex = SegmentIndex + 1;

                const FVector Segment = End - Start;
                const float SegmentLengthSquared = Segment.SizeSquared2D();
                if (SegmentLengthSquared > KINDA_SMALL_NUMBER)
                {
                    const float Progress = FMath::Clamp(
                        FVector::DotProduct(Point - Start, Segment) / SegmentLengthSquared,
                        0.0f, 1.0f);
                    const FVector ClosestPoint = Start + Segment * Progress;
                    const FVector Forward = Segment.GetSafeNormal2D();
                    const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
                    // Preserve the lane where this NPC actually spawned. A fixed
                    // +/-300 cm clamp pulled agents from the outer lanes inward.
                    PathLateralOffset =
                        FVector::DotProduct(Point - ClosestPoint, Right);
                    bHasPathLateralOffset = true;
                }
            }
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[%s] Shared Milestones Set: %d points"), *GetName(), SharedGazeMilestones.Num());
}

// 리더로부터 직접 경로를 복사해오는 함수
void ANPCCharacterBase::SyncScenarioDataFromReference()
{
    // 리더가 존재하고, 내가 팔로워라면
    if (ReferenceNPC && InitializationType == ENPCInitializationType::Spawned)
    {
        // 리더의 시선 이정표를 내 것으로 복사
        // (리더도 SharedGazeMilestones를 가지고 있다고 가정)
        SharedGazeMilestones = ReferenceNPC->SharedGazeMilestones;
        CurrentGazeIndex = 0;

        UE_LOG(LogTemp, Log, TEXT("[%s] Sync scenario data from Reference NPC [%s]"), *GetName(), *ReferenceNPC->GetName());
    }
}

// 1. 성격 Enum을 서버가 주는 키값(String)으로 매칭하는 로직
FString GetPersonaKey(EPersonaType Persona)
{
    switch (Persona)
    {
    case EPersonaType::CAUTIOUS: return TEXT("CAUTIOUS");
    case EPersonaType::RUSHER:   return TEXT("RUSHER");
    case EPersonaType::SIGHTSEER: return TEXT("SIGHTSEER");
    }
    return TEXT("CAUTIOUS");
}

// 매 프레임 상황 판단(위험, 서버 플랜?)
void ANPCCharacterBase::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (bHasReachedFinalDestination)
    {
        bIsLooking = false;
        LookAtLocation = FVector::ZeroVector;
        UpdateGazeMeasurement(DeltaTime);
        UpdateHeadLookOffsets(DeltaTime);
        return;
    }

    UpdateSocialObservations();
    EvaluateSocialInfluence();

    // 매 틱마다 위험한지, 아니면 서버 플랜을 따라야 하는지 판단
    HandleNPCBehavior();
    UpdateGazeMeasurement(DeltaTime);
    UpdateSoundReactionRotation(DeltaTime);
    UpdateHeadLookOffsets(DeltaTime);
    UpdateIntermediateWaypointRecovery(DeltaTime);
    UpdateFinalDestinationNavigation(DeltaTime);

    // 소리 감지 중일 때 파란색 선으로 표시(디버그용)
    //if (bIsSoundDetected)
    //{
        //DrawDebugLine(GetWorld(), GetActorLocation() + FVector(0, 0, 60), SensedSoundLocation, FColor::Blue, false, -1, 0, 2.0f);
        //DrawDebugSphere(GetWorld(), SensedSoundLocation, 30.0f, 12, FColor::Blue, false, -1);
    //}
}

void ANPCCharacterBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FinishCurrentGazeMeasurement();
    FinishGazeCandidateMeasurements();
    SubmitGazeMeasurement();
    Super::EndPlay(EndPlayReason);
}

void ANPCCharacterBase::UpdateSoundReactionRotation(float DeltaTime)
{
    GazeMotorComponent->UpdateSoundReactionRotation(*this, DeltaTime);
}

void ANPCCharacterBase::UpdateHeadLookOffsets(float DeltaTime)
{
    GazeMotorComponent->UpdateHeadLookOffsets(*this, DeltaTime);
}

bool ANPCCharacterBase::CanObserveNPC(const ANPCCharacterBase* OtherNPC, bool& bOutInFOV, bool& bOutHasLineOfSight) const
{
    return PerceptionComponent->CanObserveNPC(*this, OtherNPC, bOutInFOV, bOutHasLineOfSight);
}

void ANPCCharacterBase::UpdateSocialObservations()
{
    PerceptionComponent->UpdateSocialObservations(*this);
}

float ANPCCharacterBase::GetObserverResponseWeight(FName ObservedDriver) const
{
    return GazeDecisionComponent->GetObserverResponseWeight(*this, ObservedDriver);
}

FName ANPCCharacterBase::GetObserverResponseDriver(FName ObservedDriver) const
{
    return GazeDecisionComponent->GetObserverResponseDriver(*this, ObservedDriver);
}

float ANPCCharacterBase::GetSocialObservationDelay() const
{
    return GazeDecisionComponent->GetSocialObservationDelay(*this);
}

void ANPCCharacterBase::EvaluateSocialInfluence()
{
    GazeDecisionComponent->EvaluateSocialInfluence(*this);
}

bool ANPCCharacterBase::AreSocialTargetsEquivalent(const ANPCCharacterBase* FirstNPC, const ANPCCharacterBase* SecondNPC) const
{
    return GazeDecisionComponent->AreSocialTargetsEquivalent(*this, FirstNPC, SecondNPC);
}

float ANPCCharacterBase::GetSocialBaseHoldDuration() const
{
    return GazeDecisionComponent->GetSocialBaseHoldDuration(*this);
}

float ANPCCharacterBase::GetSocialObserverHoldBonus() const
{
    return GazeDecisionComponent->GetSocialObserverHoldBonus(*this);
}

float ANPCCharacterBase::GetSocialMaximumHoldDuration() const
{
    return GazeDecisionComponent->GetSocialMaximumHoldDuration(*this);
}

void ANPCCharacterBase::ActivateOrExtendSocialGazeLock()
{
    GazeDecisionComponent->ActivateOrExtendSocialGazeLock(*this);
}

void ANPCCharacterBase::ReleaseSocialGazeLock(bool bApplyCooldown)
{
    GazeDecisionComponent->ReleaseSocialGazeLock(*this, bApplyCooldown);
}

// --- 사회적 시선 시퀀스 함수들 ---
void ANPCCharacterBase::StartSocialGazeLogic()
{
    GazeDecisionComponent->StartSocialGazeLogic(*this);
}

void ANPCCharacterBase::SwitchToFollowTarget()
{
    GazeDecisionComponent->SwitchToFollowTarget(*this);
}

void ANPCCharacterBase::RestoreNormalGaze()
{
    GazeDecisionComponent->RestoreNormalGaze(*this);
}

void ANPCCharacterBase::ResetMoveIndex()
{
    bHasReachedFinalDestination = false;
    bHasEnteredDestinationBox = false;
    bHasAssignedDestinationEntry = false;
    bHasReachedDestinationEntry = false;
    bHasStartedFinalAdvance = false;
    IndividualDestinationEntryPoint = FVector::ZeroVector;
    DestinationEntrySign = 0.0f;
    DestinationSelectionAttempt = 0;
    PersonalDestinationSpacingScale = 0.0f;
    ActivePathLateralOffsetScale = 1.0f;
    IntermediateWaypointStuckTime = 0.0f;
    NextIntermediateWaypointRetryTime = 0.0f;
    bIntermediateWaypointMovementStarted = false;
    CurrentMoveIndex = 0;           // 이동 순서 초기화
    bHasReachedFinalDestination = false;
    IndividualDestination = FVector::ZeroVector;
    bHasAssignedDestination = false;
    bLoggedMissingDestinationBox = false;
    DestinationStuckTime = 0.0f;
    NextDestinationRepathTime = 0.0f;
    CurrentServerTarget = TEXT("");  // 서버 타겟 초기화
    CurrentServerDriver = TEXT("");
    CurrentServerPlanIndex = INDEX_NONE;
    LastAppliedTarget = TEXT("");    // 마지막 상태 기록 초기화

    UE_LOG(LogTemp, Warning, TEXT("[NPC] Move Index has been Reset to 0"));
}

bool ANPCCharacterBase::GetCurrentPlanGazeLocation(FVector& OutLocation) const
{
    OutLocation = FVector::ZeroVector;
    if (!LocalPlanList.IsValidIndex(CurrentServerPlanIndex))
    {
        return false;
    }

    const FPlanItem& PlanItem = LocalPlanList[CurrentServerPlanIndex];
    if (!PlanItem.bHasResolvedGaze)
    {
        return false;
    }

    if (IsValid(PlanItem.GazeTargetActor.Get()))
    {
        OutLocation = PlanItem.GazeTargetActor->GetActorTransform().TransformPosition(PlanItem.GazeActorLocalLocation);
    }
    else
    {
        OutLocation = PlanItem.GazeWorldLocation;
    }

    return !OutLocation.IsNearlyZero();
}

void ANPCCharacterBase::HandleNPCBehavior()
{
    GazeDecisionComponent->HandleNPCBehavior(*this);
}

void ANPCCharacterBase::UpdateObservableBehavior(const FString& TargetName, const FVector& TargetLocation)
{
    ESocialObservableBehavior NewBehavior = ESocialObservableBehavior::None;
    FString NewTargetName = TargetName;
    FName NewDriver = NAME_None;
    float NewSalience = 0.0f;

    if (CurrentGazeSource == EGazeSource::Safety)
    {
        NewBehavior = ESocialObservableBehavior::LookAtVehicle;
        // Keep the legacy observable enum for Blueprint compatibility, but
        // expose the actual generic Safety actor name instead of "vehicle".
        NewTargetName = TargetName;
        NewDriver = FName(TEXT("Safety"));
        NewSalience = 1.0f;
    }
    else if (CurrentGazeSource == EGazeSource::Sound)
    {
        NewBehavior = ESocialObservableBehavior::ReactToSound;
        NewTargetName = TEXT("sound");
        NewDriver = FName(TEXT("Stimulus"));
        NewSalience = 0.85f;
    }
    else if (bIsApplyingSocialGaze)
    {
        // A copied social gaze is a response, not a new independent cue. Do not
        // publish it again, otherwise followers can echo the gaze back to the
        // original NPC and prevent that NPC from advancing to its next plan gaze.
        NewBehavior = ESocialObservableBehavior::None;
        NewTargetName = TEXT("none");
        NewDriver = NAME_None;
        NewSalience = 0.0f;
    }
    else if (!TargetName.IsEmpty() &&
        !TargetName.Equals(TEXT("forward"), ESearchCase::IgnoreCase) &&
        !TargetName.Equals(TEXT("none"), ESearchCase::IgnoreCase))
    {
        NewBehavior = ESocialObservableBehavior::LookAtTarget;
        NewDriver = CurrentServerDriver.IsEmpty() ? FName(TEXT("Info")) : FName(*CurrentServerDriver);
        NewSalience = 0.75f;
    }
    else
    {
        NewTargetName = TEXT("none");
    }

    const bool bTargetLocationChanged =
        !ObservableTargetLocation.IsNearlyZero() && !TargetLocation.IsNearlyZero() &&
        FVector::DistSquared(ObservableTargetLocation, TargetLocation) >
            FMath::Square(SocialTargetMergeRadius);
    const bool bBehaviorChanged =
        NewBehavior != CurrentObservableBehavior ||
        !NewTargetName.Equals(ObservableTargetName, ESearchCase::IgnoreCase) ||
        NewDriver != ObservableDriver ||
        bTargetLocationChanged;

    CurrentObservableBehavior = NewBehavior;
    ObservableTargetName = NewTargetName;
    ObservableTargetLocation = TargetLocation;
    ObservableDriver = NewDriver;
    ObservableBehaviorSalience = NewSalience;

    if (bBehaviorChanged)
    {
        ++ObservableBehaviorSequence;
        ObservableBehaviorChangedTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

        if (bVisualizeSocialObservation)
        {
            UE_LOG(LogTemp, Log,
                TEXT("[%s] Observable behavior changed: %s -> %s, Driver=%s (Sequence=%d)"),
                *GetName(),
                *GetObservableBehaviorDebugName(),
                *ObservableTargetName,
                *ObservableDriver.ToString(),
                ObservableBehaviorSequence);
        }
    }
}

FString ANPCCharacterBase::GetObservableBehaviorDebugName() const
{
    switch (CurrentObservableBehavior)
    {
    case ESocialObservableBehavior::LookAtTarget:
        return TEXT("LookAtTarget");
    case ESocialObservableBehavior::LookAtVehicle:
        return TEXT("LookAtVehicle");
    case ESocialObservableBehavior::ReactToSound:
        return TEXT("ReactToSound");
    default:
        return TEXT("None");
    }
}


// 위험 여부 체크 (AIController 변수 확인)
bool ANPCCharacterBase::CheckIfDanger()
{
    AAIController* AIC = Cast<AAIController>(GetController());
    if (!AIC) return false;

    bool bCurrentDanger = false;
    // 리플렉션을 통해 AIController의 bIsDanger 변수 값을 가져오기
    FProperty* DangerProp = AIC->GetClass()->FindPropertyByName(TEXT("bIsDanger"));
    if (DangerProp)
    {
        DangerProp->GetValue_InContainer(AIC, &bCurrentDanger);
    }
    return bCurrentDanger;
}

// 4. 위험 상황 시 행동 (차 쳐다보기 + 멈추기)
void ANPCCharacterBase::ExecuteEmergencyResponse()
{
    AAIController* AIC = Cast<AAIController>(GetController());
    if (AIC)
    {
        AIC->StopMovement(); // 걷고 있었다면 그 자리에 멈춤
    }

    // 시선을 차량으로 고정 (UpdateLookAtTarget 내부의 축 설정이 중요!)
    UpdateLookAtTarget(TEXT("Mpving Vehicle"));

}

void ANPCCharacterBase::ExecuteServerPlan()
{
    // 위험 상황이 해제되면, 서버 플랜에서 마지막으로 보라고 했던 대상을 다시 봅니다.
    // 만약 기억된 타겟이 없다면 기본적으로 "forward"(정면)를 보게 합니다.

    // The server gaze has already been resolved from the detector bbox into
    // the current plan item. CurrentServerTarget is only a descriptive label;
    // it must never be interpreted as an Actor Tag.
    FVector ServerGazeLocation;
    if (GetCurrentPlanGazeLocation(ServerGazeLocation))
    {
        LookAtLocation = ServerGazeLocation;
        bIsLooking = true;
    }
    else if (CurrentServerTarget.IsEmpty() ||
        CurrentServerTarget.Equals(TEXT("forward"), ESearchCase::IgnoreCase))
    {
        bIsLooking = false;
    }
}


void ANPCCharacterBase::UpdateLookAtTarget(FString TargetObjectName)
{
    if (TargetObjectName.Equals(TEXT("forward"), ESearchCase::IgnoreCase) ||
        TargetObjectName.Equals(TEXT("none"), ESearchCase::IgnoreCase))
    {
        bIsLooking = false;
        return;
    }

    TArray<AActor*> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), FName(*TargetObjectName), FoundActors);

    if (FoundActors.Num() > 0)
    {
        AActor* TargetActor = FoundActors[0];
        FVector FoundLocation = FVector::ZeroVector;
        bool bSocketFound = false;

        // 액터 안의 모든 '씬 컴포넌트'(메시, 트랜스폼 포함)를 다 가져옵니다.
        TArray<USceneComponent*> AllComponents;
        TargetActor->GetComponents<USceneComponent>(AllComponents);

        for (USceneComponent* Comp : AllComponents)
        {
            // 1. 컴포넌트 이름 자체가 "LookAtSocket"인 경우 (Scene 컴포넌트 추가했을 때)
            // 2. 혹은 메시 컴포넌트 안에 "LookAtSocket"이라는 소켓이 존재하는 경우
            if (Comp->GetName().Contains(TEXT("LookAtSocket")))
            {
                FoundLocation = Comp->GetComponentLocation();
                bSocketFound = true;
                break;
            }
            else if (Comp->DoesSocketExist(TEXT("LookAtSocket")))
            {
                FoundLocation = Comp->GetSocketLocation(TEXT("LookAtSocket"));
                bSocketFound = true;
                break;
            }
        }

        if (bSocketFound)
        {
            LookAtLocation = FoundLocation;
            bIsLooking = true;
        }
        else
        {
            // 안전장치: 소켓을 못 찾으면 액터 중심에서 위로 200만큼 올린 곳을 봅니다.
            LookAtLocation = TargetActor->GetActorLocation() + FVector(0.0f, 0.0f, 200.0f);
            bIsLooking = true;
            UE_LOG(LogTemp, Error, TEXT("[warning] LookAtSocket isn't. use original location: %s"), *LookAtLocation.ToString());
        }
        
        if (bIsLooking)
        {
            FColor LineColor = FColor::Green;

            if (InitializationType == ENPCInitializationType::Reference)
            {
                // 리더가 위험을 보고 있을 때: 초록색
                if (CheckIfDanger()) LineColor = FColor::Green;
            }
            else if (InitializationType == ENPCInitializationType::Spawned)
            {
                // 팔로워가 볼 때: 파란색
                LineColor = FColor::Blue;
            }

            // 타겟 위치에 작은 구체 표시
            DrawDebugSphere(GetWorld(), LookAtLocation, 15.0f, 12, LineColor, false, 0.1f);
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT(" Target NOT Found with Tag: %s"), *TargetObjectName);
        bIsLooking = false;
    }
}


void ANPCCharacterBase::HandleSoundDetection(FVector SoundLocation)
{
    PerceptionComponent->HandleSoundDetection(*this, SoundLocation);
}

void ANPCCharacterBase::SetPlanFromAllPlans(const TMap<FString, FPlanItemList>& AllPlans)
{
    FString MyKey = GetPersonaKey(MyPersona);

    if (AllPlans.Contains(MyKey))
    {
        SetPlanList(AllPlans[MyKey].Items);

        UE_LOG(LogTemp, Warning, TEXT("[%s] persona : (%s)  plan : %d개"),
            *GetName(), *MyKey, AllPlans[MyKey].Items.Num());
    }
    else
    {
        // 데이터 매칭 실패 시 로그
        UE_LOG(LogTemp, Error, TEXT("[%s] can't find server key: %s"), *GetName(), *MyKey);
    }
}

void ANPCCharacterBase::SetPlanList(const TArray<FPlanItem>& NewPlan)
{
    // 배열을 비우고 크기를 미리 예약 (효율적)
    LocalPlanList.Empty();
    LocalPlanList.Reserve(NewPlan.Num());

    // 통째로 대입(=)하지 않고 하나씩 추가 (C1001 회피용)
    for (const FPlanItem& Item : NewPlan)
    {
        LocalPlanList.Add(Item);
    }

    ResetMoveIndex();
    UE_LOG(LogTemp, Log, TEXT("[%s] plan copy: %d"), *GetName(), LocalPlanList.Num());
}

float ANPCCharacterBase::CalculateGaussianScore(FVector TestLocation, FThreatSource Source)
{
    return GazeDecisionComponent->CalculateGaussianScore(*this, TestLocation, Source);
}

FVector ANPCCharacterBase::CalculateGMMBestPeak(const TArray<FThreatSource>& Threats)
{
    return GazeDecisionComponent->CalculateGMMBestPeak(*this, Threats);
}

FString ANPCCharacterBase::GetPersonaKeyName() const
{
    switch (MyPersona)
    {
    case EPersonaType::CAUTIOUS:
        return TEXT("CAUTIOUS");
    case EPersonaType::RUSHER:
        return TEXT("RUSHER");
    case EPersonaType::SIGHTSEER:
        return TEXT("SIGHTSEER");
    default:
        return TEXT("CAUTIOUS");
    }
}
