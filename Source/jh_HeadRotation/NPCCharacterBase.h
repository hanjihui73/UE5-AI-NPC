#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "NPCPlanTypes.h"
#include "Navigation/CrowdFollowingComponent.h" 
#include "AIController.h"
#include "NPCCharacterBase.generated.h"

class UNPCPerceptionComponent;
class UNPCGazeDecisionComponent;
class UNPCGazeMotorComponent;


UENUM(BlueprintType)
enum class ENPCInitializationType : uint8
{
    Reference   UMETA(DisplayName = "Reference"),
    Spawned     UMETA(DisplayName = "Spawned")
};

UENUM(BlueprintType)
enum class EGazeState : uint8
{
    Normal,         // 일반 플랜 따라가기
    CheckLeader,    // 리더 얼굴/머리 보기
    FollowTarget    // 리더가 보는 타겟 같이 보기
};

UENUM(BlueprintType)
enum class ESocialObservableBehavior : uint8
{
    None            UMETA(DisplayName = "None"),
    LookAtTarget    UMETA(DisplayName = "Look At Target"),
    LookAtVehicle   UMETA(DisplayName = "Look At Vehicle"),
    ReactToSound    UMETA(DisplayName = "React To Sound")
};

//성격 유형 정의
UENUM(BlueprintType)
enum class EPersonaType : uint8
{
    CAUTIOUS    UMETA(DisplayName = "Cautious"),
    RUSHER      UMETA(DisplayName = "Rusher"),
    SIGHTSEER   UMETA(DisplayName = "Sightseer")
};

UENUM(BlueprintType)
enum class EGazeSource : uint8
{
    None,
    Server,
    Safety,
    Sound,
    Social
};

/** * GMM(Gaussian Mixture Model)의 개별 가우시안 성분을 정의하는 구조체
 * 각 위협 요소(차량, 소리 등)를 하나의 '산'으로 치환
 */
USTRUCT(BlueprintType)
struct FThreatSource
{
    GENERATED_BODY()

    // 1. 산의 위치 (평균, mu): 차나 소리가 있는 좌표
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector Location;

    // 2. 산의 높이 (가중치, alpha): 위험의 강도나 우선순위 점수
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Intensity;

    // 3. 산의 넓이 (분산, sigma): 위험이 퍼져 있는 범위 (값이 클수록 완만하고 넓은 산)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Spread;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    EGazeSource Source;

    // Optional actor that produced this candidate. Safety stimuli use it so the
    // selected object can be identified without vehicle-specific tags.
    UPROPERTY(BlueprintReadOnly)
    TObjectPtr<AActor> TargetActor;

    // 생성자 (기본값 설정)
    FThreatSource()
        : Location(FVector::ZeroVector), Intensity(0.0f), Spread(500.0f), Source(EGazeSource::None), TargetActor(nullptr) {
    }

    FThreatSource(FVector InLoc, float InInt, float InSpr, EGazeSource InSource = EGazeSource::None, AActor* InTargetActor = nullptr)
        : Location(InLoc), Intensity(InInt), Spread(InSpr), Source(InSource), TargetActor(InTargetActor) {
    }
};

UCLASS()
class JH_HEADROTATION_API ANPCCharacterBase : public ACharacter
{
    GENERATED_BODY()


public:
    ANPCCharacterBase();

    // Default subobjects are driven by the existing character update order.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Architecture")
    TObjectPtr<UNPCPerceptionComponent> PerceptionComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Architecture")
    TObjectPtr<UNPCGazeDecisionComponent> GazeDecisionComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Architecture")
    TObjectPtr<UNPCGazeMotorComponent> GazeMotorComponent;

    UFUNCTION(BlueprintCallable, Category = "AI|Group")
    void InitializeFromMassSpawner(int32 SpawnIndex, AActor* InDestPoint, const TArray<AActor*>& InTargetPoints);

    // 서버에서 받은 특정 성격의 플랜 배열
    UPROPERTY(BlueprintReadWrite, Category = "AI")
    TArray<FPlanItem> Items;

