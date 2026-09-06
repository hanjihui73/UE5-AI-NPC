#include "VisionCaptureActor.h"

#include "UVisionSaveGame.h"          
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget.h"
#include "Kismet/KismetRenderingLibrary.h"

#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "RenderingThread.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Serialization/BufferArchive.h"

#include "GoalManager.h"
#include "DrawDebugHelpers.h"

AVisionCaptureActor::AVisionCaptureActor()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(RootComp);

	CaptureComp = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureComp"));
	CaptureComp->SetupAttachment(RootComp);

    //罹≪퀜???붿껌???ㅼ뼱???꾨쭔 ?섎룞?쇰줈(g?뚮?????
    //留ㅽ봽?덉엫?섎㈃ ?덈Т 鍮꾩슜???щ땲源?
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
}


//ScreneCapture2D媛 李띿? ?붾㈃??rt????μ떆耳쒖꽌 ?섏쨷???쒕쾭??蹂대궪 ???ъ슜 ?덉젙
void AVisionCaptureActor::BeginPlay()
{
	Super::BeginPlay();

    // ?붾뱶?먯꽌 GoalManager ?먮룞?쇰줈 李얠븘???곌껐?섍린
    if (!MyGoalManager)
    {
        MyGoalManager = Cast<AGoalManager>(UGameplayStatics::GetActorOfClass(GetWorld(), AGoalManager::StaticClass()));
        if (MyGoalManager)
        {
            UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Successfully found GoalManager!"));
        }
    }

    // SaveGames 湲곕컲 VisionMemory 罹먯떆???ъ슜?섏? ?딆뒿?덈떎.
    // PNG 罹≪쿂 ?뚯씪? 洹몃?濡???ν븯吏留? ?대뵒瑜?蹂쇱?/?뚮옖 寃쎈줈 罹먯떆???붿뒪?ъ뿉 ??ν븯嫄곕굹 濡쒕뱶?섏? ?딆뒿?덈떎.
    PlanCache.Empty();
    FingerprintCache.Empty();
    UE_LOG(LogTemp, Warning, TEXT("[VisionMemory] SaveGame cache disabled. Skipping disk load."));

    if (CaptureComp && CaptureRT)
    {
        CaptureComp->TextureTarget = CaptureRT;
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] BeginPlay: CaptureComp or CaptureRT is null (CaptureComp=%s, CaptureRT=%s)"),
            CaptureComp ? TEXT("OK") : TEXT("NULL"),
            CaptureRT ? *CaptureRT->GetName() : TEXT("NULL"));
    }
    // --- Exposure / PostProcess 怨좎젙 ---
    CaptureComp->bAlwaysPersistRenderingState = true;


    // UE5?먯꽌 EyeAdaptation???곕줈 ?곸슜?섎뒗 寃쎌슦???덉뼱??異붽?濡??덉젙??

}

//吏湲??쒕쾭??蹂대궡???섎뒗吏 ?뺤씤?섎뒗 肄붾뱶
bool AVisionCaptureActor::CanSendNow(bool bAnalyzeOnly) const
{
    if (bInFlight) return false;

    if (bAnalyzeOnly)
    {
        return true;
    }

    const double Now = FPlatformTime::Seconds();
    if (LastRequestTime > 0.0 && (Now - LastRequestTime) < CooldownSeconds)
    {
        return false;
    }
    return true;
}

void AVisionCaptureActor::MarkRequestStarted()
{
	bInFlight = true;
	LastRequestTime = FPlatformTime::Seconds();
}

void AVisionCaptureActor::MarkRequestFinished()
{
	bInFlight = false;
}

void AVisionCaptureActor::SyncPostProcessFromPlayerCamera()
{
    if (!CaptureComp) return;

    APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(this, 0);
    if (!CameraManager)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] PlayerCameraManager not found; keeping capture post process."));
        return;
    }

    const FMinimalViewInfo& PlayerView = CameraManager->GetCameraCacheView();
    CaptureComp->PostProcessSettings = PlayerView.PostProcessSettings;
    CaptureComp->PostProcessBlendWeight = PlayerView.PostProcessBlendWeight;
}

void AVisionCaptureActor::RequestSnapshot(const FString& PromptText, float ETA)
{
    if (!CanSendNow(false)) return;
    if (!CaptureRT || !CaptureComp) return;

    MarkRequestStarted();
    SaveCaptureProjection(INDEX_NONE);
    SyncPostProcessFromPlayerCamera();

    // 1) RT??罹≪쿂 ?붿껌
    CaptureComp->CaptureScene();

    // 2) ?뚮뜑 ?꾨즺源뚯? ?뺤떎???湲?寃??붾㈃?대굹 ?대젃寃??ㅻⅤ寃??섏삤?붽굅 諛⑹?)
    FRenderCommandFence Fence;
    Fence.BeginFence();
    Fence.Wait();

    // 3) ???
    const FString FileName = FString::Printf(TEXT("capture_%lld.png"), FDateTime::UtcNow().ToUnixTimestamp());
    const FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), FileName);

    const bool bOK = SaveRenderTargetToPngFile(SavePath);
    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Saved PNG: %s (ok=%d)"), *SavePath, bOK ? 1 : 0);

    if (bOK)
    {
        LastCapturePngPath = SavePath;
        // ????깃났 ???쒕쾭 ?꾩넚 ?쒖옉
        PostPngFileToServer(SavePath, PromptText, ETA);
    }
    else
    {
        // ????ㅽ뙣硫??붿껌 ??
        MarkRequestFinished();
    }
}

