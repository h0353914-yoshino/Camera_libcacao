//
// BypassCamera.cpp — JNI 入口點 + BypassCameraContext 初始化/銷毀 + 公用工具
// 依照 so_32 nativeInitialize / nativeFinalize / nativeChangeToPhotoMode 等 Ghidra 反編譯重建
// JNI 簽名均從 BypassCamera.smali 確認
//

#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <imageprocessor/BypassCamera.h>
#include <cacao/CacaoClient.h>

#define LOG_TAG "libimageprocessorjni_real"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────
// 全域 JavaVM* — 由 JNI_OnLoad 設定，getJNIEnv() 使用
// ─────────────────────────────────────────────────────
static JavaVM *gJavaVMPointer = nullptr;
static thread_local bool gVideoThreadDirect = false;

namespace imageprocessor
{

    bool isVideoThreadDirect() __attribute__((visibility("hidden")));
    bool isVideoThreadDirect()
    {
        return gVideoThreadDirect;
    }

    void setVideoThreadDirect(bool direct) __attribute__((visibility("hidden")));
    void setVideoThreadDirect(bool direct)
    {
        gVideoThreadDirect = direct;
    }

} // namespace imageprocessor

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/)
{
    gJavaVMPointer = vm;
    return JNI_VERSION_1_6;
}