    // 리더나 부모로부터 공유받은 시선 전환용 경로 이정표 (좌표 체크용)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Path")
    TArray<AActor*> SharedGazeMilestones;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Group")
    ENPCInitializationType InitializationType = ENPCInitializationType::Reference;

    // 디테일 패널에서 선택할 성격 변수 
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Personality")
    EPersonaType MyPersona = EPersonaType::CAUTIOUS;

    //성격에 맞는 String 반환 함수 (서버 키값 매칭용)
    UFUNCTION(BlueprintCallable, Category = "AI|Personality")
    FString GetPersonaKeyName() const;

    // 현재 시선 상태
    UPROPERTY(BlueprintReadOnly, Category = "AI|Gaze")
    EGazeState CurrentGazeState = EGazeState::Normal;

    // 리더가 위험을 감지했는지 감시 (Tick에서 사용)
    bool bLastLeaderDangerState = false;

    // 상태 전환 함수들
    void StartSocialGazeLogic();
    void SwitchToFollowTarget();
    void RestoreNormalGaze();
    void UpdateLookAtTarget(FString TargetObjectName);

    FTimerHandle GazeTimerHandle;

    // 시선 전용 인덱스 (CurrentMoveIndex와 분리하여 시선 단계만 관리)
    UPROPERTY(BlueprintReadWrite, Category = "AI|Gaze")
    int32 CurrentGazeIndex = 0;

    // 리더로부터 경로(위치 데이터)만 복사해오는 함수
    UFUNCTION(BlueprintCallable, Category = "AI|Path")
    void SyncScenarioDataFromReference();

    // 이동용 인덱스
    UPROPERTY(BlueprintReadWrite, Category = "AI")
    int32 CurrentMoveIndex = 0;

    /** True once this NPC has settled at its own final navigation slot. */
    UPROPERTY(BlueprintReadOnly, Category = "AI|Path")
    bool bHasReachedFinalDestination = false;

    UPROPERTY(BlueprintReadWrite, Category = "AI")
    bool bIsDangerDetected = false; // 차량 위험 감지 시 true로 변경

    /** Minimum observer-relative danger score required to enter the GMM. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinimumSafetyDangerScore = 0.15f;

    /** Seconds used when predicting the closest approach of a moving stimulus. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float SafetyPredictionHorizon = 3.0f;

    /** Common range in which this NPC evaluates every Safety component. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety", meta = (ClampMin = "100.0"))
    float SafetyDetectionRadius = 1500.0f;

    /** Common speed (cm/s) treated as fully dangerous by this NPC. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety", meta = (ClampMin = "1.0"))
    float SafetyDangerousSpeed = 600.0f;

    /** Temporary diagnosis switch. Disable after validating the Safety pipeline. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety|Debug")
    bool bForceSafetyWinnerForDebug = false;

    /** Logs real Safety inputs and winners without changing their scores. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety|Debug")
    bool bEnableSafetyDebugLogging = true;

    /** GMM weight used only while bForceSafetyWinnerForDebug is enabled. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Safety|Debug", meta = (ClampMin = "0.0"))
    float ForcedSafetyGMMWeight = 1000.0f;

    UPROPERTY(BlueprintReadWrite, Category = "AI")
    FString DangerTargetTag = TEXT("vehicle"); // 위험 시 볼 대상 태그

    UFUNCTION(BlueprintCallable, Category = "AI")
    FVector GetNextMoveLocation(const TArray<AActor*>& InTargetPoints);
    // 시선과 이동을 통합 관리하는 핵심 함수
    UFUNCTION(BlueprintCallable, Category = "AI")
    FVector GetNextStep(const TArray<FPlanItem>& PlanList, const TArray<AActor*>& PathPoints);

    // 기존 FRotator CurrentTargetRot 대신 FVector를 추가하거나 사용합니다.
    UPROPERTY(BlueprintReadOnly, Category = "AI")
    FVector LookAtLocation;

    UPROPERTY(BlueprintReadOnly, Category = "AI")
    bool bIsLooking = false;

    UPROPERTY(BlueprintReadOnly, Category = "AI|Gaze")
    EGazeSource CurrentGazeSource = EGazeSource::None;

    /** Local-space look offsets consumed by the character animation Blueprint. */
    UPROPERTY(BlueprintReadOnly, Category = "AI|Gaze")
    float HeadYaw = 0.0f;