void AVisionCaptureActor::RequestSnapshotAnalyzeOnly(int32 PointIndex, float ETA)
{
    if (!CanSendNow(true)) return;
    if (!CaptureRT || !CaptureComp) return;

    if (PointIndex == 0)
    {
        ResolvedBBoxTargetsByPoint.Reset();
        CaptureProjectionByPoint.Reset();
    }

    CurrentPointIndex = PointIndex;     //?꾩옱 ?붿껌 以묒씤 ?몃뜳??????섏쨷???쒕쾭 ?묐떟 ??留ㅼ묶)

    MarkRequestStarted();
    SaveCaptureProjection(PointIndex);
    SyncPostProcessFromPlayerCamera();
    CaptureComp->CaptureScene();

    FRenderCommandFence Fence;
    Fence.BeginFence();
    Fence.Wait();

    // SaveGames/Memory 罹먯떆 湲곕뒫???뺣땲??
    // ?댁쟾 湲곕줉怨?鍮꾧탳?섏? ?딄퀬 留ㅻ쾲 ?쒕쾭濡??대?吏瑜?蹂대깄?덈떎.
    bHasSentAnyImageToServer = true;
    UE_LOG(LogTemp, Warning, TEXT("[VisionMemory] Cache disabled. Sending Index %d to Server."), PointIndex);

    const FString FileName = FString::Printf(TEXT("capture_plan_%d.png"), PointIndex);
    const FString SavePath = FPaths::Combine(FPaths::ProjectSavedDir(), FileName);

    const bool bOK = SaveRenderTargetToPngFile(SavePath);

    if (bOK)
    {
        LastCapturePngPath = SavePath;
        PostPngFileToServer(SavePath, TEXT(""), ETA);
    }
    else
    {
        MarkRequestFinished();
    }
}

bool AVisionCaptureActor::SaveRenderTargetToPngFile(const FString& Filename)
{
    if (!CaptureRT) return false;

    FTextureRenderTargetResource* RTRes = CaptureRT->GameThread_GetRenderTargetResource();
    if (!RTRes) return false;

    const int32 W = CaptureRT->SizeX;
    const int32 H = CaptureRT->SizeY;

    TArray<FColor> Pixels;

    // UNorm + 媛먮쭏 蹂댁젙
    FReadSurfaceDataFlags Flags(RCM_UNorm);
    Flags.SetLinearToGamma(true);  // ?듭떖
    Flags.SetOutputStencil(false);

    if (!RTRes->ReadPixels(Pixels, Flags))
    {
        UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] ReadPixels failed"));
        return false;
    }

    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    if (!Wrapper.IsValid() ||
        !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), W, H, ERGBFormat::BGRA, 8))
    {
        UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] ImageWrapper SetRaw failed"));
        return false;
    }

    const TArray64<uint8>& PngData = Wrapper->GetCompressed(100);
    return FFileHelper::SaveArrayToFile(PngData, *Filename);
}

