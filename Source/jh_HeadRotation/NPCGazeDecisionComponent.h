#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCGazeDecisionComponent.generated.h"

class ANPCCharacterBase;
class USafetyStimulusComponent;
struct FThreatSource;

// Native settings only: serialized persona configuration stays on the character.
struct FNPCPersonaGazeSettings
{
    float Information = 30.0f;
    float Safety = 100.0f;
    float Stimulus = 70.0f;
    float Social = 0.0f;
    float HoldTime = 1.5f;
};

/** Character-driven Brain: gaze decisions. No independent component tick.
 * Existing character properties remain the Blueprint/serialization boundary. */
UCLASS()
class JH_HEADROTATION_API UNPCGazeDecisionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UNPCGazeDecisionComponent();

    void HandleNPCBehavior(ANPCCharacterBase& NPC) const;
    float CalculateGaussianScore(ANPCCharacterBase& NPC, FVector TestLocation, FThreatSource Source) const;
    FVector CalculateGMMBestPeak(ANPCCharacterBase& NPC, const TArray<FThreatSource>& Threats) const;
    float GetObserverResponseWeight(const ANPCCharacterBase& NPC, FName ObservedDriver) const;
    FName GetObserverResponseDriver(const ANPCCharacterBase& NPC, FName ObservedDriver) const;
    float GetSocialObservationDelay(const ANPCCharacterBase& NPC) const;
    void EvaluateSocialInfluence(ANPCCharacterBase& NPC) const;
    bool AreSocialTargetsEquivalent(const ANPCCharacterBase& NPC, const ANPCCharacterBase* FirstNPC, const ANPCCharacterBase* SecondNPC) const;
    float GetSocialBaseHoldDuration(const ANPCCharacterBase& NPC) const;
    float GetSocialObserverHoldBonus(const ANPCCharacterBase& NPC) const;
    float GetSocialMaximumHoldDuration(const ANPCCharacterBase& NPC) const;
    void ActivateOrExtendSocialGazeLock(ANPCCharacterBase& NPC) const;
    void ReleaseSocialGazeLock(ANPCCharacterBase& NPC, bool bApplyCooldown) const;
    void StartSocialGazeLogic(ANPCCharacterBase& NPC) const;
    void SwitchToFollowTarget(ANPCCharacterBase& NPC) const;
    void RestoreNormalGaze(ANPCCharacterBase& NPC) const;

private:
    void UpdatePlanMilestone(ANPCCharacterBase& NPC) const;
    FNPCPersonaGazeSettings GetPersonaSettings(const ANPCCharacterBase& NPC) const;
    void AppendServerCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Information) const;
    bool AppendSafetyCandidates(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Safety, float Motive_HoldTime) const;
    void AppendSoundCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Stimulus, float Motive_HoldTime) const;
    void AppendSocialCandidate(ANPCCharacterBase& NPC, TArray<FThreatSource>& ThreatMap, float Motive_Information, float Motive_Social) const;
    FString ApplySelectedGaze(ANPCCharacterBase& NPC, const TArray<FThreatSource>& ThreatMap, bool bHasSafetyCandidate, float FinalLookAtDuration) const;
    void PublishGazeAndDebug(ANPCCharacterBase& NPC, const FString& TargetToLook) const;
};
