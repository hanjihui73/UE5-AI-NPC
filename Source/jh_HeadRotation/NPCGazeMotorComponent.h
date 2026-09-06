#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "NPCGazeMotorComponent.generated.h"

class ANPCCharacterBase;
class USafetyStimulusComponent;
struct FThreatSource;

/** Character-driven Body: gaze and movement reactions. No independent component tick.
 * Existing character properties remain the Blueprint/serialization boundary. */
UCLASS()
class JH_HEADROTATION_API UNPCGazeMotorComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UNPCGazeMotorComponent();

    void UpdateSoundReactionRotation(ANPCCharacterBase& NPC, float DeltaTime) const;
    void UpdateHeadLookOffsets(ANPCCharacterBase& NPC, float DeltaTime) const;
    void UpdateReactiveMovement(ANPCCharacterBase& NPC) const;
};