bool AVisionCaptureActor::PostPngFileToServer(const FString& PngPath, const FString& PromptText, float ETA)
{
    // 1) ?뚯씪??諛붿씠?몃줈 ?쎄린
    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *PngPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] Failed to read PNG file: %s"), *PngPath);
        return false;
    }

    // 2) multipart 諛붿슫?붾━/諛붾뵒 援ъ꽦
    const FString Boundary = TEXT("----UEBoundary7MA4YWxkTrZu0gW");
    const FString LineEnd = TEXT("\r\n");

    TArray<uint8> Body;
    Body.Reserve(FileBytes.Num() + 2048);

    auto AppendString = [&](const FString& S)
        {
            FTCHARToUTF8 Conv(*S);
            Body.Append((uint8*)Conv.Get(), Conv.Length());
        };

    if (!PromptText.IsEmpty())
    {
        AppendString(TEXT("--") + Boundary + LineEnd);
        AppendString(TEXT("Content-Disposition: form-data; name=\"prompt\"") + LineEnd + LineEnd);
        AppendString(PromptText + LineEnd);
    }

    AppendString(TEXT("--") + Boundary + LineEnd);
    AppendString(TEXT("Content-Disposition: form-data; name=\"file\"; filename=\"capture.png\"") + LineEnd);
    AppendString(TEXT("Content-Type: image/png") + LineEnd + LineEnd);
    Body.Append(FileBytes);
    AppendString(LineEnd);
    AppendString(TEXT("--") + Boundary + TEXT("--") + LineEnd);

    // 3) HTTP ?붿껌 ?앹꽦
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
    Req->SetURL(TEXT("http://127.0.0.1:8000/analyze"));
    Req->SetVerb(TEXT("POST"));
    Req->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary);
    Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
    Req->SetHeader(TEXT("X-ETA-Seconds"), FString::SanitizeFloat(ETA));
    Req->SetHeader(
        TEXT("X-Experiment-Architecture"),
        bUseDecentralizedExperiment ? TEXT("A") : TEXT("B"));
    Req->SetContent(Body);

    // 4) ?묐떟 肄쒕갚
    const int32 RequestCapturePointIndex = LastCapturePointIndex;
    Req->OnProcessRequestComplete().BindWeakLambda(
        this,
        [this, PromptText, RequestCapturePointIndex](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
        {
            if (!IsValid(this)) return;

            if (!bSuccess || !Response.IsValid())
            {
                UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] /analyze failed"));
                MarkRequestFinished();
                return;
            }

            const int32 Code = Response->GetResponseCode();
            const FString Text = Response->GetContentAsString();

            if (Code < 200 || Code >= 300)
            {
                MarkRequestFinished();
                return;
            }

            // Resolve detections for both route scans and interactive requests.
            UpdateLastBestBBoxesFromAnalyzeJson(Text, RequestCapturePointIndex);

            // 遺꾩꽍 寃곌낵 泥섎━
            if (PromptText.IsEmpty())
            {
                // [AnalyzeOnly 紐⑤뱶] 寃곗젙?섏? ?딄퀬 寃곌낵留?留ㅻ땲??먭쾶 ?꾨떖
                UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] AnalyzeOnly Mode: Broadcasting result."));
                OnAnalyzeOnlyResult.Broadcast(Text);
                MarkRequestFinished();
                return;
            }

            // [Decide 紐⑤뱶] 遺꾩꽍 寃곌낵濡?怨좉컻 ?뚮━湲?寃곗젙
            UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Decide Mode: Moving to PostDecideToServer with delay."));
            FString Mode = TEXT("crossing");
            TArray<FString> Candidates = { TEXT("look_forward"), TEXT("look_left"), TEXT("look_right"), TEXT("look_traffic_light") };

            // 5珥?吏???湲곕? ?꾪븳 ?몃━寃뚯씠??諛⑹떇 ?곸슜
            FTimerHandle TimerHandle;
            FTimerDelegate TimerDelegate;
            TimerDelegate.BindLambda([this, Mode, Candidates, Text]()
                {
                    if (!PostDecideToServer(Mode, Candidates, Text))
                    {
                        MarkRequestFinished();
                    }
                });

            // ?꾩옱 ?≫꽣??World?먯꽌 ??대㉧ ?ㅽ뻾
            GetWorld()->GetTimerManager().SetTimer(TimerHandle, TimerDelegate, 5.0f, false);
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Sending PNG to server... path=%s"), *PngPath);
    return Req->ProcessRequest();
}