// ─────────────────────────────────────────────────────
// getJNIEnv — 從目前執行緒取得 JNIEnv*
// 來自 Ghidra so_32 @ 0x00017f78
// ─────────────────────────────────────────────────────
extern "C" JNIEnv *getJNIEnv(void)
{
    if (!gJavaVMPointer)
        return nullptr;
    JNIEnv *env = nullptr;
    jint ret = gJavaVMPointer->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (ret == JNI_OK)
        return env;
    // 若非 JNI_OK，嘗試 attach（背景執行緒情況）
    if (ret == JNI_EDETACHED)
    {
        if (gJavaVMPointer->AttachCurrentThread(&env, nullptr) == JNI_OK)
            return env;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────
// getSurface — 從 Java android.view.Surface 物件取得底層 ANativeWindow*
// 原始 .so @ 0x7fc8：透過 JNI 反射讀取 Surface.mNativeObject 欄位
// 對外導出（被外部呼叫端使用）
// ─────────────────────────────────────────────────────
extern "C" ANativeWindow *getSurface(JNIEnv *env, jobject surfaceObj)
{
    jclass surfaceClass = env->FindClass("android/view/Surface");
    jclass globalRef = (jclass)env->NewGlobalRef(surfaceClass);
    jfieldID fieldId = env->GetFieldID(globalRef, "mNativeObject", "J");
    jlong nativeObject = env->GetLongField(surfaceObj, fieldId);
    return reinterpret_cast<ANativeWindow *>((uintptr_t)nativeObject);
}

namespace imageprocessor
{

    // ─────────────────────────────────────────────────────
    // 工具：從 ProcessResultBase 取得 BypassCameraContext*
    // result->getUserData() 儲存的是 BypassCameraContext*
    // ─────────────────────────────────────────────────────
    BypassCameraContext *getCtxFromResult(const cacao::ProcessResultBase *result) __attribute__((visibility("hidden")));
    BypassCameraContext *getCtxFromResult(const cacao::ProcessResultBase *result)
    {
        if (!result)
            return nullptr;
        return static_cast<BypassCameraContext *>(result->getUserData());
    }

    // ─────────────────────────────────────────────────────
    // callPhotoCallback — 呼叫 Java callbackFromNative(IIIZZZIII)V
    // ─────────────────────────────────────────────────────
    __attribute__((visibility("hidden"))) void callPhotoCallback(BypassCameraContext *ctx,
                                                                 int type, int p1, int p2,
                                                                 bool b1, bool b2, bool b3,
                                                                 int i1, int i2, int i3)
    {
        if (!ctx || !ctx->photoJObj || !ctx->photoMethodId)
            return;
        JNIEnv *env = getJNIEnv();
        if (!env)
            return;
        env->CallVoidMethod(ctx->photoJObj, ctx->photoMethodId,
                            (jint)type,
                            (jint)p1, (jint)p2,
                            (jboolean)b1, (jboolean)b2, (jboolean)b3,
                            (jint)i1, (jint)i2, (jint)i3);
    }

    // ─────────────────────────────────────────────────────
    // callVideoCallback — 呼叫 Java callbackFromNativeVideo(I)V
    // ─────────────────────────────────────────────────────
    __attribute__((visibility("hidden"))) void callVideoCallback(BypassCameraContext *ctx, int type)
    {
        if (!ctx || !ctx->videoJObj || !ctx->videoMethodId)
            return;
        JNIEnv *env = getJNIEnv();
        if (!env)
            return;
        env->CallVoidMethod(ctx->videoJObj, ctx->videoMethodId, (jint)type);
    }

    // ─────────────────────────────────────────────────────
    // BypassCameraBufferContext 建構/解構
    // ─────────────────────────────────────────────────────
    __attribute__((visibility("hidden")))
    BypassCameraBufferContext::BypassCameraBufferContext()
    {
        byBufferPtr = nullptr;
        byNativeHandle = nullptr;
        surface = nullptr;
        pthread_mutex_init(&mutex, nullptr);
        ready = 0;
        _pad[0] = _pad[1] = _pad[2] = 0;
        dequeueCount = 0;
        totalBufCount = 0;
    }

    __attribute__((visibility("hidden")))
    BypassCameraBufferContext::~BypassCameraBufferContext()
    {
        // BufEntry*/ImageBuf* 的實際釋放由
        // BypassCameraBurstBufferManager_deleteBuffers 負責（透過
        // BypassCameraPhoto_finalize 呼叫），這裡只清掉容器本身，避免
        // ctx 整個被銷毀時，若忘了呼叫 finalize 也不會 leak 掉這兩個
        // SortedVector 物件本身（不重複刪除 value，因為那是 finalize 的責任）。
        delete byBufferPtr;
        delete byNativeHandle;
        pthread_mutex_destroy(&mutex);
    }

    // ─────────────────────────────────────────────────────
    // BypassCameraContext 建構/解構
    // ─────────────────────────────────────────────────────
    __attribute__((visibility("hidden")))
    BypassCameraContext::BypassCameraContext()
    {
        cacao = nullptr;
        initTag = 0xFFFFFFFF;
        cameraMode = 0;
        photoJObj = nullptr;
        photoMethodId = nullptr;
        snapshotReadyCb = nullptr;
        snapshotReadyResult = nullptr;
        snapshotCb = nullptr;
        requestCounter = 0;
        burstResultsById = nullptr;
        burstPrepareCb = nullptr;
        burstPrepareResult = nullptr;
        snapshotFreeCb = nullptr;
        snapshotFreeResult = nullptr;
        burstFinishCb = nullptr;
        burstFinishResult = nullptr;
        burstCb = nullptr;
        _pad1[0] = _pad1[1] = nullptr;
        secondResultsById = nullptr;
        pthread_mutex_init(&photoLock, nullptr);
        photoInitialized = false;
        _pad2[0] = _pad2[1] = _pad2[2] = 0;

        videoJObj = nullptr;
        videoMethodId = nullptr;
        videoFinishCb = nullptr;
        videoFinishResult = nullptr;
        videoPrepareCb = nullptr;
        videoPrepareResult = nullptr;
        videoStartSuperSlowCb = nullptr;
        videoStartSuperSlowResult = nullptr;
        videoStartRecCb = nullptr;
        videoStartRecResult = nullptr;
        videoSuperSlowCb = nullptr;
        videoSuperSlowResult = nullptr;
        videoThreadCb = nullptr;
        memset(videoThreadState, 0, sizeof(videoThreadState));
        videoInitialized = false;
        memset(_pad3, 0, sizeof(_pad3));

        pthread_mutex_init(&videoCommandLock, nullptr);
        pthread_cond_init(&videoCommandCond, nullptr);
        videoCommandThreadRunning = false;
        videoCommandThread = 0;

        // videoSurfaceProducer(sp<IGraphicBufferProducer>) 由其自身預設建構子初始化為 null，
        // 不需要額外程式碼（原版建構子也只是把它設為 null，等同 sp<T> 的預設行為）
        burstInitialized = false;
        _pad4[0] = _pad4[1] = _pad4[2] = 0;
        field_C0 = 0;
        field_C4 = 0;
        burstJObj = nullptr;
        burstMethodId = nullptr;
        burstSnapshotCb = nullptr;
        cachedVideoOutWidth = 0;
        cachedVideoOutHeight = 0;
        cachedSuperSlowFrameNum = 0;
        cachedPhotoInWidth = 0;
        cachedPhotoInHeight = 0;
        cachedPhotoOutWidth = 0;
        cachedPhotoOutHeight = 0;
        cachedPhotoCaptureNum = 0;
    }

    __attribute__((visibility("hidden")))
    BypassCameraContext::~BypassCameraContext()
    {
        if (videoCommandThreadRunning)
        {
            pthread_mutex_lock(&videoCommandLock);
            videoCommandThreadRunning = false;
            pthread_cond_broadcast(&videoCommandCond);
            pthread_mutex_unlock(&videoCommandLock);
            pthread_join(videoCommandThread, nullptr);
        }

        while (!videoCommandQueue.empty())
        {
            delete videoCommandQueue.front();
            videoCommandQueue.pop_front();
        }

        pthread_cond_destroy(&videoCommandCond);
        pthread_mutex_destroy(&videoCommandLock);
        // videoSurfaceProducer(sp<IGraphicBufferProducer>) 由其自身解構子自動釋放
        pthread_mutex_destroy(&photoLock);

        delete snapshotReadyCb;
        snapshotReadyCb = nullptr;
        delete snapshotCb;
        snapshotCb = nullptr;
        delete snapshotFreeCb;
        snapshotFreeCb = nullptr;
        if (burstResultsById)
        {
            for (size_t i = 0; i < burstResultsById->size(); i++)
            {
                delete (*burstResultsById)[i].value;
            }
            delete burstResultsById;
            burstResultsById = nullptr;
        }
        delete burstPrepareCb;
        burstPrepareCb = nullptr;
        delete burstFinishCb;
        burstFinishCb = nullptr;
        delete burstCb;
        burstCb = nullptr;
        if (secondResultsById)
        {
            // 這裡存的是 BufEntry*，其生命週期由 buffer pool（bufCtx.buffers）
            // 擁有，不可在這裡 delete；只需清空追蹤容器本身。
            delete secondResultsById;
            secondResultsById = nullptr;
        }
        delete videoFinishCb;
        videoFinishCb = nullptr;
        delete videoPrepareCb;
        videoPrepareCb = nullptr;
        delete videoStartSuperSlowCb;
        videoStartSuperSlowCb = nullptr;
        delete videoStartRecCb;
        videoStartRecCb = nullptr;
        delete videoSuperSlowCb;
        videoSuperSlowCb = nullptr;
        delete videoThreadCb;
        videoThreadCb = nullptr;

        // 釋放 ProcessCtrlResult 物件
        delete static_cast<cacao::ProcessCtrlResult *>(snapshotReadyResult);
        snapshotReadyResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(burstPrepareResult);
        burstPrepareResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(snapshotFreeResult);
        snapshotFreeResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(burstFinishResult);
        burstFinishResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(videoFinishResult);
        videoFinishResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(videoPrepareResult);
        videoPrepareResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(videoStartSuperSlowResult);
        videoStartSuperSlowResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(videoStartRecResult);
        videoStartRecResult = nullptr;
        delete static_cast<cacao::ProcessCtrlResult *>(videoSuperSlowResult);
        videoSuperSlowResult = nullptr;

        // 釋放 Java global refs
        JNIEnv *env = getJNIEnv();
        if (env)
        {
            if (photoJObj)
            {
                env->DeleteGlobalRef(photoJObj);
                photoJObj = nullptr;
            }
            if (videoJObj)
            {
                env->DeleteGlobalRef(videoJObj);
                videoJObj = nullptr;
            }
        }
    }

} // namespace imageprocessor

// ─────────────────────────────────────────────────────
// NULL CHECK MACRO（與原始 .so 保持一致的回傳值）
// ─────────────────────────────────────────────────────
#define CHECK_CTX(handle, ctx, retval)                                                        \
    do                                                                                        \
    {                                                                                         \
        if ((handle) == (jlong)0L)                                                            \
        {                                                                                     \
            ALOGE("%s: null native handle", __func__);                                        \
            return retval;                                                                    \
        }                                                                                     \
        (ctx) = reinterpret_cast<imageprocessor::BypassCameraContext *>((uintptr_t)(handle)); \
        if (!(ctx))                                                                           \
        {                                                                                     \
            ALOGE("%s: null ctx", __func__);                                                  \
            return retval;                                                                    \
        }                                                                                     \
    } while (0)

// ─────────────────────────────────────────────────────
// 前向宣告（實作在其他 .cpp 檔）
// ─────────────────────────────────────────────────────
extern "C" int BypassCameraPhoto_initialize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" void BypassCameraPhoto_finalize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraPhoto_changeToPhotoMode(imageprocessor::BypassCameraContext *ctx,
                                                   jint inW, jint inH, jint outW, jint outH, jint captureNum);
extern "C" int BypassCameraPhoto_requestSnapshotReady(imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraPhoto_prepareSnapshot(JNIEnv *env,
                                                 imageprocessor::BypassCameraContext *ctx,
                                                 jobject surface, jint totalBufCount);
namespace imageprocessor
{
    int BypassCameraPhoto_requestSnapshot(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx,
                                          jboolean p1, jboolean p2,
                                          jdouble p3, jdouble p4, jdouble p5,
                                          jboolean p6, jstring p7,
                                          jboolean p8, jint p9, jboolean p10,
                                          jint p11, jboolean p12, jint p13,
                                          jint p14, jint p15, jint p16,
                                          jboolean p17);
} // namespace imageprocessor
extern "C" int BypassCameraPhoto_requestSnapshotFree(imageprocessor::BypassCameraContext *ctx);

extern "C" int BypassCameraVideo_initialize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" void BypassCameraVideo_finalize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraVideo_changeToVideoMode(imageprocessor::BypassCameraContext *ctx,
                                                   jint mode, jint inW, jint inH, jint outW, jint outH, jint fps);
extern "C" int BypassCameraVideo_changeToSuperSlowMode(imageprocessor::BypassCameraContext *ctx,
                                                       jint mode, jint inW, jint inH, jint outW, jint outH,
                                                       jint outerFps, jint parametersFps, jint frameNum);
extern "C" int BypassCameraVideo_prepareVideoRecording(JNIEnv *env, imageprocessor::BypassCameraContext *ctx,
                                                       jobject surface, jint p1, jint p2, jint p3);
extern "C" int BypassCameraVideo_prepareSuperSlowRecording(JNIEnv *env, imageprocessor::BypassCameraContext *ctx,
                                                           jobject surface, jint p1, jint p2, jint p3);
extern "C" int BypassCameraVideo_startVideoRecording(imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraVideo_startSuperSlowRecording(imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraVideo_stopVideoRecording(imageprocessor::BypassCameraContext *ctx);

extern "C" int BypassCameraBurst_initialize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" void BypassCameraBurst_finalize(JNIEnv *env, jobject thiz, imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraPhoto_prepareBurstShot(imageprocessor::BypassCameraContext *ctx);
extern "C" int BypassCameraPhoto_finishBurstShot(imageprocessor::BypassCameraContext *ctx);

__attribute__((visibility("hidden"))) int BypassCamera_getCaps(JNIEnv *env, jint cameraIdx, jobject capsObj);

// 前向宣告
namespace imageprocessor
{
    void BypassCamera_finalize(JNIEnv *env, jobject thiz, BypassCameraContext *ctx);
}

namespace imageprocessor
{

    __attribute__((visibility("hidden"))) void enqueueVideoCommand(JNIEnv *env,
                                                                   imageprocessor::BypassCameraContext *ctx,
                                                                   imageprocessor::VideoThreadCommandKind kind,
                                                                   jobject surface,
                                                                   jint p1, jint p2, jint p3)
    {
        if (!ctx)
            return;
        jobject globalSurface = surface ? env->NewGlobalRef(surface) : nullptr;
        auto *command = new imageprocessor::VideoThreadCommand{kind, globalSurface, p1, p2, p3};
        pthread_mutex_lock(&ctx->videoCommandLock);
        ctx->videoCommandQueue.push_back(command);
        pthread_cond_signal(&ctx->videoCommandCond);
        pthread_mutex_unlock(&ctx->videoCommandLock);
    }

} // namespace imageprocessor

void *imageprocessor::BypassCameraVideo_worker(void *arg)
{
    auto *ctx = static_cast<imageprocessor::BypassCameraContext *>(arg);
    if (!ctx)
        return nullptr;

    for (;;)
    {
        pthread_mutex_lock(&ctx->videoCommandLock);
        while (ctx->videoCommandThreadRunning && ctx->videoCommandQueue.empty())
        {
            pthread_cond_wait(&ctx->videoCommandCond, &ctx->videoCommandLock);
        }
        if (!ctx->videoCommandThreadRunning && ctx->videoCommandQueue.empty())
        {
            pthread_mutex_unlock(&ctx->videoCommandLock);
            break;
        }
        imageprocessor::VideoThreadCommand *command = ctx->videoCommandQueue.front();
        ctx->videoCommandQueue.pop_front();
        pthread_mutex_unlock(&ctx->videoCommandLock);

        gVideoThreadDirect = true;
        switch (command->kind)
        {
        case imageprocessor::VideoThreadCommandKind::PrepareVideo:
            (void)BypassCameraVideo_prepareVideoRecording(nullptr, ctx, command->surface,
                                                          command->p1, command->p2, command->p3);
            break;
        case imageprocessor::VideoThreadCommandKind::PrepareSuperSlow:
            (void)BypassCameraVideo_prepareSuperSlowRecording(nullptr, ctx, command->surface,
                                                              command->p1, command->p2, command->p3);
            break;
        case imageprocessor::VideoThreadCommandKind::StartVideo:
            (void)BypassCameraVideo_startVideoRecording(ctx);
            break;
        case imageprocessor::VideoThreadCommandKind::StartSuperSlow:
            (void)BypassCameraVideo_startSuperSlowRecording(ctx);
            break;
        case imageprocessor::VideoThreadCommandKind::StopVideo:
            (void)BypassCameraVideo_stopVideoRecording(ctx);
            break;
        }
        gVideoThreadDirect = false;
        if (command->surface)
        {
            JNIEnv *env = getJNIEnv();
            if (env)
                env->DeleteGlobalRef(command->surface);
        }
        delete command;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────
// nativeIsDebugable()Z
// 來自 so_32 @ 0x000170b8
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jboolean JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeIsDebugable(
    JNIEnv *, jobject)
{
    return JNI_FALSE;
}

// ─────────────────────────────────────────────────────
// nativeInitialize(I)J
// param: int facing (0=BACK, 1=FRONT)
// return: jlong nativeHandle (0L = 失敗)
// 來自 so_32 @ 0x000170bc
// 原版邏輯:
//   1. 靜態 singleton guard（只建一次）
//   2. 若 ctx 已有 cacao，先 finalize 清理
//   3. ProcessFactory::createProcess() → 存入 ctx
//   4. 若失敗: log error, return -1
//   5. ctx->cameraMode = facing
//   6. BypassCameraPhoto_initialize
//   7. BypassCameraVideo_initialize
//   8. BypassCameraBurst_initialize
//   9. 任一步失敗: log, finalize, log, return -1
//   10. return 0 (成功, ctx pointer 作為 jlong)
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jlong JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeInitialize(
    JNIEnv *env, jobject thiz, jint facing)
{

    // 靜態 singleton（原版用 __cxa_guard_acquire）
    static imageprocessor::BypassCameraContext sCtx;
    static bool sCtxInitialized = false;
    if (!sCtxInitialized)
    {
        sCtxInitialized = true;
    }

    imageprocessor::BypassCameraContext *ctx = &sCtx;

    // 若 ctx 已有 cacao，先 finalize 清理
    if (ctx->cacao != nullptr)
    {
        imageprocessor::BypassCamera_finalize(env, thiz, ctx);
    }

    // 建立 Cacao 物件（先於 photo/video init）
    cacao::ProcessFactory factory;
    ctx->cacao = factory.createProcess();
    if (ctx->cacao == nullptr)
    {
        ALOGE("nativeInitialize: ProcessFactory::createProcess() failed");
        return (jlong)-1L;
    }

    ctx->cameraMode = (uint32_t)facing;

    // 初始化 photo/video/burst 回呼
    int ret = BypassCameraPhoto_initialize(env, thiz, ctx);
    if (ret != 0)
    {
        ALOGE("nativeInitialize: BypassCameraPhoto_initialize failed: %d", ret);
        imageprocessor::BypassCamera_finalize(env, thiz, ctx);
        ALOGE("nativeInitialize: failed");
        return (jlong)-1L;
    }

    ret = BypassCameraVideo_initialize(env, thiz, ctx);
    if (ret != 0)
    {
        ALOGE("nativeInitialize: BypassCameraVideo_initialize failed: %d", ret);
        imageprocessor::BypassCamera_finalize(env, thiz, ctx);
        ALOGE("nativeInitialize: failed");
        return (jlong)-1L;
    }

    ret = BypassCameraBurst_initialize(env, thiz, ctx);
    if (ret != 0)
    {
        ALOGE("nativeInitialize: BypassCameraBurst_initialize failed: %d", ret);
        imageprocessor::BypassCamera_finalize(env, thiz, ctx);
        ALOGE("nativeInitialize: failed");
        return (jlong)-1L;
    }

    return (jlong)(uintptr_t)ctx;
}

// ─────────────────────────────────────────────────────
// BypassCamera_finalize — 清理 ctx 中的 cacao 與回呼
// 原版邏輯: 刪除 cacao，清理 photo/video/burst 回呼
// ─────────────────────────────────────────────────────
namespace imageprocessor
{
    void BypassCamera_finalize(JNIEnv *env, jobject thiz, BypassCameraContext *ctx)
    {
        if (!ctx)
            return;

        if (ctx->cacao)
        {
            ctx->cacao->stop();
            delete ctx->cacao;
            ctx->cacao = nullptr;
        }

        BypassCameraPhoto_finalize(env, thiz, ctx);
        BypassCameraVideo_finalize(env, thiz, ctx);
        BypassCameraBurst_finalize(env, thiz, ctx);
    }
} // namespace imageprocessor

// ─────────────────────────────────────────────────────
// nativeFinalize(J)I
// return: 0=成功
// 來自 so_32 @ 0x000172b8
// 原版邏輯: 使用靜態 singleton，不 delete ctx
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeFinalize(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    imageprocessor::BypassCamera_finalize(env, thiz, ctx);
    return 0;
}

// ─────────────────────────────────────────────────────
// nativeChangeToPhotoMode(JIIIIII)I
// params: handle, mode, inW, inH, outW, outH, captureNum
//
// smali 交叉比對（BypassCamera.smali::changeToPhotoMode）確認 Java 端
// PhotoMode enum 只有唯一值 NORMAL（ordinal=0），且第 6 個 int 是
// captureNum，不是 flags。逐指令反組譯 so_32 @ 0x186dc（完整分析見
// SemcCameraUI/.tmp/investigation/snapshot_double_progress_findings.md）
// 確認這個內部函式本身根本不吃 mode：傳給 cald 的 ProcessCtrlMode
// 物件其 mode-type 欄位（field_08）是寫死的常數 0，不是從任何呼叫端
// 參數讀出來的——mode 只在這層 JNI wrapper 存在（配合 Java 端固定只有
// 一種 PhotoMode 的事實），不轉發到下面的內部函式。
// 反編譯這層 wrapper 本身（so_32 @ 0x172dc）額外確認：原版在轉呼叫前先
// 檢查 mode==0，不是 0 就直接記 log、回傳 -1，完全不呼叫內部函式（跟
// changeToVideoMode 的 mode>=5 guard 是同一種模式），這裡先前漏加。
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToPhotoMode(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jint mode, jint inW, jint inH, jint outW, jint outH, jint captureNum)
{
    if (mode != 0)
    {
        ALOGE("%s: invalid mode=%d", __func__, mode);
        return -1;
    }

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraPhoto_changeToPhotoMode(ctx, inW, inH, outW, outH, captureNum);
}

// ─────────────────────────────────────────────────────
// nativeChangeToVideoMode(JIIIIII)I
// params: handle, mode, inW, inH, outW, outH, fps
// smali 交叉比對（BypassCamera.smali::changeToVideoMode）確認第 6 個 int
// 是方法自己的 fps 參數（p4），不是 flags。
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToVideoMode(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jint mode, jint inW, jint inH, jint outW, jint outH, jint fps)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraVideo_changeToVideoMode(ctx, mode, inW, inH, outW, outH, fps);
}

// ─────────────────────────────────────────────────────
// nativeChangeToSuperSlowMode(JIIIIIIII)I
// params: handle, mode, inW, inH, outW, outH, outerFps, parametersFps, frameNum
// smali 交叉比對（BypassCamera.smali::changeToSuperSlowMode）確認最後兩個
// int 分別是方法自己的 fps 參數（outerFps）與 SuperSlowRecordingParameters.fps
// （parametersFps）——Java 端真的有兩個不同來源、都叫 fps 的值，不是我們的
// 命名錯誤。
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeChangeToSuperSlowMode(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jint mode, jint inW, jint inH, jint outW, jint outH,
    jint outerFps, jint parametersFps, jint frameNum)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraVideo_changeToSuperSlowMode(
        ctx, mode, inW, inH, outW, outH, outerFps, parametersFps, frameNum);
}

// ─────────────────────────────────────────────────────
// nativeGetLatestRequestId(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetLatestRequestId(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, 0);
    return (jint)ctx->requestCounter;
}

// ─────────────────────────────────────────────────────
// nativeRequestPrepareSnapshot(JLandroid/view/Surface;I)I
// params: handle, surface, mode
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestPrepareSnapshot(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jobject surface, jint mode)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraPhoto_prepareSnapshot(env, ctx, surface, mode);
}

// ─────────────────────────────────────────────────────
// nativeRequestSnapshotReady(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestSnapshotReady(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraPhoto_requestSnapshotReady(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestSnapshot(JZZDDDZLjava/lang/String;ZIZIZIIIIZI)I
// params: handle, z1, z2, d1, d2, d3, z3, str, z4, i1, z5, i2, z6, i3, i4, i5, i6, z7, i7
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestSnapshot(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jboolean p1, jboolean p2,
    jdouble p3, jdouble p4, jdouble p5,
    jboolean p6, jstring p7,
    jboolean p8, jint p9, jboolean p10,
    jint p11, jboolean p12, jint p13,
    jint p14, jint p15, jint p16,
    jboolean p17, jint p18)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return imageprocessor::BypassCameraPhoto_requestSnapshot(env, thiz, ctx,
                                                             p1, p2, p3, p4, p5, p6, p7,
                                                             p8, p9, p10, p11, p12, p13, p14, p15, p16, p17);
}

