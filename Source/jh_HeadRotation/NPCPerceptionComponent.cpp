#include "NPCPerceptionComponent.h"
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

UNPCPerceptionComponent::UNPCPerceptionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

bool UNPCPerceptionComponent::CanObserveNPC(const ANPCCharacterBase& NPC, const ANPCCharacterBase* OtherNPC, bool& bOutInFOV, bool& bOutHasLineOfSight) const
{
    bOutInFOV = false;
    bOutHasLineOfSight = false;

    if (!OtherNPC || OtherNPC == (&NPC) || !NPC.GetWorld())
    {
        return false;
    }

    const FVector ToOther = OtherNPC->GetActorLocation() - NPC.GetActorLocation();
    const float DistanceSquared = ToOther.SizeSquared2D();
    if (DistanceSquared > FMath::Square(NPC.SocialObservationRadius))
    {
        return false;
    }

    FVector FlatDirection = ToOther;
    FlatDirection.Z = 0.0f;
    FlatDirection.Normalize();

    FVector FlatForward = NPC.GetActorForwardVector();
    FlatForward.Z = 0.0f;
    FlatForward.Normalize();

    const float MinimumDot = FMath::Cos(FMath::DegreesToRadians(NPC.SocialObservationHalfAngle));
    bOutInFOV = FVector::DotProduct(FlatForward, FlatDirection) >= MinimumDot;

    const FVector TraceStart = NPC.GetActorLocation() + FVector(0.0f, 0.0f, 60.0f);
    const FVector TraceEnd = OtherNPC->GetActorLocation() + FVector(0.0f, 0.0f, 60.0f);

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SocialObservation), false, (&NPC));
    QueryParams.AddIgnoredActor(OtherNPC);

    FHitResult HitResult;
    const bool bBlocked = NPC.GetWorld()->LineTraceSingleByChannel(
        HitResult,
        TraceStart,
        TraceEnd,
        ECC_Visibility,
        QueryParams);

    bOutHasLineOfSight = !bBlocked;
    return bOutInFOV && bOutHasLineOfSight;
}

void UNPCPerceptionComponent::UpdateSocialObservations(ANPCCharacterBase& NPC) const
{
    if (!NPC.bEnableSocialObservation || !NPC.GetWorld())
    {
        NPC.VisibleSocialNPCs.Reset();
        return;
    }

    const float Now = NPC.GetWorld()->GetTimeSeconds();
    if (Now < NPC.NextSocialObservationTime)
    {
        return;
    }
    NPC.NextSocialObservationTime = Now + NPC.SocialObservationInterval;

    NPC.VisibleSocialNPCs.Reset();

    TArray<AActor*> NPCActors;
    UGameplayStatics::GetAllActorsOfClass(NPC.GetWorld(), ANPCCharacterBase::StaticClass(), NPCActors);

    int32 InRadiusCount = 0;
    int32 OutsideFOVCount = 0;
    int32 BlockedLOSCount = 0;

    for (AActor* Actor : NPCActors)
    {
        ANPCCharacterBase* OtherNPC = Cast<ANPCCharacterBase>(Actor);
        if (!OtherNPC || OtherNPC == (&NPC))
        {
            continue;
        }

        const bool bInRadius = FVector::DistSquared2D(
            NPC.GetActorLocation(), OtherNPC->GetActorLocation()) <=
            FMath::Square(NPC.SocialObservationRadius);
        if (!bInRadius)
        {
            continue;
        }
        ++InRadiusCount;

        bool bInFOV = false;
        bool bHasLineOfSight = false;
        const bool bCanObserve = NPC.CanObserveNPC(OtherNPC, bInFOV, bHasLineOfSight);

        if (!bInFOV)
        {
            ++OutsideFOVCount;
        }
        else if (!bHasLineOfSight)
        {
            ++BlockedLOSCount;
        }

        if (bCanObserve)
        {
            NPC.VisibleSocialNPCs.Add(OtherNPC);
        }

    }

    if (NPC.bVisualizeSocialObservation)
    {
        const FVector DebugTextLocation = NPC.GetActorLocation() +
            FVector(0.0f, 0.0f, NPC.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 45.0f);
        DrawDebugString(
            NPC.GetWorld(),
            DebugTextLocation,
            FString::Printf(TEXT("Social Range: %d"), NPC.VisibleSocialNPCs.Num()),
            nullptr,
            FColor::Cyan,
            NPC.SocialObservationInterval * 1.5f,
            false,
            0.75f);

        if (InRadiusCount != NPC.LastDebugInRadiusCount ||
            OutsideFOVCount != NPC.LastDebugOutsideFOVCount ||
            BlockedLOSCount != NPC.LastDebugBlockedLOSCount ||
            NPC.VisibleSocialNPCs.Num() != NPC.LastDebugVisibleCount)
        {
            UE_LOG(LogTemp, Log,
                TEXT("[%s][SocialDebug] InRadius=%d OutsideFOV=%d BlockedLOS=%d Visible=%d Radius=%.1f HalfAngle=%.1f"),
                *NPC.GetName(), InRadiusCount, OutsideFOVCount, BlockedLOSCount,
                NPC.VisibleSocialNPCs.Num(), NPC.SocialObservationRadius, NPC.SocialObservationHalfAngle);

            NPC.LastDebugInRadiusCount = InRadiusCount;
            NPC.LastDebugOutsideFOVCount = OutsideFOVCount;
            NPC.LastDebugBlockedLOSCount = BlockedLOSCount;
            NPC.LastDebugVisibleCount = NPC.VisibleSocialNPCs.Num();
        }
    }
}

void UNPCPerceptionComponent::HandleSoundDetection(ANPCCharacterBase& NPC, FVector SoundLocation) const
{
    // 소리 위치 저장
    NPC.SensedSoundLocation = SoundLocation;
    NPC.bIsSoundDetected = true;

    NPC.SoundDetectedTime = NPC.GetWorld()->GetTimeSeconds();

    UE_LOG(LogTemp, Warning, TEXT("[%s] detect sound at time: %f"), *NPC.GetName(), NPC.SoundDetectedTime);
}

void UNPCPerceptionComponent::CollectSafetyStimuli(
    const ANPCCharacterBase& NPC, TArray<USafetyStimulusComponent*>& OutStimuli) const
{
    USafetyStimulusComponent::GetActiveStimuli(NPC.GetWorld(), OutStimuli);
}