//?댁젣 ?ш린??llm???곌껐?섎㈃??寃곗젙???섍쾶 ??
bool AVisionCaptureActor::PostDecideToServer(const FString& Mode, const TArray<FString>& Candidates, const FString& ObservationJson)
{
    // 1) observation JSON ?뚯떛 (臾몄옄??-> JSON ?ㅻ툕?앺듃)
    TSharedPtr<FJsonObject> ObsObj;
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ObservationJson);
        if (!FJsonSerializer::Deserialize(Reader, ObsObj) || !ObsObj.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] observation JSON parse failed"));
            return false;
        }
    }

    // 2) decide ?붿껌 JSON 留뚮뱾湲? { mode, candidates, observation }
    //llm? ?대?吏瑜?蹂댁? ?딄퀬 mode+candiates + observation??蹂닿퀬 ?좏깮
    TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
    RootObj->SetStringField(TEXT("mode"), Mode);

    TArray<TSharedPtr<FJsonValue>> CandArr;
    for (const FString& C : Candidates)
    {
        CandArr.Add(MakeShared<FJsonValueString>(C));
    }
    RootObj->SetArrayField(TEXT("candidates"), CandArr);
    RootObj->SetObjectField(TEXT("observation"), ObsObj);

    FString BodyString;
    {
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
        FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);
    }

    // 3) HTTP ?붿껌 ?앹꽦
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
    Req->SetURL(TEXT("http://127.0.0.1:8000/decide"));
    Req->SetVerb(TEXT("POST"));
    Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
    Req->SetContentAsString(BodyString);

    // 4) ?묐떟 肄쒕갚: ?ш린??decision JSON 諛쏆쓬
    Req->OnProcessRequestComplete().BindLambda(
        [this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
        {
            if (!bSuccess || !Response.IsValid())
            {
                UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] /decide failed (no response)"));
                MarkRequestFinished();
                return;
            }

            const int32 Code = Response->GetResponseCode();
            const FString Text = Response->GetContentAsString();
            UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] /decide HTTP %d: %s"), Code, *Text);

            if (Code < 200 || Code >= 300)
            {
                MarkRequestFinished();
                return;
            }

            // 1) JSON ?뚯떛 (?덉쟾)
            TSharedPtr<FJsonObject> Root;
            {
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
                if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
                {
                    UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] /decide JSON parse failed"));
                    MarkRequestFinished();
                    return;
                }
            }

            // 2) DecideResult 梨꾩슦湲?(TryGet + 湲곕낯媛믪쑝濡??덉쟾?섍쾶)
            FDecideResult R;

            // chosen_candidate
            {
                FString Chosen;
                if (!Root->TryGetStringField(TEXT("chosen_candidate"), Chosen))
                {
                    Chosen = TEXT("look_forward");
                }
                R.ChosenCandidate = Chosen;
            }

            // target_label
            {
                FString Target;
                if (!Root->TryGetStringField(TEXT("target_label"), Target))
                {
                    Target = TEXT("none");
                }
                R.TargetLabel = Target;
            }

            // hold_time / cooldown / confidence
            {
                double Num = 0.0;

                if (Root->TryGetNumberField(TEXT("hold_time"), Num))  R.HoldTime = (float)Num;
                else R.HoldTime = 1.0f;

                if (Root->TryGetNumberField(TEXT("cooldown"), Num))   R.Cooldown = (float)Num;
                else R.Cooldown = 1.5f;

                if (Root->TryGetNumberField(TEXT("confidence"), Num)) R.Confidence = (float)Num;
                else R.Confidence = 0.0f;

                // clamp (?쒕쾭???숈씪 踰붿쐞濡?
                R.HoldTime = FMath::Clamp(R.HoldTime, 0.6f, 1.5f);
                R.Cooldown = FMath::Clamp(R.Cooldown, 0.8f, 2.5f);
                R.Confidence = FMath::Clamp(R.Confidence, 0.0f, 1.0f);
            }

            // reason
            {
                FString Reason;
                if (!Root->TryGetStringField(TEXT("reason"), Reason))
                {
                    Reason = TEXT("");
                }
                R.Reason = Reason.Left(80);
            }

            // drivers (?놁뼱????二쎄쾶)
            {
                const TSharedPtr<FJsonObject>* DriversPtr = nullptr;
                if (Root->TryGetObjectField(TEXT("drivers"), DriversPtr) && DriversPtr && DriversPtr->IsValid())
                {
                    const TSharedPtr<FJsonObject>& DriversObj = *DriversPtr;

                    int32 V = 0;
                    if (DriversObj->TryGetNumberField(TEXT("Safety"), *(double*)&V)) {} // (?꾨옒?먯꽌 ?쒕?濡?泥섎━)
                    // UE JSON? int濡?吏곸젒 紐?諛쏆쓣 ?뚭? ?덉뼱??Number濡?諛쏄퀬 罹먯뒪??
                    auto GetInt0to5 = [&](const TCHAR* Key)->int32
                        {
                            double D = 0.0;
                            if (!DriversObj->TryGetNumberField(Key, D)) return 0;
                            int32 I = (int32)D;
                            return FMath::Clamp(I, 0, 5);
                        };

                    R.Safety = GetInt0to5(TEXT("Safety"));
                    R.Info = GetInt0to5(TEXT("Info"));
                    R.Interest = GetInt0to5(TEXT("Interest"));
                    R.Social = GetInt0to5(TEXT("Social"));
                    R.Habit = GetInt0to5(TEXT("Habit"));
                }
                else
                {
                    // drivers ?꾨씫 ???덉쟾 湲곕낯媛?
                    R.Safety = 2; R.Info = 1; R.Interest = 0; R.Social = 0; R.Habit = 1;
                }
            }

            // 3) 理쒖쥌 濡쒓렇 (?붾쾭源낆슜)
            UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] DecideResult chosen=%s target=%s hold=%.2f cd=%.2f conf=%.2f drivers=(%d,%d,%d,%d,%d)"),
                *R.ChosenCandidate, *R.TargetLabel, R.HoldTime, R.Cooldown, R.Confidence,
                R.Safety, R.Info, R.Interest, R.Social, R.Habit);

            //4) target_label濡?bbox媛 ?ㅼ젣濡?李얠븘吏?붿? 濡쒓렇濡??뺤씤?섍린
            if (!R.TargetLabel.IsEmpty() && !R.TargetLabel.Equals(TEXT("none"), ESearchCase::IgnoreCase))
            {
                if (FBestBBox* Found = LastBestBBoxByLabel.Find(R.TargetLabel))
                {
                    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Found bbox for %s: [%.3f %.3f %.3f %.3f] conf=%.2f"),
                        *R.TargetLabel, Found->X1, Found->Y1, Found->X2, Found->Y2, Found->Confidence);

                    const float Cx = (Found->X1 + Found->X2) * 0.5f;
                    const float Cy = (Found->Y1 + Found->Y2) * 0.5f;

                    const float Dx = Cx - 0.5f;
                    const float Dy = Cy - 0.5f;

                    const float HFOV = (CaptureComp ? CaptureComp->FOVAngle : 90.f);

                    float Aspect = 16.f / 9.f;
                    if (CaptureRT && CaptureRT->SizeY > 0)
                    {
                        Aspect = (float)CaptureRT->SizeX / (float)CaptureRT->SizeY;
                    }

                    const float HFOVRad = FMath::DegreesToRadians(HFOV);
                    const float VFOVRad = 2.f * FMath::Atan(FMath::Tan(HFOVRad * 0.5f) / Aspect);
                    const float VFOV = FMath::RadiansToDegrees(VFOVRad);

                    float LookYaw = Dx * HFOV;
                    float LookPitch = -Dy * VFOV;

                    LookYaw = FMath::Clamp(LookYaw, -60.f, 60.f);
                    LookPitch = FMath::Clamp(LookPitch, -30.f, 30.f);

                    const float HeadAlpha = FMath::Clamp(Found->Confidence, 0.f, 1.f);

                    ApplyLookToNPC(LookYaw, LookPitch, HeadAlpha);

                    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] ApplyLook yaw=%.1f pitch=%.1f alpha=%.2f (target=%s)"),
                        LookYaw, LookPitch, HeadAlpha, *R.TargetLabel);
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] No bbox found for target_label=%s (stored labels=%d)"),
                        *R.TargetLabel, LastBestBBoxByLabel.Num());
                }
            }

            // 5) NPC???뚮━湲?
            OnDecideResult.Broadcast(R);

            MarkRequestFinished();
        }
    );



    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Sending /decide JSON: %s"), *BodyString);

    const bool bOk = Req->ProcessRequest();
    if (!bOk)
    {
        UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] /decide ProcessRequest() returned false"));
        MarkRequestFinished();
    }
    return bOk;
}

