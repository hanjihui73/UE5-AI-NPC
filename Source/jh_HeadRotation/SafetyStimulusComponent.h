#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SafetyStimulusComponent.generated.h"

class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ESafetyStimulusType : uint8
{
    Unknown     UMETA(DisplayName = "Unknown"),
    Person      UMETA(DisplayName = "Person"),
    Object      UMETA(DisplayName = "Object"),
    Vehicle     UMETA(DisplayName = "Vehicle"),
    Environment UMETA(DisplayName = "Environment")
};

/**
 * Attach this component to any actor that can become a physical safety stimulus.
 * The component reports objective properties; each observing NPC computes its own
 * danger score from distance, motion and persona.
 */
UCLASS(ClassGroup = (Safety), meta = (BlueprintSpawnableComponent))
class JH_HEADROTATION_API USafetyStimulusComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    USafetyStimulusComponent();

    virtual void TickComponent(
        float DeltaTime,
        ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Safety", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float BaseDanger = 0.15f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Safety")
    ESafetyStimulusType StimulusType = ESafetyStimulusType::Unknown;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Safety")
    bool bSafetyEnabled = true;

    UFUNCTION(BlueprintPure, Category = "Safety")
    FVector GetStimulusLocation() const;

    UFUNCTION(BlueprintPure, Category = "Safety")
    FVector GetStimulusVelocity() const;

    /** Mass converted to a stable 0..1 factor. Read automatically from primitive components. */
    UFUNCTION(BlueprintPure, Category = "Safety")
    float GetMassFactor() const;

    /** Horizontal collision radius inferred from the owning actor's world bounds. */
    UFUNCTION(BlueprintPure, Category = "Safety")
    float GetCollisionRadius() const;

    /** GMM spread inferred from the owning actor's world bounds. */
    UFUNCTION(BlueprintPure, Category = "Safety")
    float GetGaussianSpread() const;

    UFUNCTION(BlueprintPure, Category = "Safety|Debug")
    FString GetTrackedComponentName() const;

    static void GetActiveStimuli(const UWorld* World, TArray<USafetyStimulusComponent*>& OutStimuli);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    static TArray<TWeakObjectPtr<USafetyStimulusComponent>> ActiveStimuli;

    UPrimitiveComponent* ResolveTrackedPrimitive() const;

    FVector LastSampledLocation = FVector::ZeroVector;
    FVector EstimatedVelocity = FVector::ZeroVector;
    bool bHasLocationSample = false;
    mutable TWeakObjectPtr<UPrimitiveComponent> TrackedPrimitive;
};