    UPROPERTY(BlueprintReadOnly, Category = "AI|Gaze")
    float HeadPitch = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI")
    AActor* DestPoint;

    // Fraction of the DestPoint box used for final NPC slots. Keeping this below
    // 1.0 prevents a large crowd destination from scattering agents to its edges.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Path",
        meta = (ClampMin = "0.25", ClampMax = "1.0", UIMin = "0.25", UIMax = "1.0"))
    float DestinationUseAreaScale = 0.65f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI")
    TArray<AActor*> TargetPoints;

    UFUNCTION(BlueprintCallable, Category = "AI")
    void HandleSoundDetection(FVector SoundLocation);

    // 이 NPC가 따라가야 할 리더 설정
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "AI|Group")
    ANPCCharacterBase* ReferenceNPC;

    // 내가 리더인지 설정 (기본값 true)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI|Group")
    bool bIsReferenceNPC = true;

    // 서버에서 받은 플랜 아이템들을 저장할 로컬 배열
    UPROPERTY(BlueprintReadWrite, Category = "AI")
    TArray<FPlanItem> LocalPlanList;

    // 외부(GoalManager)에서 플랜을 넣어줄 함수
    UFUNCTION(BlueprintCallable, Category = "AI")
    void SetPlanList(const TArray<FPlanItem>& NewPlan);

    // 서버에서 받은 '전체' 플랜 데이터 중에서 내 성격 것만 골라 담는 함수
    UFUNCTION(BlueprintCallable, Category = "AI")
    void SetPlanFromAllPlans(const TMap<FString, FPlanItemList>& AllPlans);

    // 외부에서 강제로 경로를 주입할 때 사용
    UFUNCTION(BlueprintCallable, Category = "AI|Path")
    void SetSharedPathPoints(const TArray<AActor*>& InPathPoints);

    // 개별 가우시안 점수 계산
    UFUNCTION(BlueprintCallable, Category = "GMM")
    float CalculateGaussianScore(FVector TestLocation, FThreatSource Source);

    //최고의 봉우리(Peak)를 찾는 함수
    UFUNCTION(BlueprintCallable, Category = "GMM")
    FVector CalculateGMMBestPeak(const TArray<FThreatSource>& Threats);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMM|Visualization")
    bool bVisualizeGaussianField = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMM|Visualization", meta = (ClampMin = "500.0"))
    float GaussianFieldExtent = 2000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMM|Visualization", meta = (ClampMin = "50.0"))
    float GaussianFieldCellSize = 200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMM|Visualization", meta = (ClampMin = "0.05"))
    float GaussianFieldRefreshInterval = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Observation")
    bool bEnableSocialObservation = true;

    // Enable this only on the NPC whose incoming social influence you want to inspect.
    // Its current influence source is outlined in the world without drawing debug text.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Observation",
        meta = (DisplayName = "Show Social Influence Source Outline"))
    bool bVisualizeSocialObservation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Observation", meta = (ClampMin = "50.0"))
    float SocialObservationRadius = 500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Observation", meta = (ClampMin = "1.0", ClampMax = "179.0"))
    float SocialObservationHalfAngle = 60.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Observation", meta = (ClampMin = "0.05"))
    float SocialObservationInterval = 0.2f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation")
    TArray<ANPCCharacterBase*> VisibleSocialNPCs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    ESocialObservableBehavior CurrentObservableBehavior = ESocialObservableBehavior::None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    FString ObservableTargetName = TEXT("none");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    FVector ObservableTargetLocation = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    int32 ObservableBehaviorSequence = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    float ObservableBehaviorChangedTime = 0.0f;

    // Why the source NPC is currently acting. Kept as a tag so new server drivers
    // can be added without changing this C++ type.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    FName ObservableDriver = NAME_None;

    // Generic strength of the published cue. Gaze and future animations can share it.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Observation|Behavior")
    float ObservableBehaviorSalience = 0.0f;

    // A continuously observed gaze becomes more convincing over this period.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Persistence", meta = (ClampMin = "0.1"))
    float SocialPersistenceBuildTime = 2.0f;

    // 0.5 means a gaze held for SocialPersistenceBuildTime receives up to a
    // 1.5x persistence multiplier.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Persistence", meta = (ClampMin = "0.0"))
    float SocialPersistenceMaximumBonus = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MinimumSocialInfluenceScore = 0.05f;

    // Continuous observation time required before copying another NPC's gaze.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float CautiousSocialObservationDelay = 0.9f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float SightseerSocialObservationDelay = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float RusherSocialObservationDelay = 1.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float SocialObservationDelayRandomRange = 0.15f;

    // Each additional NPC looking at the same target shortens the delay, but
    // never removes it completely.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float SocialGroupObservationDelayReduction = 0.35f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Observation Delay", meta = (ClampMin = "0.0"))
    float MinimumSocialObservationDelay = 0.25f;

    // NPCs looking at nearby points or in nearly the same direction are treated
    // as sharing one social-interest target.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "10.0"))
    float SocialTargetMergeRadius = 250.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "1.0", ClampMax = "45.0"))
    float SocialTargetMergeAngle = 12.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SocialGroupStrengthPerObserver = 0.12f;

    // Multiplies the existing per-observer hold bonus after a social gaze has
    // been selected. This affects hold time, not the selection score.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "0.0"))
    float SocialGroupHoldBonusMultiplier = 2.0f;

    // Limits how many NPCs can contribute their base social motive to one GMM
    // candidate, preventing very large crowds from producing unbounded weights.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "1"))
    int32 MaximumSocialGMMContributorCount = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Social Influence|Stability", meta = (ClampMin = "0.0"))
    float SocialReselectionCooldown = 0.50f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    float CurrentSocialInfluenceScore = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    ANPCCharacterBase* CurrentSocialInfluenceSource = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    ESocialObservableBehavior CurrentSocialInfluenceBehavior = ESocialObservableBehavior::None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    FString CurrentSocialInfluenceTargetName = TEXT("none");

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    FVector CurrentSocialInfluenceTargetLocation = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    int32 CurrentSocialInfluenceSequence = 0;

    // The observer's own reason for responding; this is deliberately separate
    // from the source NPC's ObservableDriver.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence")
    FName CurrentSocialResponseDriver = NAME_None;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence|Debug")
    int32 CurrentSocialObserverCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Social Influence|Debug")
    float CurrentSocialLockRemaining = 0.0f;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // 의사결정을 내리는 핵심 함수 (Tick이나 필요한 곳에서 호출)
    void HandleNPCBehavior();

    //서버의 플랜(PlanList)을 따르는 일반 행동
    void ExecuteServerPlan();

    // 위험 상황 시 대응 행동
    void ExecuteEmergencyResponse();

    // 현재 AI Controller에서 위험 여부 가져오기
    bool CheckIfDanger();

    FString CurrentServerTarget;    // 현재 서버 플랜에서 보고 있어야 할 타겟 이름을 저장
    FString CurrentServerDriver;    // 현재 서버 플랜이 이 시선을 선택한 이유
    int32 CurrentServerPlanIndex = INDEX_NONE;

    bool GetCurrentPlanGazeLocation(FVector& OutLocation) const;

    FString LastAppliedTarget;   // 실제로 고개가 돌아가고 있는 현재 목표물 이름

    UPROPERTY(BlueprintReadWrite, Category = "AI")
    bool bIsSoundDetected = false; // 소리 감지 상태 플래그

    UPROPERTY(BlueprintReadWrite, Category = "AI")
    FVector SensedSoundLocation;   // 감지된 소리의 좌표 저장

    FTimerHandle SoundTimerHandle; // 소리 확인 후 복귀를 위한 타이머

    UFUNCTION(BlueprintCallable, Category = "AI")
    void ResetMoveIndex();

    UFUNCTION(BlueprintImplementableEvent, Category = "AI")
    void RequestNextStep();

    float PersonalHoldTimeModifier;  // 시선 유지 시간 배율 오프셋 (0.2 ~ 0.8초 편차용)
    float PersonalThreatWeightBonus; // 위협 인지 민감도 무작위 편차
    float CurrentLookAtTimer;
    float SoundDetectedTime = 0.0f; //소리가 발생한 절대 시간을 개체별로 기억하기 위함
    FVector PersonalSoundLookOffset;
    float VehicleDangerStartTime = 0.0f; // 현재 Safety 자극 주시 시작 시간
    TWeakObjectPtr<AActor> ActiveSafetyTarget;
    float NextSafetyDebugLogTime = 0.0f;
    float NextSafetyWinnerDebugLogTime = 0.0f;