void AVisionCaptureActor::SaveCaptureProjection(int32 PointIndex)
{
    if (!CaptureComp)
    {
        bHasCaptureProjection = false;
        return;
    }

    LastCaptureTransform = CaptureComp->GetComponentTransform();
    LastCaptureHorizontalFOV = CaptureComp->FOVAngle;
    LastCaptureAspectRatio = 16.f / 9.f;
    if (CaptureRT && CaptureRT->SizeX > 0 && CaptureRT->SizeY > 0)
    {
        LastCaptureAspectRatio = static_cast<float>(CaptureRT->SizeX) / static_cast<float>(CaptureRT->SizeY);
    }
    LastCapturePointIndex = PointIndex;
    bHasCaptureProjection = true;

    if (PointIndex != INDEX_NONE)
    {
        FCaptureProjectionSnapshot Snapshot;
        Snapshot.Transform = LastCaptureTransform;
        Snapshot.HorizontalFOV = LastCaptureHorizontalFOV;
        Snapshot.AspectRatio = LastCaptureAspectRatio;
        CaptureProjectionByPoint.Add(PointIndex, Snapshot);
    }
}

bool AVisionCaptureActor::TraceNormalizedBBox(
    const FCaptureProjectionSnapshot& Projection,
    float X1,
    float Y1,
    float X2,
    float Y2,
    float DebugLifeTime,
    FVector& OutWorldLocation,
    AActor*& OutHitActor,
    int32& OutHitVotes) const
{
    OutWorldLocation = FVector::ZeroVector;
    OutHitActor = nullptr;
    OutHitVotes = 0;

    if (!GetWorld() || X2 <= X1 || Y2 <= Y1)
    {
        return false;
    }

    X1 = FMath::Clamp(X1, 0.f, 1.f);
    Y1 = FMath::Clamp(Y1, 0.f, 1.f);
    X2 = FMath::Clamp(X2, 0.f, 1.f);
    Y2 = FMath::Clamp(Y2, 0.f, 1.f);

    const float CenterX = (X1 + X2) * 0.5f;
    const float CenterY = (Y1 + Y2) * 0.5f;
    const float HorizontalTangent = FMath::Tan(FMath::DegreesToRadians(Projection.HorizontalFOV * 0.5f));
    const float VerticalTangent = HorizontalTangent / FMath::Max(Projection.AspectRatio, KINDA_SMALL_NUMBER);
    const FVector RayStart = Projection.Transform.GetLocation();
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VisionBBoxResolve), true, this);

    struct FSampleHit
    {
        FHitResult Hit;
        float CenterDistanceSq = 0.f;
    };

    TArray<FSampleHit> SampleHits;
    TMap<AActor*, int32> HitCounts;
    const float SampleFractions[] = { 0.25f, 0.5f, 0.75f };

    for (const float YFraction : SampleFractions)
    {
        for (const float XFraction : SampleFractions)
        {
            const float SampleX = FMath::Lerp(X1, X2, XFraction);
            const float SampleY = FMath::Lerp(Y1, Y2, YFraction);
            const FVector LocalRay(
                1.f,
                (SampleX * 2.f - 1.f) * HorizontalTangent,
                (1.f - SampleY * 2.f) * VerticalTangent);
            const FVector WorldDirection = Projection.Transform.TransformVectorNoScale(LocalRay.GetSafeNormal());
            const FVector RayEnd = RayStart + WorldDirection * 100000.f;

            FHitResult Hit;
            if (GetWorld()->LineTraceSingleByChannel(Hit, RayStart, RayEnd, ECC_Visibility, QueryParams))
            {
                FSampleHit& SampleHit = SampleHits.AddDefaulted_GetRef();
                SampleHit.Hit = Hit;
                SampleHit.CenterDistanceSq = FVector2D::DistSquared(
                    FVector2D(SampleX, SampleY), FVector2D(CenterX, CenterY));
                HitCounts.FindOrAdd(Hit.GetActor())++;
            }
        }
    }

    AActor* BestActor = nullptr;
    int32 BestHitCount = 0;
    for (const TPair<AActor*, int32>& Pair : HitCounts)
    {
        if (Pair.Key && Pair.Value > BestHitCount)
        {
            BestActor = Pair.Key;
            BestHitCount = Pair.Value;
        }
    }

    const FSampleHit* BestSample = nullptr;
    for (const FSampleHit& SampleHit : SampleHits)
    {
        if (SampleHit.Hit.GetActor() == BestActor &&
            (!BestSample || SampleHit.CenterDistanceSq < BestSample->CenterDistanceSq))
        {
            BestSample = &SampleHit;
        }
    }

    if (!BestSample)
    {
        const FVector CenterLocalRay(
            1.f,
            (CenterX * 2.f - 1.f) * HorizontalTangent,
            (1.f - CenterY * 2.f) * VerticalTangent);
        const FVector CenterRayEnd = RayStart
            + Projection.Transform.TransformVectorNoScale(CenterLocalRay.GetSafeNormal()) * 100000.f;
        if (DebugLifeTime > 0.f)
        {
            DrawDebugLine(GetWorld(), RayStart, CenterRayEnd, FColor::Red, false, DebugLifeTime, 0, 1.5f);
        }
        return false;
    }

    OutWorldLocation = BestSample->Hit.ImpactPoint;
    OutHitActor = BestActor;
    OutHitVotes = BestHitCount;
    if (DebugLifeTime > 0.f)
    {
        DrawDebugLine(GetWorld(), RayStart, OutWorldLocation, FColor::Cyan, false, DebugLifeTime, 0, 1.5f);
        DrawDebugSphere(GetWorld(), OutWorldLocation, 18.f, 12, FColor::Cyan, false, DebugLifeTime);
    }
    return true;
}

