#include "NPCGazeMotorComponent.h"
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

UNPCGazeMotorComponent::UNPCGazeMotorComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UNPCGazeMotorComponent::UpdateSoundReactionRotation(ANPCCharacterBase& NPC, float DeltaTime) const
{
    UCharacterMovementComponent* MoveComp = NPC.GetCharacterMovement();
    if (!MoveComp)
    {
        return;
    }

    // Hearing alone must not affect movement. The GMM/persona decision owns
    // the reaction, so an ignored sound cannot turn a Rusher's body.
    const bool bShouldReactToSound =
        NPC.bIsSoundDetected && NPC.CurrentGazeSource == EGazeSource::Sound;

    if (bShouldReactToSound)
    {
        // Use the same winning GMM point as the head-look code so body and head
        // cannot react to two slightly different sound locations.
        const FVector ActiveSoundLookLocation =
            NPC.LookAtLocation.IsNearlyZero() ? NPC.SensedSoundLocation : NPC.LookAtLocation;
        FVector ToSound = ActiveSoundLookLocation - NPC.GetActorLocation();
        ToSound.Z = 0.0f;
        if (ToSound.IsNearlyZero())
        {
            return;
        }

        const float TargetYaw = ToSound.Rotation().Yaw;
        const float SignedYaw = FMath::FindDeltaAngleDegrees(
            NPC.GetActorRotation().Yaw, TargetYaw);
        const float YawDifference = FMath::Abs(SignedYaw);

        if (YawDifference > 70.0f || NPC.bSoundBodyTurnActive)
        {
            if (!NPC.bSoundBodyTurnActive)
            {
                NPC.bSoundBodyTurnActive = true;
                NPC.bSavedOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
                NPC.bSavedUseControllerDesiredRotation = MoveComp->bUseControllerDesiredRotation;

                if (AAIController* AIC = Cast<AAIController>(NPC.GetController()))
                {
                    AIC->StopMovement();
                    // Manual actor rotation below is authoritative. A focal
                    // point here would compete with it and may reverse the turn.
                    AIC->ClearFocus(EAIFocusPriority::Gameplay);
                }
                NPC.bIsPausedByDanger = true;
            }

            MoveComp->bOrientRotationToMovement = false;
            MoveComp->bUseControllerDesiredRotation = false;

            // Let the head cover up to 70 degrees and rotate the body only by
            // the excess, preserving the sign (left/right) of the sound angle.
            const float RetainedHeadYaw = FMath::Clamp(SignedYaw, -70.0f, 70.0f);
            const float DesiredBodyYaw = TargetYaw - RetainedHeadYaw;
            const FRotator DesiredRotation(0.0f, DesiredBodyYaw, 0.0f);
            const FRotator NewRotation = FMath::RInterpConstantTo(
                NPC.GetActorRotation(), DesiredRotation, DeltaTime, 180.0f);
            NPC.SetActorRotation(NewRotation);
        }
    }
    else if (NPC.bSoundBodyTurnActive)
    {
        NPC.bSoundBodyTurnActive = false;
        MoveComp->bOrientRotationToMovement = NPC.bSavedOrientRotationToMovement;
        MoveComp->bUseControllerDesiredRotation = NPC.bSavedUseControllerDesiredRotation;

        if (AAIController* AIC = Cast<AAIController>(NPC.GetController()))
        {
            AIC->ClearFocus(EAIFocusPriority::Gameplay);
        }
    }
}

void UNPCGazeMotorComponent::UpdateHeadLookOffsets(ANPCCharacterBase& NPC, float DeltaTime) const
{
    float DesiredYaw = 0.0f;
    float DesiredPitch = 0.0f;

    if (NPC.bIsLooking && !NPC.LookAtLocation.IsNearlyZero())
    {
        const FVector ToTarget = NPC.LookAtLocation - NPC.GetActorLocation();
        if (!ToTarget.IsNearlyZero())
        {
            const FRotator LocalLookRotation =
                (ToTarget.Rotation() - NPC.GetActorRotation()).GetNormalized();
            DesiredYaw = FMath::Clamp(LocalLookRotation.Yaw, -70.0f, 70.0f);
            DesiredPitch = FMath::Clamp(LocalLookRotation.Pitch, -45.0f, 45.0f);
        }
    }

    NPC.HeadYaw = FMath::FInterpTo(NPC.HeadYaw, DesiredYaw, DeltaTime, 8.0f);
    NPC.HeadPitch = FMath::FInterpTo(NPC.HeadPitch, DesiredPitch, DeltaTime, 8.0f);
}

void UNPCGazeMotorComponent::UpdateReactiveMovement(ANPCCharacterBase& NPC) const
{
    AAIController* AIC = Cast<AAIController>(NPC.GetController());
    // --- [5] 몸 회전 로직 (75도 체크 유지) ---
    const bool bWinningReactiveGaze =
        NPC.CurrentGazeSource == EGazeSource::Safety || NPC.CurrentGazeSource == EGazeSource::Sound;
    if (AIC && NPC.bIsLooking && bWinningReactiveGaze)
    {
        FVector Forward = NPC.GetActorForwardVector();
        FVector DirectionToTarget = (NPC.LookAtLocation - NPC.GetActorLocation()).GetSafeNormal();
        Forward.Z = 0; DirectionToTarget.Z = 0;

        float DotProduct = FVector::DotProduct(Forward.GetSafeNormal(), DirectionToTarget.GetSafeNormal());
        float AngleDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(DotProduct, -1.0f, 1.0f)));

        // A selected sound reaction always pauses first, even when the head can
        // cover the angle without help from the body. Safety keeps its existing
        // large-angle-only pause rule.
        const bool bShouldPause = NPC.CurrentGazeSource == EGazeSource::Sound || AngleDegrees > 75.0f;
        if (bShouldPause)
        {
            if (NPC.CurrentGazeSource == EGazeSource::Safety)
            {
                AIC->SetFocalPoint(NPC.LookAtLocation);
            }
            else
            {
                AIC->ClearFocus(EAIFocusPriority::Gameplay);
            }
            NPC.bIsPausedByDanger = true;
            AIC->StopMovement();
        }
    }

    // --- [6] 안전 복귀 로직 ---
    if (!bWinningReactiveGaze && NPC.bIsPausedByDanger)
    {
        if (AIC) AIC->ClearFocus(EAIFocusPriority::Gameplay);
        NPC.ExecuteServerPlan();
        // Resume the waypoint that was interrupted. Moving the index backward
        // here made agents walk back to an already completed waypoint.
        NPC.RequestNextStep();
        NPC.bIsPausedByDanger = false;

        NPC.CurrentLookAtTimer = 0.0f;
    }

}
