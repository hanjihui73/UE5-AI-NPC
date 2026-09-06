#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DecideTypes.h"                 // FDecideResult 여기서 가져옴
#include "GoalManager.h"
#include "VisionCaptureActor.generated.h"


USTRUCT(BlueprintType)
struct FPlanItemArray
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	TArray<FPlanItem> Items;
};

USTRUCT(BlueprintType)
struct FBestBBox
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float X1 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float Y1 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float X2 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float Y2 = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float Confidence = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool  bValid = false;
};

USTRUCT(BlueprintType)
struct FResolvedBBoxTarget
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FString Label;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) FVector WorldLocation = FVector::ZeroVector;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<AActor> HitActor = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) float Confidence = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 CapturePointIndex = INDEX_NONE;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bResolved = false;
};

USTRUCT(BlueprintType)
struct FResolvedBBoxTargetArray
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TArray<FResolvedBBoxTarget> Items;
};

struct FCaptureProjectionSnapshot
{
	FTransform Transform = FTransform::Identity;
	float HorizontalFOV = 90.f;
	float AspectRatio = 16.f / 9.f;
};

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class AGoalManager;

// 델리게이트 선언
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDecideResult, const FDecideResult&, Result); //NPC가 바인딩할 이벤트
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAnalyzeOnlyResult, const FString&, AnalyzeJson); // NPC 말고 한 번 시뮬레이션 돌릴거 (AnalyzeOnly)
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLocalCacheFound, const TArray<FPlanItem>&, CachedPlan); // 로컬 기억에서 데이터를 바로 NPC에게 쏴주기 위한 델리게이트 (서버 패스용)


UCLASS()
class JH_HEADROTATION_API AVisionCaptureActor : public AActor
{
	GENERATED_BODY()

public:
	AVisionCaptureActor();

	UFUNCTION(BlueprintCallable, Category = "Vision")
	void RequestSnapshot(const FString& PromptText,float ETA=0.0f);

	//decide결과
	UPROPERTY(BlueprintAssignable, Category = "VLM")
	FOnDecideResult OnDecideResult;

	//planning용 analyze-only결과
	UPROPERTY(BlueprintAssignable, Category = "VLM")
	FOnAnalyzeOnlyResult OnAnalyzeOnlyResult;

	// [ADDED] 로컬 기억을 찾았을 때 발생하는 이벤트
	UPROPERTY(BlueprintAssignable, Category = "Vision|Memory")
	FOnLocalCacheFound OnLocalCacheFound;

	//NPC에 전달하는 함수 선언
	UFUNCTION(BlueprintImplementableEvent, Category = "Vision|Look")
	void ApplyLookToNPC(float LookYaw, float LookPitch, float HeadAlpha);

	UFUNCTION(BlueprintCallable, Category = "Vision")
	void RequestSnapshotAnalyzeOnly(int32 PointIndex, float ETA);

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Vision|Target")
	AActor* TargetNPC = nullptr;

	// 서버에서 결과를 받은 후 캐시를 업데이트할 때 부를 함수
	UFUNCTION(BlueprintCallable, Category = "Vision|Memory")
	void UpdateMemory(int32 Index, const TArray<FPlanItem>& NewPlans, const FString& NewFingerprint);


	UFUNCTION(BlueprintCallable, Category = "Vision|Scanning")
	FString GenerateSceneFingerprint();

	// 매니저 참조 (에디터에서 할당하거나 BeginPlay에서 찾기)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision|Manager")
	AGoalManager* MyGoalManager;

	// false=B안(공용 분석 1회 + 통합 계획 1회), true=A안(성격 3개 독립 분석 + 독립 계획)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vision|Experiment")
	bool bUseDecentralizedExperiment = false;

	// 이번 스캔 과정에서 서버에 사진을 보낸 적이 있는지 체크
	bool bHasSentAnyImageToServer = false;

	// 스캔 시작 전 플래그 리셋용
	void ResetServerRequestStatus() { bHasSentAnyImageToServer = false; }

	// 현재 스캔 중인 인덱스를 외부(GoalManager)에서 읽을 수 있게 해주는 함수
	UFUNCTION(BlueprintCallable, Category = "Vision")
	int32 GetCurrentPointIndex() const { return CurrentPointIndex; }

	// First-stage bbox resolver output. NPC gaze does not consume this yet.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Vision|BBox")
	bool GetResolvedBBoxTarget(const FString& Label, FResolvedBBoxTarget& OutTarget) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Vision|BBox")
	bool GetResolvedBBoxTargetAtPoint(int32 PointIndex, const FString& Label, FResolvedBBoxTarget& OutTarget) const;

	bool ResolvePlanBBoxToWorld(
		int32 PointIndex,
		float X1,
		float Y1,
		float X2,
		float Y2,
		FVector& OutWorldLocation,
		AActor*& OutHitActor,
		int32& OutHitVotes) const;

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vision|Memory")
	TMap<int32, FPlanItemArray> PlanCache;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vision|Memory")
	TMap<int32, FString> FingerprintCache;

private:
	UPROPERTY(VisibleAnywhere, Category = "Vision")
	USceneComponent* RootComp;

	UPROPERTY(VisibleAnywhere, Category = "Vision")
	USceneCaptureComponent2D* CaptureComp;

	UPROPERTY(EditAnywhere, Category = "Vision")
	UTextureRenderTarget2D* CaptureRT;

	UPROPERTY(EditAnywhere, Category = "Vision|Throttle")
	float CooldownSeconds = 0.3f;

	UPROPERTY()
	TMap<FString, FBestBBox> LastBestBBoxByLabel;
	void UpdateLastBestBBoxesFromAnalyzeJson(const FString& AnalyzeJson, int32 CapturePointIndex);

	UPROPERTY(VisibleAnywhere, Category = "Vision|BBox")
	TMap<FString, FResolvedBBoxTarget> ResolvedBBoxTargets;

	UPROPERTY(VisibleAnywhere, Category = "Vision|BBox")
	TMap<int32, FResolvedBBoxTargetArray> ResolvedBBoxTargetsByPoint;
	TMap<int32, FCaptureProjectionSnapshot> CaptureProjectionByPoint;

	FTransform LastCaptureTransform = FTransform::Identity;
	float LastCaptureHorizontalFOV = 90.f;
	float LastCaptureAspectRatio = 16.f / 9.f;
	int32 LastCapturePointIndex = INDEX_NONE;
	bool bHasCaptureProjection = false;
	FString LastCapturePngPath;

	void SaveCaptureProjection(int32 PointIndex);
	bool ResolveBBoxToWorld(const FString& Label, const FBestBBox& BBox, FResolvedBBoxTarget& OutTarget) const;
	bool TraceNormalizedBBox(
		const FCaptureProjectionSnapshot& Projection,
		float X1,
		float Y1,
		float X2,
		float Y2,
		float DebugLifeTime,
		FVector& OutWorldLocation,
		AActor*& OutHitActor,
		int32& OutHitVotes) const;
	bool SaveBBoxDebugImage() const;


	bool bInFlight = false;
	double LastRequestTime = -1.0;

	int32 CurrentPointIndex = -1; //현재 스캔 중인 지점 인덱스 저장용

	bool CanSendNow(bool bAnalyzeOnly) const;
	void MarkRequestStarted();
	void MarkRequestFinished();
	void SyncPostProcessFromPlayerCamera();

	bool SaveRenderTargetToPngFile(const FString& Filename);
	bool PostPngFileToServer(const FString& PngPath, const FString& PromptText, float ETA);
	bool PostDecideToServer(const FString& Mode, const TArray<FString>& Candidates, const FString& ObservationJson);

	
};