bool AVisionCaptureActor::ResolveBBoxToWorld(
    const FString& Label,
    const FBestBBox& BBox,
    FResolvedBBoxTarget& OutTarget) const
{
    OutTarget = FResolvedBBoxTarget();
    OutTarget.Label = Label;
    OutTarget.Confidence = BBox.Confidence;
    OutTarget.CapturePointIndex = LastCapturePointIndex;

    if (!bHasCaptureProjection || !GetWorld() || !BBox.bValid)
    {
        return false;
    }

    FCaptureProjectionSnapshot Projection;
    Projection.Transform = LastCaptureTransform;
    Projection.HorizontalFOV = LastCaptureHorizontalFOV;
    Projection.AspectRatio = LastCaptureAspectRatio;
    AActor* BestActor = nullptr;
    int32 BestHitCount = 0;
    if (!TraceNormalizedBBox(
        Projection, BBox.X1, BBox.Y1, BBox.X2, BBox.Y2, 0.05f,
        OutTarget.WorldLocation, BestActor, BestHitCount))
    {
        UE_LOG(LogTemp, Warning, TEXT("[VisionBBox] MISS label=%s point=%d rays=9"),
            *Label, LastCapturePointIndex);
        return false;
    }

    OutTarget.HitActor = BestActor;
    OutTarget.bResolved = true;
    UE_LOG(LogTemp, Warning,
        TEXT("[VisionBBox] HIT label=%s point=%d actor=%s location=%s confidence=%.2f votes=%d/9"),
        *Label,
        LastCapturePointIndex,
        *GetNameSafe(BestActor),
        *OutTarget.WorldLocation.ToString(),
        BBox.Confidence,
        BestHitCount);
    return true;
}

bool AVisionCaptureActor::ResolvePlanBBoxToWorld(
    int32 PointIndex,
    float X1,
    float Y1,
    float X2,
    float Y2,
    FVector& OutWorldLocation,
    AActor*& OutHitActor,
    int32& OutHitVotes) const
{
    const FCaptureProjectionSnapshot* Projection = CaptureProjectionByPoint.Find(PointIndex);
    if (!Projection)
    {
        UE_LOG(LogTemp, Error, TEXT("[PlanBBox] Missing capture projection for point=%d"), PointIndex);
        OutWorldLocation = FVector::ZeroVector;
        OutHitActor = nullptr;
        OutHitVotes = 0;
        return false;
    }

    return TraceNormalizedBBox(
        *Projection, X1, Y1, X2, Y2, 0.f,
        OutWorldLocation, OutHitActor, OutHitVotes);
}

