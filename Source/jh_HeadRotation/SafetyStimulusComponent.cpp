#include "SafetyStimulusComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"

TArray<TWeakObjectPtr<USafetyStimulusComponent>> USafetyStimulusComponent::ActiveStimuli;

USafetyStimulusComponent::USafetyStimulusComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USafetyStimulusComponent::BeginPlay()
{
    Super::BeginPlay();
    ResolveTrackedPrimitive();
    LastSampledLocation = GetStimulusLocation();
    EstimatedVelocity = FVector::ZeroVector;
    bHasLocationSample = true;
    ActiveStimuli.AddUnique(this);
}

void USafetyStimulusComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    const FVector CurrentLocation = GetStimulusLocation();
    if (bHasLocationSample && DeltaTime > KINDA_SMALL_NUMBER)
    {
        EstimatedVelocity = (CurrentLocation - LastSampledLocation) / DeltaTime;
    }
    LastSampledLocation = CurrentLocation;
    bHasLocationSample = true;
}

void USafetyStimulusComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ActiveStimuli.Remove(this);
    Super::EndPlay(EndPlayReason);
}

FVector USafetyStimulusComponent::GetStimulusLocation() const
{
    if (const UPrimitiveComponent* Primitive = ResolveTrackedPrimitive())
    {
        return Primitive->Bounds.Origin;
    }

    const AActor* Owner = GetOwner();
    return Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
}

FVector USafetyStimulusComponent::GetStimulusVelocity() const
{
    if (const UPrimitiveComponent* Primitive = ResolveTrackedPrimitive())
    {
        const FVector ComponentVelocity = Primitive->GetComponentVelocity();
        return ComponentVelocity.IsNearlyZero(1.0f) ? EstimatedVelocity : ComponentVelocity;
    }

    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return FVector::ZeroVector;
    }

    const FVector ReportedVelocity = Owner->GetVelocity();
    return ReportedVelocity.IsNearlyZero(1.0f) ? EstimatedVelocity : ReportedVelocity;
}

UPrimitiveComponent* USafetyStimulusComponent::ResolveTrackedPrimitive() const
{
    if (TrackedPrimitive.IsValid())
    {
        return TrackedPrimitive.Get();
    }

    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return nullptr;
    }

    TArray<UPrimitiveComponent*> PrimitiveComponents;
    Owner->GetComponents<UPrimitiveComponent>(PrimitiveComponents);

    UPrimitiveComponent* BestPrimitive = nullptr;
    float BestSize = -1.0f;
    for (UPrimitiveComponent* Primitive : PrimitiveComponents)
    {
        if (!IsValid(Primitive) || !Primitive->IsRegistered() ||
            !Primitive->IsCollisionEnabled())
        {
            continue;
        }

        const float Size = Primitive->Bounds.SphereRadius;
        if (Size > BestSize)
        {
            BestSize = Size;
            BestPrimitive = Primitive;
        }
    }

    TrackedPrimitive = BestPrimitive;
    return BestPrimitive;
}

FString USafetyStimulusComponent::GetTrackedComponentName() const
{
    return GetNameSafe(ResolveTrackedPrimitive());
}

float USafetyStimulusComponent::GetMassFactor() const
{
    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return 0.0f;
    }

    float TotalMassKg = 0.0f;
    bool bHasSimulatedMass = false;
    const UPrimitiveComponent* Primitive = ResolveTrackedPrimitive();
    if (IsValid(Primitive) && Primitive->IsCollisionEnabled() &&
        Primitive->IsSimulatingPhysics())
    {
        TotalMassKg = FMath::Max(Primitive->GetMass(), 0.0f);
        bHasSimulatedMass = true;
    }

    if (bHasSimulatedMass)
    {
        // A saturating conversion avoids one heavy vehicle overwhelming every
        // other feature while preserving the ordering of light and heavy objects.
        return TotalMassKg / (TotalMassKg + 500.0f);
    }

    // Spline, Timeline and manually moved actors usually do not simulate
    // physics. Use their bounds as a stable impact proxy without warnings.
    FVector BoundsOrigin;
    FVector BoundsExtent;
    Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent);
    const float SizeRadius = BoundsExtent.Size();
    return SizeRadius / (SizeRadius + 300.0f);
}

float USafetyStimulusComponent::GetCollisionRadius() const
{
    if (const UPrimitiveComponent* Primitive = ResolveTrackedPrimitive())
    {
        const FVector BoundsExtent = Primitive->Bounds.BoxExtent;
        return FMath::Max(FVector2D(BoundsExtent.X, BoundsExtent.Y).Size(), 50.0f);
    }

    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return 100.0f;
    }

    FVector BoundsOrigin;
    FVector BoundsExtent;
    Owner->GetActorBounds(true, BoundsOrigin, BoundsExtent);
    return FMath::Max(FVector2D(BoundsExtent.X, BoundsExtent.Y).Size(), 50.0f);
}

float USafetyStimulusComponent::GetGaussianSpread() const
{
    // Twice the horizontal bounds radius gives large objects a wider spatial
    // influence while retaining a useful minimum for small props and people.
    return FMath::Max(GetCollisionRadius() * 2.0f, 200.0f);
}

void USafetyStimulusComponent::GetActiveStimuli(
    const UWorld* World, TArray<USafetyStimulusComponent*>& OutStimuli)
{
    OutStimuli.Reset();

    for (int32 Index = ActiveStimuli.Num() - 1; Index >= 0; --Index)
    {
        USafetyStimulusComponent* Stimulus = ActiveStimuli[Index].Get();
        if (!IsValid(Stimulus))
        {
            ActiveStimuli.RemoveAtSwap(Index);
            continue;
        }

        if (Stimulus->bSafetyEnabled && Stimulus->GetWorld() == World)
        {
            OutStimuli.Add(Stimulus);
        }
    }
}