// ─────────────────────────────────────────────────────
// nativeRequestSnapshotFree(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestSnapshotFree(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraPhoto_requestSnapshotFree(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestPrepareVideoRecording(JLandroid/view/Surface;III)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestPrepareVideoRecording(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jobject surface, jint p1, jint p2, jint p3)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraVideo_prepareVideoRecording(env, ctx, surface, p1, p2, p3);
}

// ─────────────────────────────────────────────────────
// nativeRequestPrepareSuperSlowRecording(JLandroid/view/Surface;III)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestPrepareSuperSlowRecording(
    JNIEnv *env, jobject thiz, jlong nativeHandle,
    jobject surface, jint p1, jint p2, jint p3)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    ALOGD("nativeRequestPrepareSuperSlowRecording: ctx=%p surface=%p p1=%d p2=%d p3=%d", ctx, surface, (int)p1, (int)p2, (int)p3);

    return BypassCameraVideo_prepareSuperSlowRecording(env, ctx, surface, p1, p2, p3);
}

// ─────────────────────────────────────────────────────
// nativeRequestStartVideoRecording(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestStartVideoRecording(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraVideo_startVideoRecording(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestStartSuperSlowRecording(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestStartSuperSlowRecording(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    ALOGD("nativeRequestStartSuperSlowRecording: ctx=%p", ctx);

    return BypassCameraVideo_startSuperSlowRecording(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestStopVideoRecording(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestStopVideoRecording(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraVideo_stopVideoRecording(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestPrepareBurstShot(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestPrepareBurstShot(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    if (!ctx->burstCb)
    {
        int ret = BypassCameraBurst_initialize(env, thiz, ctx);
        if (ret != 0)
            return ret;
    }

    return BypassCameraPhoto_prepareBurstShot(ctx);
}

// ─────────────────────────────────────────────────────
// nativeRequestFinishBurstShot(J)I
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeRequestFinishBurstShot(
    JNIEnv *env, jobject thiz, jlong nativeHandle)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);

    return BypassCameraPhoto_finishBurstShot(ctx);
}

// ─────────────────────────────────────────────────────
// nativeGetCaps(ILBypassCameraParameters$Capability;)I  [static]
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeGetCaps(
    JNIEnv *env, jclass clazz, jint cameraIdx, jobject capsObj)
{

    return BypassCamera_getCaps(env, cameraIdx, capsObj);
}

// ─────────────────────────────────────────────────────
// nativeSetConfig(JLBypassCameraParameters;)I
// 來自 so_32 @ 0x00017d30
// 邏輯：從 BypassCameraParameters.mMap 讀取 "climax-recognition" 字串，
//       轉換為 ProcessCtrlDynamicParameterConfig.configValue 後呼叫 setConfig
// ─────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_sonymobile_imageprocessor_bypasscamera2_BypassCamera_nativeSetConfig(
    JNIEnv *env, jobject thiz, jlong nativeHandle, jobject configObj)
{

    imageprocessor::BypassCameraContext *ctx;
    CHECK_CTX(nativeHandle, ctx, -1);
    if (!ctx->cacao)
        return -1;

    // 取得 BypassCameraParameters class
    jclass cls = env->GetObjectClass(configObj);
    if (!cls)
        return -1;

    // 取得 mMap field (Ljava/util/Map;)
    jfieldID mapFid = env->GetFieldID(cls, "mMap", "Ljava/util/Map;");
    env->DeleteLocalRef(cls);
    if (!mapFid)
    {
        env->ExceptionClear();
        ALOGE("nativeSetConfig: GetFieldID(mMap) failed");
        return -1;
    }

    // FindClass java/util/Map
    jclass mapCls = env->FindClass("java/util/Map");
    if (!mapCls)
    {
        ALOGE("nativeSetConfig: %s: Failed find class java/util/Map.", __FUNCTION__);
        return -1;
    }

    // GetObjectField: 取得 mMap 實例
    jobject mapObj = env->GetObjectField(configObj, mapFid);
    if (!mapObj)
    {
        ALOGE("nativeSetConfig: %s: Failed get object field for java/util/Map.", __FUNCTION__);
        env->DeleteLocalRef(mapCls);
        return -1;
    }

    // GetMethodID: Map.get(Object) → Object
    jmethodID getMid = env->GetMethodID(mapCls, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(mapCls);
    if (!getMid)
    {
        ALOGE("nativeSetConfig: %s: Failed get method id for java/util/Map.get(Object).", __FUNCTION__);
        env->DeleteLocalRef(mapObj);
        return -1;
    }

    // NewStringUTF("climax-recognition")，呼叫 map.get(key)
    jstring keyStr = env->NewStringUTF("climax-recognition");
    jobject valObj = env->CallObjectMethod(mapObj, getMid, keyStr);
    env->DeleteLocalRef(keyStr);
    env->DeleteLocalRef(mapObj);

    cacao::ProcessCtrlDynamicParameterConfig cfg;
    cfg.configValue = 0; // 預設值：off

    if (valObj != nullptr)
    {
        const char *val = env->GetStringUTFChars((jstring)valObj, nullptr);
        jint ret = 0;
        if (strcmp(val, "on") == 0)
        {
            cfg.configValue = 2;
        }
        else if (strcmp(val, "auto") == 0)
        {
            cfg.configValue = 1;
        }
        else if (strcmp(val, "off") == 0)
        {
            cfg.configValue = 0;
        }
        else
        {
            ALOGE("nativeSetConfig: %s: Invalid climax recognition value. val: %s",
                  __FUNCTION__, val);
            env->ReleaseStringUTFChars((jstring)valObj, val);
            env->DeleteLocalRef(valObj);
            return -1;
        }
        env->ReleaseStringUTFChars((jstring)valObj, val);
        env->DeleteLocalRef(valObj);
    }

    return ctx->cacao->setConfig(&cfg);
}