bool AVisionCaptureActor::GetResolvedBBoxTarget(const FString& Label, FResolvedBBoxTarget& OutTarget) const
{
    if (const FResolvedBBoxTarget* Found = ResolvedBBoxTargets.Find(Label))
    {
        OutTarget = *Found;
        return Found->bResolved;
    }

    for (const TPair<FString, FResolvedBBoxTarget>& Pair : ResolvedBBoxTargets)
    {
        if (Pair.Key.Equals(Label, ESearchCase::IgnoreCase))
        {
            OutTarget = Pair.Value;
            return Pair.Value.bResolved;
        }
    }

    OutTarget = FResolvedBBoxTarget();
    return false;
}

bool AVisionCaptureActor::GetResolvedBBoxTargetAtPoint(
    int32 PointIndex,
    const FString& Label,
    FResolvedBBoxTarget& OutTarget) const
{
    const FResolvedBBoxTargetArray* PointTargets = ResolvedBBoxTargetsByPoint.Find(PointIndex);
    if (!PointTargets)
    {
        OutTarget = FResolvedBBoxTarget();
        return false;
    }

    for (const FResolvedBBoxTarget& Target : PointTargets->Items)
    {
        if (Target.Label.Equals(Label, ESearchCase::IgnoreCase))
        {
            OutTarget = Target;
            return Target.bResolved;
        }
    }

    OutTarget = FResolvedBBoxTarget();
    return false;
}

bool AVisionCaptureActor::SaveBBoxDebugImage() const
{
    if (LastCapturePngPath.IsEmpty() || LastBestBBoxByLabel.Num() == 0)
    {
        return false;
    }

    TArray<uint8> CompressedData;
    if (!FFileHelper::LoadFileToArray(CompressedData, *LastCapturePngPath))
    {
        return false;
    }

    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> Decoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!Decoder.IsValid() ||
        !Decoder->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
    {
        return false;
    }

    TArray64<uint8> RawData;
    if (!Decoder->GetRaw(ERGBFormat::BGRA, 8, RawData))
    {
        return false;
    }

    const int32 Width = static_cast<int32>(Decoder->GetWidth());
    const int32 Height = static_cast<int32>(Decoder->GetHeight());
    if (Width <= 0 || Height <= 0 || RawData.Num() < static_cast<int64>(Width) * Height * 4)
    {
        return false;
    }

    FColor* Pixels = reinterpret_cast<FColor*>(RawData.GetData());
    const int32 Thickness = FMath::Clamp(FMath::Min(Width, Height) / 180, 2, 6);

    for (const TPair<FString, FBestBBox>& Pair : LastBestBBoxByLabel)
    {
        const FBestBBox& Box = Pair.Value;
        const int32 X1 = FMath::Clamp(FMath::RoundToInt(Box.X1 * (Width - 1)), 0, Width - 1);
        const int32 Y1 = FMath::Clamp(FMath::RoundToInt(Box.Y1 * (Height - 1)), 0, Height - 1);
        const int32 X2 = FMath::Clamp(FMath::RoundToInt(Box.X2 * (Width - 1)), 0, Width - 1);
        const int32 Y2 = FMath::Clamp(FMath::RoundToInt(Box.Y2 * (Height - 1)), 0, Height - 1);
        const uint32 Hash = GetTypeHash(Pair.Key);
        const FColor Color(
            static_cast<uint8>(80 + (Hash & 0x7f)),
            static_cast<uint8>(80 + ((Hash >> 8) & 0x7f)),
            static_cast<uint8>(80 + ((Hash >> 16) & 0x7f)),
            255);

        for (int32 T = 0; T < Thickness; ++T)
        {
            const int32 Left = FMath::Clamp(X1 + T, 0, Width - 1);
            const int32 Right = FMath::Clamp(X2 - T, 0, Width - 1);
            const int32 Top = FMath::Clamp(Y1 + T, 0, Height - 1);
            const int32 Bottom = FMath::Clamp(Y2 - T, 0, Height - 1);

            for (int32 X = Left; X <= Right; ++X)
            {
                Pixels[Top * Width + X] = Color;
                Pixels[Bottom * Width + X] = Color;
            }
            for (int32 Y = Top; Y <= Bottom; ++Y)
            {
                Pixels[Y * Width + Left] = Color;
                Pixels[Y * Width + Right] = Color;
            }
        }
    }

    TSharedPtr<IImageWrapper> Encoder = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!Encoder.IsValid() ||
        !Encoder->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, ERGBFormat::BGRA, 8))
    {
        return false;
    }

    const FString DebugPath = FPaths::Combine(
        FPaths::GetPath(LastCapturePngPath),
        FPaths::GetBaseFilename(LastCapturePngPath) + TEXT("_bbox_debug.png"));
    const TArray64<uint8>& DebugPng = Encoder->GetCompressed(100);
    const bool bSaved = FFileHelper::SaveArrayToFile(DebugPng, *DebugPath);
    UE_LOG(LogTemp, Warning, TEXT("[VisionBBox] Debug image: %s saved=%d"), *DebugPath, bSaved ? 1 : 0);
    return bSaved;
}

