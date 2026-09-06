#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCPerceptionComponent.generated.h"

class ANPCCharacterBase;
class USafetyStimulusComponent;
struct FThreatSource;

/** Character-driven Eyes: perception. No independent component tick.
 * Existing character properties remain the Blueprint/serialization boundary. */
UCLASS()
class JH_HEADROTATION_API UNPCPerceptionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UNPCPerceptionComponent();

    bool CanObserveNPC(const ANPCCharacterBase& NPC, const ANPCCharacterBase* OtherNPC, bool& bOutInFOV, bool& bOutHasLineOfSight) const;
    void UpdateSocialObservations(ANPCCharacterBase& NPC) const;
    void HandleSoundDetection(ANPCCharacterBase& NPC, FVector SoundLocation) const;
    void CollectSafetyStimuli(const ANPCCharacterBase& NPC, TArray<USafetyStimulusComponent*>& OutStimuli) const;
};