private:
    // Components implement behavior against the existing serialized character state.
    friend class UNPCPerceptionComponent;
    friend class UNPCGazeDecisionComponent;
    friend class UNPCGazeMotorComponent;

    void UpdateSoundReactionRotation(float DeltaTime);
    void UpdateHeadLookOffsets(float DeltaTime);
    void EnterFinalDestinationState();
    void UpdateFinalDestinationNavigation(float DeltaTime);
    void UpdateIntermediateWaypointRecovery(float DeltaTime);
    bool StartFinalAdvance(class UBoxComponent* DestinationBox);
    bool TryBuildDestinationEntryPoint(class UBoxComponent* DestinationBox, FVector& OutEntryPoint) const;
    bool TryChooseAlternativeDestination(class UBoxComponent* DestinationBox, FVector& OutDestination);

    void UpdateSocialObservations();
    bool CanObserveNPC(const ANPCCharacterBase* OtherNPC, bool& bOutInFOV, bool& bOutHasLineOfSight) const;
    void UpdateObservableBehavior(const FString& TargetName, const FVector& TargetLocation);
    FString GetObservableBehaviorDebugName() const;
    void EvaluateSocialInfluence();
    float GetObserverResponseWeight(FName ObservedDriver) const;
    FName GetObserverResponseDriver(FName ObservedDriver) const;
    float GetSocialObservationDelay() const;
    bool AreSocialTargetsEquivalent(const ANPCCharacterBase* FirstNPC, const ANPCCharacterBase* SecondNPC) const;
    float GetSocialBaseHoldDuration() const;
    float GetSocialObserverHoldBonus() const;
    float GetSocialMaximumHoldDuration() const;
    void ActivateOrExtendSocialGazeLock();
    void ReleaseSocialGazeLock(bool bApplyCooldown);
    void RegisterGazeMeasurement();
    void ResetGazeMeasurement();
    void UpdateGazeMeasurement(float DeltaTime);
    void FinishCurrentGazeMeasurement();
    void UpdateGazeCandidateMeasurement(const TArray<FThreatSource>& Candidates, EGazeSource WinningSource);
    void FinishGazeCandidateMeasurements();
    void SubmitGazeMeasurement();
    int32 GetCurrentSocialMeasurementOrigin() const;

    float NextSocialObservationTime = 0.0f;
    float NextSocialDebugLogTime = 0.0f;
    int32 LastDebugInRadiusCount = INDEX_NONE;
    int32 LastDebugOutsideFOVCount = INDEX_NONE;
    int32 LastDebugBlockedLOSCount = INDEX_NONE;
    int32 LastDebugVisibleCount = INDEX_NONE;
    int32 DebugBestSocialObserverCount = 0;
    float DebugBestSocialScore = 0.0f;
    float DebugSocialObservationElapsed = 0.0f;
    float DebugSocialObservationRequired = 0.0f;
    float DebugServerGMMWeight = 0.0f;
    float DebugSocialGMMWeight = 0.0f;
    FString DebugSocialState = TEXT("Idle");
    bool bIsApplyingSocialGaze = false;
    ANPCCharacterBase* LockedSocialInfluenceSource = nullptr;
    FVector LockedSocialTargetLocation = FVector::ZeroVector;
    FString LockedSocialTargetName = TEXT("none");
    FName LockedSocialResponseDriver = NAME_None;
    ESocialObservableBehavior LockedSocialInfluenceBehavior = ESocialObservableBehavior::None;
    float LockedSocialInfluenceScore = 0.0f;
    int32 LockedSocialInfluenceSequence = 0;
    float SocialLockStartTime = 0.0f;
    float SocialLockEndTime = 0.0f;
    float NextSocialSelectionAllowedTime = 0.0f;
    int32 LockedSocialObserverCount = 0;
    ANPCCharacterBase* PendingSocialInfluenceSource = nullptr;
    int32 PendingSocialInfluenceSequence = 0;
    float PendingSocialInfluenceStartTime = 0.0f;
    float PersonalSocialObservationDelayOffset = 0.0f;

    EGazeSource MeasuredGazeSource = EGazeSource::None;
    FString MeasuredGazeTarget = TEXT("none");
    float MeasuredGazeStartTime = 0.0f;
    int32 GazeMeasurementEventCounts[5] = { 0, 0, 0, 0, 0 };
    float GazeMeasurementDurations[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    int32 GazeCandidateExposureCounts[5] = { 0, 0, 0, 0, 0 };
    int32 GazeCandidateSelectedCounts[5] = { 0, 0, 0, 0, 0 };
    bool bGazeCandidateActive[5] = { false, false, false, false, false };
    bool bGazeCandidateSelectedDuringExposure[5] = { false, false, false, false, false };
    float SocialObserverSeconds = 0.0f;
    float MeasuredSocialSeconds = 0.0f;
    int32 MeasuredSocialOrigin = 0;
    int32 SocialOriginEventCounts[5] = { 0, 0, 0, 0, 0 };
    float SocialOriginDurations[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    bool bGazeMeasurementRegistered = false;

    void DrawGaussianFieldHeatmap(const TArray<FThreatSource>& Threats);
    FColor GetGaussianHeatColor(float NormalizedScore) const;
    float NextGaussianFieldDrawTime = 0.0f;

    // 가야 할 곳으로 고개를 돌리는 중인지 체크
    bool bIsRotatingHeadBack = false;
    bool bIsPausedByDanger = false; // 위험 때문에 멈췄는지 체크하는 변수
    bool bSoundBodyTurnActive = false;
    bool bSavedOrientRotationToMovement = true;
    bool bSavedUseControllerDesiredRotation = false;
    FVector IndividualDestination = FVector::ZeroVector;
    FVector IndividualDestinationEntryPoint = FVector::ZeroVector;
    bool bHasAssignedDestination = false;
    bool bHasAssignedDestinationEntry = false;
    bool bHasReachedDestinationEntry = false;
    bool bHasStartedFinalAdvance = false;
    bool bHasEnteredDestinationBox = false;
    bool bLoggedMissingDestinationBox = false;
    float DestinationEntrySign = 0.0f;
    int32 DestinationSelectionAttempt = 0;
    float PersonalDestinationSpacingScale = 0.0f;
    float DestinationStuckTime = 0.0f;
    float NextDestinationRepathTime = 0.0f;
    float PathLateralOffset = 0.0f;
    float ActivePathLateralOffsetScale = 1.0f;
    float IntermediateWaypointStuckTime = 0.0f;
    float NextIntermediateWaypointRetryTime = 0.0f;
    bool bIntermediateWaypointMovementStarted = false;
    bool bHasPathLateralOffset = false;
    bool bMassRouteInitialized = false;
};