void AVisionCaptureActor::UpdateLastBestBBoxesFromAnalyzeJson(
    const FString& AnalyzeJson, int32 CapturePointIndex)
{
    LastBestBBoxByLabel.Reset();
    ResolvedBBoxTargets.Reset();

    TSharedPtr<FJsonObject> Root;
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AnalyzeJson);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("[VisionCaptureActor] analyze JSON parse failed (for bbox store)"));
            return;
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Detections = nullptr;
    if (!Root->TryGetArrayField(TEXT("detections"), Detections) || !Detections)
    {
        UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] analyze JSON has no 'detections'"));
        return;
    }

    for (const TSharedPtr<FJsonValue>& V : *Detections)
    {
        const TSharedPtr<FJsonObject>* DObjPtr = nullptr;
        if (!V.IsValid() || !V->TryGetObject(DObjPtr) || !DObjPtr || !DObjPtr->IsValid())
            continue;

        const TSharedPtr<FJsonObject>& DObj = *DObjPtr;

        FString Label;
        if (!DObj->TryGetStringField(TEXT("label"), Label))
            continue;

        double ConfD = 0.0;
        DObj->TryGetNumberField(TEXT("confidence"), ConfD);
        const float Conf = (float)ConfD;

        const TArray<TSharedPtr<FJsonValue>>* BBoxArr = nullptr;
        if (!DObj->TryGetArrayField(TEXT("bbox"), BBoxArr) || !BBoxArr || BBoxArr->Num() != 4)
            continue;

        const float X1 = (float)(*BBoxArr)[0]->AsNumber();
        const float Y1 = (float)(*BBoxArr)[1]->AsNumber();
        const float X2 = (float)(*BBoxArr)[2]->AsNumber();
        const float Y2 = (float)(*BBoxArr)[3]->AsNumber();

        // label蹂꾨줈 confidence媛 ????寃껊쭔 ?좎?
        FBestBBox* Existing = LastBestBBoxByLabel.Find(Label);
        if (!Existing || Conf > Existing->Confidence)
        {
            FBestBBox NewOne;
            NewOne.X1 = X1; NewOne.Y1 = Y1; NewOne.X2 = X2; NewOne.Y2 = Y2;
            NewOne.Confidence = Conf;
            NewOne.bValid = true;

            LastBestBBoxByLabel.Add(Label, NewOne);
        }
    }

    for (const TPair<FString, FBestBBox>& Pair : LastBestBBoxByLabel)
    {
        FResolvedBBoxTarget Resolved;
        ResolveBBoxToWorld(Pair.Key, Pair.Value, Resolved);
        ResolvedBBoxTargets.Add(Pair.Key, Resolved);
    }

    if (CapturePointIndex != INDEX_NONE)
    {
        FResolvedBBoxTargetArray PointTargets;
        ResolvedBBoxTargets.GenerateValueArray(PointTargets.Items);
        ResolvedBBoxTargetsByPoint.Add(CapturePointIndex, MoveTemp(PointTargets));
    }

    SaveBBoxDebugImage();

    UE_LOG(LogTemp, Warning, TEXT("[VisionCaptureActor] Stored best bboxes: %d labels, resolved world targets: %d"),
        LastBestBBoxByLabel.Num(), ResolvedBBoxTargets.Num());
}


// ?대?吏???쒓컖???뱀쭠??吏㏃? 臾몄옄?대줈 蹂?섑븯??濡쒖쭅
FString AVisionCaptureActor::GenerateSceneFingerprint()
{
    if (!CaptureRT) return TEXT("");

    FTextureRenderTargetResource* RTRes = CaptureRT->GameThread_GetRenderTargetResource();
    if (!RTRes) return TEXT("");

    TArray<FColor> Pixels;
    // ?꾩껜瑜????쎌쑝硫??먮━誘濡? 二쇱슂 吏???섑뵆留?(以묒븰 諛?4遺꾨㈃ 9媛??ъ씤??
    // ?ㅼ젣 援ы쁽?먯꽌???깅뒫???꾪빐 ReadPixels瑜??곕릺 踰붿쐞瑜?醫곹엳嫄곕굹 ?됯퇏媛믪쓣 ?곷땲??
    RTRes->ReadPixels(Pixels);

    if (Pixels.Num() == 0) return TEXT("");

    // 媛꾨떒??Average Color Hash 諛⑹떇 (?띾룄 ?곗꽑)
    int32 Step = Pixels.Num() / 16; // 16媛?援ъ뿭 ?섑뵆留?
    uint64 Hash = 0;
    for (int32 i = 0; i < Pixels.Num(); i += Step)
    {
        Hash += Pixels[i].R + Pixels[i].G + Pixels[i].B;
    }

    return FString::Printf(TEXT("%llu"), Hash);
}


void AVisionCaptureActor::UpdateMemory(int32 Index, const TArray<FPlanItem>& NewPlans, const FString& NewFingerprint)
{
    UE_LOG(LogTemp, Log,
        TEXT("[VisionMemory] UpdateMemory skipped. SaveGame cache is disabled. Index=%d, Plans=%d"),
        Index,
        NewPlans.Num());
}

