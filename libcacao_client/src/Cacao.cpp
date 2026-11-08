// Cacao.cpp — android::Cacao 客戶端 facade 完整實作
// 依照 libcacao_client.so (ARM 32-bit) Ghidra 反編譯結果 100% 重建
//
// 包含:
//   - Cacao::mService / mServicePid / mLock / mDeathNotifier (靜態全域)
//   - Cacao::DeathNotifier (Binder 死亡通知)
//   - Cacao::CacaoProcessCallback (進程結果/進度回呼, BnCacaoProcessCallback)
//   - Cacao::CacaoClient (主連線客戶端, BnCacaoClient + DeathRecipient)
//   - Cacao (facade: getService/create/connect/disconnect/getCaps/...)

#include <binder/IInterface.h>
#include <binder/IBinder.h>
#include <binder/IMemory.h>
#include <cacao/CacaoAidlClient.h>
#ifndef __ANDROID_VENDOR__
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#endif
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Parcel.h>
#include <utils/Vector.h>
#include <log/log.h>
#include <new>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <cacao/CacaoClient.h>
#include <cacao/ICacaoService.h>
#include <cacao/ICacao.h>
#include <cacao/ICacaoClient.h>
#include <cacao/ICacaoProcessCallback.h>
#include <cacao/ProcessCtrlGatewayBase.h>

#define LOG_TAG "CacaoClient"

// ── vtable dispatch helpers ──
// 原始 binary 透過 vtable offset 直接呼叫 serialize/deserialize/getSerializedSize。
// 這些 helper 模擬相同行為，適用於 ProcessCtrlMode/Param/Config/Result 等
// 同時繼承 ObjectBase-derived base + ISerializable 的類別。
// primary vtable layout（以 ProcessCtrlMode 為例）：
//   [0] D1, [1] D0, [2] dump, [3] getClassName,
//   ProcessModeBase: [4] getType
//   ISerializable: [5] getSerializedSize, [6] serialize, [7] deserialize
// ProcessCtrlParam: ... [4] clone, [5] copy, [6] getSerializedSize, [7] serialize, [8] deserialize
// ProcessCtrlDPConfig: [4] getConfigId, [5] clone, [6] copy,
//                     [7] getSerializedSize, [8] serialize, [9] deserialize
// ProcessCtrlResult: [4-8] base, [9] getSz, [10] serialize, [11] deserialize
//
// 由於各類別的 vtable slot offset 不一致，直接呼叫 C++ 虛函式不可行。
// 符合原始 binary 行為的方式是透過 ISerializable* 的 secondary vtable。
// ProcessCtrlMode/Param/Config/Result 都繼承 ISerializable：
//   ProcessCtrlMode  at +0x04
//   ProcessCtrlParam at +0x04
//   ProcessCtrlDPConfig at +0x04
//   ProcessCtrlResult at +0x04
//
// 原始 binary 的做法是 *(vtable + offset)(this)，但在 C++ source 中
// 我們可以用 reinterpret_cast 解析 secondary base 指標。
// secondary_base_ptr = *(void**)((char*)obj + 4)  → ISerializable vtable
// 但更安全的做法：static_cast 到實際子類別後再 cast ISerializable。
//
// 實際上，原始 .so 中的呼叫方式是：透過 primary vtable 的合併 slot 呼叫。
// 為了相容性，我們使用通用 vtable 呼叫 helper。

namespace
{

    // 取得物件的 secondary ISerializable*（位於 this + sizeof(void*)）
    inline cacao::ISerializable *toSerializable(void *obj)
    {
        // secondary vtable at offset +sizeof(void*)（ProcessModeBase/ProcessParamBase 之後）
        return reinterpret_cast<cacao::ISerializable *>(
            reinterpret_cast<char *>(obj) + sizeof(void *));
    }

    inline const cacao::ISerializable *toSerializable(const void *obj)
    {
        return reinterpret_cast<const cacao::ISerializable *>(
            reinterpret_cast<const char *>(obj) + sizeof(void *));
    }

    template <typename U>
    static inline android::sp<android::IMemory> allocMemory_common(U raw_size)
    {
        size_t size = (size_t)raw_size;
        android::sp<android::IMemoryHeap> heap(
            new android::MemoryHeapBase(size, 0, nullptr));
        android::sp<android::IMemory> mem(
            new android::MemoryBase(heap, 0, size));
        return mem;
    }

} // anonymous namespace

namespace android
{

    // ══════════════════════════════════════════════════════════════
    // Cacao 靜態全域狀態（由 wrapper libcacao_client.so 透過 dlsym 存取）
    // ══════════════════════════════════════════════════════════════

    sp<ICacaoService> Cacao::mService = nullptr;
    int Cacao::mServicePid = 0;
    pthread_mutex_t Cacao::mServiceLock = PTHREAD_MUTEX_INITIALIZER;
    sp<Cacao::DeathNotifier> Cacao::mDeathNotifier = nullptr;

    // ══════════════════════════════════════════════════════════════
    // Cacao::DeathNotifier — Binder 死亡通知器
    // 服務崩潰時清理靜態 singleton 狀態
    // Object size = 0x0c (vtable + RefBase 8 bytes)
    // ══════════════════════════════════════════════════════════════

    Cacao::DeathNotifier::DeathNotifier() {}
    Cacao::DeathNotifier::~DeathNotifier() {}

    // 原始 .so binderDied 邏輯:
    //   mutex_lock(mLock)
    //   if mService != null: asBinder(mService).unlinkToDeath(mDeathNotifier, nullptr, 0)
    //     clear mDeathNotifier sp
    //   mService.clear(); mServicePid = 0
    //   ALOGI("cacao service died")
    //   mutex_unlock(mLock)
    void Cacao::DeathNotifier::binderDied(const wp<IBinder> &who)
    {
        pthread_mutex_lock(&Cacao::mServiceLock);
        if (Cacao::mService != nullptr)
        {
            sp<IBinder> binder = IInterface::asBinder(Cacao::mService);
            if (binder != nullptr)
            {
                binder->unlinkToDeath(
                    wp<IBinder::DeathRecipient>(Cacao::mDeathNotifier),
                    nullptr, 0);
            }
            Cacao::mDeathNotifier.clear();
        }
        Cacao::mService.clear();
        Cacao::mServicePid = 0;
        ALOGI("cacao service died");
        pthread_mutex_unlock(&Cacao::mServiceLock);
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::CacaoProcessCallback — 進度/結果回呼 (BnCacaoProcessCallback)
    // 包裝 ICBProcessResultListener，用於 CacaoClient::process 呼叫時
    // Object size ≈ 0x1c (vtable+0 BBinder+4 mListener+0x10 mResult+0x14 RefBase+0x18)
    // ══════════════════════════════════════════════════════════════

    Cacao::CacaoProcessCallback::CacaoProcessCallback(
        cacao::ICBProcessResultListener *listener,
        cacao::ProcessResultBase *result)
        : mListener(listener), mResult(result)
    {
    }

    Cacao::CacaoProcessCallback::~CacaoProcessCallback() {}

    // 原始 .so CacaoProcessCallback::notifyProgress:
    //   mResult->vtable[0x2c/4](&sd)  → deserialize (ISerializable::deserialize)
    //   mListener->onHandleProgress(mResult)
    void Cacao::CacaoProcessCallback::notifyProgress(
        const sp<IMemory> &mem,
        cacao::ISerialize::SerializedData sd)
    {
        if (mResult != nullptr)
        {
            // 呼叫 vtable+0x2c = ISerializable::deserialize(SerializedData*)
            typedef int (*DesFn)(void *, cacao::ISerialize::SerializedData *);
            void **vt = *reinterpret_cast<void ***>(mResult);
            reinterpret_cast<DesFn>(vt[0x2c / 4])(mResult, &sd);
        }
        if (mListener != nullptr)
        {
            mListener->onHandleProgress(mResult);
        }
    }

    // 原始 .so CacaoProcessCallback::notifyResult:
    //   mResult->vtable[0x2c/4](&sd)  → deserialize
    //   mListener->onHandleResult(mResult)
    void Cacao::CacaoProcessCallback::notifyResult(
        const sp<IMemory> &mem,
        cacao::ISerialize::SerializedData sd,
        android::Vector<cacao::ImageBuf *> *bufs)
    {
        if (mResult != nullptr)
        {
            typedef int (*DesFn)(void *, cacao::ISerialize::SerializedData *);
            void **vt = *reinterpret_cast<void ***>(mResult);
            reinterpret_cast<DesFn>(vt[0x2c / 4])(mResult, &sd);
        }
        if (mListener != nullptr)
        {
            mListener->onHandleResult(mResult);
        }
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::CacaoClient — 主連線客戶端
    // BnCacaoClient + IBinder::DeathRecipient + ICBProcessResultListener
    // Object size = 0x30
    // Layout: vtable@0 BBinder@4 DeathRecipient@0x10 Gateway@0x14
    //         mCacao@0x18 mLock@0x1c mListener@0x20 mCookie@0x24 RefBase@0x28
    // ══════════════════════════════════════════════════════════════

    // ── ICBProcessResultListener virtual base thunks ─────────────
    // 原始 CacaoClient 不直接繼承 ICBProcessResultListener
    // onHandleProgress/onHandleResult 均由外部 listener 處理，此處不需實作

    Cacao::CacaoClient::CacaoClient()
        : mCacao(nullptr), mListener(nullptr), mCookie(nullptr)
    {
        pthread_mutex_init(&mLock, nullptr);
    }

    Cacao::CacaoClient::~CacaoClient()
    {
        disconnect();
        pthread_mutex_destroy(&mLock);
    }

    // ── notifyEvent ───────────────────────────────────────────────
    // 原始 .so CacaoClient::notifyEvent:
    //   if mListener != null:
    //     event = ProcessFactory::createEvent(eventType, sd)
    //     if event == null: ALOGE("Failed to create ProcessEvent")
    //     else: event->setId(mCookie); listener->onEvent(event); event->release()
    void Cacao::CacaoClient::notifyEvent(
        int eventType,
        const sp<IMemory> &mem,
        cacao::ISerialize::SerializedData sd)
    {
        cacao::ICBProcessEventListener *listener = mListener;
        if (listener == nullptr)
            return;

        cacao::ISerialize::SerializedData sd_copy = sd;
        cacao::ProcessEventBase *event =
            cacao::ProcessFactory::createEvent((uint32_t)eventType, &sd_copy);
        if (event == nullptr)
        {
            ALOGE("CacaoClient::notifyEvent: failed to create ProcessEvent (type=%d)", eventType);
            return;
        }
        // vtable+0x18 = setId (void*) / vtable+0x8 = setId(int)?
        // 原始: event->setId(mCookie)
        typedef void (*SetIdFn)(cacao::ProcessEventBase *, void *);
        void **vt = *reinterpret_cast<void ***>(event);
        reinterpret_cast<SetIdFn>(vt[0x18 / 4])(event, mCookie);
        listener->onHandleEvent(event);
        // 釋放 event: vtable+4 = delete dtor
        typedef void (*DtorFn)(cacao::ProcessEventBase *);
        reinterpret_cast<DtorFn>(vt[4 / 4])(event);
    }

    // ── binderDied ────────────────────────────────────────────────
    // 原始 .so CacaoClient::binderDied: ALOGI("cacao died")
    void Cacao::CacaoClient::binderDied(const wp<IBinder> &who)
    {
        ALOGI("cacao died");
    }

    // ── subscribeEvents ───────────────────────────────────────────
    // 原始 .so: mutex_lock → mListener=p, mCookie=cookie → mutex_unlock
    int Cacao::CacaoClient::subscribeEvents(
        cacao::ICBProcessEventListener *listener,
        void *cookie)
    {
        pthread_mutex_lock(&mLock);
        mListener = listener;
        mCookie = cookie;
        pthread_mutex_unlock(&mLock);
        return 0;
    }

    // ── connect ───────────────────────────────────────────────────
    // 原始 .so:
    //   mutex_lock(mLock)
    //   if svc==null: return -0x67 (UNKNOWN)
    //   if mCacao!=null: return -0x6b (ALREADY_CONNECTED)
    //   mCacao = svc->connect(this) → sp<ICacao>
    //   if mCacao==null: return -0x6f (CONNECT_FAILED)
    //   asBinder(mCacao).linkToDeath(wp<CacaoClient>(this))
    //   mutex_unlock
    int Cacao::CacaoClient::connect(const sp<ICacaoService> &svc)
    {
        pthread_mutex_lock(&mLock);
        if (svc == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x67;
        }
        if (mCacao != nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x6b;
        }
        // svc->connect(this) — 將本 BnCacaoClient 傳給服務
        sp<ICacao> cacao = svc->connect(sp<ICacaoClient>(this));
        mCacao = cacao;
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x6f;
        }
        // linkToDeath: 當服務的 ICacao binder 死亡時通知 CacaoClient
        sp<IBinder> binder = IInterface::asBinder(mCacao);
        if (binder != nullptr)
        {
            binder->linkToDeath(sp<IBinder::DeathRecipient>(this), nullptr, 0);
        }
        else
        {
            cacaoAidlLinkCacaoDeath(mCacao);
        }
        pthread_mutex_unlock(&mLock);
        return 0;
    }

    // ── disconnect ────────────────────────────────────────────────
    // 原始 .so:
    //   mutex_lock(mLock)
    //   if mCacao!=null:
    //     asBinder.unlinkToDeath(CacaoClient)
    //     mCacao->disconnect()
    //     mCacao.clear()
    //   mutex_unlock
    void Cacao::CacaoClient::disconnect()
    {
        pthread_mutex_lock(&mLock);
        if (mCacao != nullptr)
        {
            sp<IBinder> binder = IInterface::asBinder(mCacao);
            if (binder != nullptr)
            {
                binder->unlinkToDeath(
                    wp<IBinder::DeathRecipient>(this), nullptr, 0);
            }
            else
            {
                cacaoAidlUnlinkCacaoDeath(mCacao);
            }
            mCacao->disconnect();
            mCacao.clear();
        }
        pthread_mutex_unlock(&mLock);
    }

    // ── allocMemory ───────────────────────────────────────────────
    sp<IMemory> Cacao::CacaoClient::allocMemory(size_t size)
    {
        return allocMemory_common(size);
    }

    // ── freeMemory ────────────────────────────────────────────────
    void Cacao::CacaoClient::freeMemory(sp<IMemory> &mem)
    {
        if (mem != nullptr)
        {
            mem.clear();
        }
    }

    // ── makeSerializedData ────────────────────────────────────────
    // 原始 .so: sd->size = mem->size(); sd->data = mem->pointer()
    void Cacao::CacaoClient::makeSerializedData(
        const sp<IMemory> &mem,
        cacao::ISerialize::SerializedData *sd)
    {
        if (sd == nullptr || mem == nullptr)
            return;
        sd->size = (uint32_t)mem->size();
        sd->data = mem->unsecurePointer();
    }

    // ── start ─────────────────────────────────────────────────────
    // 原始 .so:
    //   mutex_lock; if mCacao==null: return -0x65
    //   if mode==null: return -0x67
    //   size = mode->getSerializedSize(); allocMemory(size)
    //   makeSerializedData(mem, &sd)
    //   mode->serialize(&sd)
    //   mCacao->start(mem, sd) → ret
    //   if ret == -0x6e: return -0x6f; if ret==0: return 0
    //   mutex_unlock
    int Cacao::CacaoClient::start(const cacao::ProcessModeBase *mode)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        if (mode == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x67;
        }
        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        uint32_t sz = toSerializable(const_cast<cacao::ProcessModeBase *>(mode))->getSerializedSize();
        sp<IMemory> mem = allocMemory(sz);
        if (mem == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x66;
        }
        makeSerializedData(mem, &sd);
        int ret = toSerializable(const_cast<cacao::ProcessModeBase *>(mode))->serialize(&sd);
        if (ret >= 0)
        {
            ret = mCacao->start(mem, sd);
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── stop ──────────────────────────────────────────────────────
    // 原始 .so: mutex; mCacao->stop() → ret; translate
    int Cacao::CacaoClient::stop()
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        int ret = mCacao->stop();
        if (ret != 0)
        {
            if (ret != -0x6e)
                ret = -0x6f;
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── setConfig ─────────────────────────────────────────────────
    // 原始 .so: 類似 start，allocMemory → malloc sd → cfg->serialize
    //   → mCacao->setConfig(configId, mem, sd)
    int Cacao::CacaoClient::setConfig(const cacao::ProcessConfigBase *cfg)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        if (cfg == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x67;
        }
        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        uint32_t sz = toSerializable(const_cast<cacao::ProcessConfigBase *>(cfg))->getSerializedSize();
        sp<IMemory> mem = allocMemory(sz);
        if (mem == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x66;
        }
        makeSerializedData(mem, &sd);
        int ret = toSerializable(const_cast<cacao::ProcessConfigBase *>(cfg))->serialize(&sd);
        if (ret >= 0)
        {
            int configId = const_cast<cacao::ProcessConfigBase *>(cfg)->getConfigId();
            ret = mCacao->setConfig(configId, mem, sd);
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── getConfig ─────────────────────────────────────────────────
    // 原始 .so: allocMemory → sd → mCacao->getConfig(id, mem, sd) → cfg->deserialize
    int Cacao::CacaoClient::getConfig(cacao::ProcessConfigBase *cfg)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        if (cfg == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x67;
        }
        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        uint32_t sz = toSerializable(cfg)->getSerializedSize();
        sp<IMemory> mem = allocMemory(sz);
        if (mem == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x66;
        }
        makeSerializedData(mem, &sd);
        int ret = toSerializable(cfg)->serialize(&sd);
        if (ret >= 0)
        {
            int configId = cfg->getConfigId();
            ret = mCacao->getConfig(configId, mem, sd);
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
            if (ret == 0)
            {
                // 反序列化回傳結果
                memset(&sd, 0, sizeof(sd));
                makeSerializedData(mem, &sd);
                toSerializable(cfg)->deserialize(&sd);
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── process ───────────────────────────────────────────────────
    // 原始 .so: allocMemory(param) → sd → param->serialize
    //   allocMemory(result size) for result
    //   new CacaoProcessCallback(listener, result) → sp<ICacaoProcessCallback>
    //   mCacao->process(paramMem, paramSd, emptyVector, resultMem, resultSd, 0, cb)
    //   deserialize result from resultMem
    int Cacao::CacaoClient::process(const cacao::ProcessParamBase *param,
                                    cacao::ICBProcessResultListener *listener,
                                    cacao::ProcessResultBase *result)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        // [已修正，20260802] 反編譯 libcacao_client.cpp 內既有的原版反組譯 dump
        // （android::Cacao::CacaoClient::process，0xfd28 附近）確認驗證規則其實
        // 很單純：result==null 就直接失敗（不管 param/listener 是不是也是
        // null），原本這裡多包了一層「result==null 時才檢查 param/listener」
        // 的邏輯，讓 result==null 但 param/listener 都給了的情況繼續往下跑，
        // 跟原版不符。目前專案內所有呼叫點都一定會給 result，這個分支實際上
        // 不會被觸發，但邏輯本身還是照原版修正。
        if (result == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x67;
        }
        cacao::ISerialize::SerializedData paramSd;
        memset(&paramSd, 0, sizeof(paramSd));
        sp<IMemory> paramMem;
        int ret = 0;
        if (param != nullptr)
        {
            uint32_t pSz = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->getSerializedSize();
            paramMem = allocMemory(pSz);
            if (paramMem == nullptr)
            {
                pthread_mutex_unlock(&mLock);
                return -0x66;
            }
            makeSerializedData(paramMem, &paramSd);
            ret = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->serialize(&paramSd);
        }
        cacao::ISerialize::SerializedData resultSd;
        memset(&resultSd, 0, sizeof(resultSd));
        sp<IMemory> resultMem;
        if (result != nullptr)
        {
            uint32_t rSz = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->getSerializedSize();
            resultMem = allocMemory(rSz);
            if (resultMem == nullptr)
            {
                pthread_mutex_unlock(&mLock);
                return -0x66;
            }
            makeSerializedData(resultMem, &resultSd);
            ret = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->serialize(&resultSd);
        }
        sp<ICacaoProcessCallback> cb;
        if (listener != nullptr && result != nullptr)
        {
            cb = new CacaoProcessCallback(listener, result);
        }
        Vector<cacao::ImageBuf *> emptyBufs;
        if (ret >= 0)
        {
            ret = mCacao->process(paramMem, paramSd, emptyBufs,
                                  resultMem, resultSd, 0, cb);
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
            if (ret == 0 && resultMem != nullptr && result != nullptr)
            {
                memset(&resultSd, 0, sizeof(resultSd));
                makeSerializedData(resultMem, &resultSd);
                toSerializable(result)->deserialize(&resultSd);
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── processAsync ──────────────────────────────────────────────
    // 類似 process，但使用 imageBufs 並傳遞to mCacao->process(imageBufs)
    int Cacao::CacaoClient::processAsync(const cacao::ProcessParamBase *param,
                                         android::Vector<cacao::ImageBuf *> *imageBufs,
                                         cacao::ICBProcessResultListener *listener,
                                         cacao::ProcessResultBase *result)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        cacao::ISerialize::SerializedData paramSd;
        memset(&paramSd, 0, sizeof(paramSd));
        sp<IMemory> paramMem;
        int ret = 0;
        if (param != nullptr)
        {
            uint32_t pSz = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->getSerializedSize();
            paramMem = allocMemory(pSz);
            if (paramMem != nullptr)
            {
                makeSerializedData(paramMem, &paramSd);
                ret = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->serialize(&paramSd);
            }
        }
        cacao::ISerialize::SerializedData resultSd;
        memset(&resultSd, 0, sizeof(resultSd));
        sp<IMemory> resultMem;
        if (result != nullptr)
        {
            uint32_t rSz = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->getSerializedSize();
            resultMem = allocMemory(rSz);
            if (resultMem != nullptr)
            {
                makeSerializedData(resultMem, &resultSd);
                ret = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->serialize(&resultSd);
            }
            else
            {
                ret = 0;
            }
        }
        sp<ICacaoProcessCallback> cb;
        if (listener != nullptr && result != nullptr)
        {
            cb = new CacaoProcessCallback(listener, result);
        }
        Vector<cacao::ImageBuf *> bufsCopy;
        if (imageBufs != nullptr)
        {
            bufsCopy = *imageBufs;
        }
        if (ret >= 0 && resultMem != nullptr)
        {
            ret = mCacao->process(paramMem, paramSd, bufsCopy,
                                  resultMem, resultSd, 0, cb);
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── processAsyncWithBinder ────────────────────────────────────
    int Cacao::CacaoClient::processAsyncWithBinder(const cacao::ProcessParamBase *param,
                                                   const sp<IBinder> &binder,
                                                   cacao::ICBProcessResultListener *listener,
                                                   cacao::ProcessResultBase *result)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        cacao::ISerialize::SerializedData paramSd;
        memset(&paramSd, 0, sizeof(paramSd));
        sp<IMemory> paramMem;
        int ret = 0;
        if (param != nullptr)
        {
            uint32_t pSz = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->getSerializedSize();
            paramMem = allocMemory(pSz);
            if (paramMem != nullptr)
            {
                makeSerializedData(paramMem, &paramSd);
                ret = toSerializable(const_cast<cacao::ProcessParamBase *>(param))->serialize(&paramSd);
            }
        }
        cacao::ISerialize::SerializedData resultSd;
        memset(&resultSd, 0, sizeof(resultSd));
        sp<IMemory> resultMem;
        if (result != nullptr)
        {
            uint32_t rSz = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->getSerializedSize();
            resultMem = allocMemory(rSz);
            if (resultMem != nullptr)
            {
                makeSerializedData(resultMem, &resultSd);
                ret = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->serialize(&resultSd);
            }
            else
            {
                ret = 0;
            }
        }
        sp<ICacaoProcessCallback> cb;
        if (listener != nullptr && result != nullptr)
        {
            cb = new CacaoProcessCallback(listener, result);
        }
        if (ret >= 0 && resultMem != nullptr)
        {
#ifndef __ANDROID_VENDOR__
            // JNI 傳進來的是 IGraphicBufferProducer 的 binder；轉成 Surface（ANativeWindow）
            // 後經 AIDL 的 android.view.Surface 傳給 server。
            sp<IGraphicBufferProducer> gbp = interface_cast<IGraphicBufferProducer>(binder);
            sp<Surface> surface;
            if (gbp != nullptr)
                surface = new Surface(gbp, true);
            ret = mCacao->process(paramMem, paramSd, surface.get(),
                                  resultMem, resultSd, 0, cb);
#else
            ret = -1;
#endif
            if (ret != 0)
            {
                if (ret != -0x6e)
                    ret = -0x6f;
            }
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ── cancel ────────────────────────────────────────────────────
    // [已確認/已修正] 反編譯 tools_Libcacao/refs/so_32/libcacao_client.so 的
    // android::BpCacao::cancel(int) 確認：它是透過 IBinder::transact(code=8, ...)
    // 送出的 Binder IPC 呼叫，不是對 mCacao 這個 C++ 物件本身做任意 vtable offset
    // 呼叫（先前 vt[0x2c/4] 的寫法完全是猜錯欄位）。ICacao 介面
    // （libcacao_service/include/cacao/ICacao.h）已經正確宣告
    // `virtual int cancel(int cookie) = 0;`，而 ICacao.cpp 裡的 BpCacao::cancel
    // 實作本身就是用 transact(8, ...)——所以只需要透過正常的 C++ virtual dispatch
    // 呼叫 mCacao->cancel(cookie) 即可，不需要（也不應該）手動戳 vtable。
    int Cacao::CacaoClient::cancel(void *cookie)
    {
        pthread_mutex_lock(&mLock);
        if (mCacao == nullptr)
        {
            pthread_mutex_unlock(&mLock);
            return -0x65;
        }
        int ret = mCacao->cancel(static_cast<int>(reinterpret_cast<intptr_t>(cookie)));
        if (ret != 0)
        {
            if (ret != -0x6e)
                ret = -0x6f;
        }
        pthread_mutex_unlock(&mLock);
        return ret;
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao — facade 建構子/解構子
    // ══════════════════════════════════════════════════════════════

    Cacao::Cacao()
        : mCacaoClient(nullptr)
    {
    }

    Cacao::~Cacao()
    {
        disconnect();
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::getService() — 查找並初始化 ICacaoService singleton
    // 原始 .so 邏輯:
    //   mutex_lock(mLock)
    //   if mService == null:
    //     sm = defaultServiceManager()
    //     loop (最多 10 次, 每次 sleep 500ms):
    //       binder = sm->getService("cacao")   // 服務注冊名稱（非 Binder 描述符）
    //       if binder != null: break
    //       ALOGI("waiting...")
    //     if binder == null: ALOGE("no service")
    //     else:
    //       mService = ICacaoService::asInterface(binder)
    //       mServicePid = mService->getServicePid()
    //       if mDeathNotifier == null:
    //         mDeathNotifier = new DeathNotifier()
    //         asBinder(mService).linkToDeath(mDeathNotifier)
    //   mutex_unlock(mLock)
    // ══════════════════════════════════════════════════════════════

    // AIDL 服務死亡通知（取代 DeathNotifier::binderDied 的 binder 版本）
    static void onAidlServiceDied()
    {
        pthread_mutex_lock(&Cacao::mServiceLock);
        Cacao::mService.clear();
        Cacao::mDeathNotifier.clear();
        Cacao::mServicePid = 0;
        ALOGI("cacao service died");
        pthread_mutex_unlock(&Cacao::mServiceLock);
    }

    void Cacao::getService()
    {
        pthread_mutex_lock(&mServiceLock);
        if (mService == nullptr)
        {
            mService = cacaoAidlGetService(onAidlServiceDied);
            if (mService == nullptr)
            {
                ALOGE("getService: cacao service not found");
            }
            else
            {
                mServicePid = mService->getServicePid();
                if (mDeathNotifier == nullptr)
                {
                    mDeathNotifier = new DeathNotifier();
                }
            }
        }
        pthread_mutex_unlock(&mServiceLock);
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::create() — 建立並連線的 Cacao 工廠
    // 原始 .so 邏輯:
    //   getService()
    //   ProcessState::self()->startThreadPool()
    //   if mService == null: return null
    //   if isSameProcess(): return null  (不支援同 process 連線)
    //   cacao = operator_new(8); ctor
    //   if connect(cacao) failed: dtor; return null
    //   return cacao
    // ══════════════════════════════════════════════════════════════

    Cacao *Cacao::create()
    {
        getService();
        ProcessState::self()->startThreadPool();
        if (mService == nullptr)
        {
            return nullptr;
        }
        // 同 process 不支援
        if (mServicePid == getpid())
        {
            return nullptr;
        }
        Cacao *cacao = new Cacao();
        int ret = cacao->connect();
        if (ret < 0)
        {
            delete cacao;
            return nullptr;
        }
        return cacao;
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::connect()
    // 原始 .so:
    //   getService()
    //   if mService==null: return 0 (or error)
    //   create CacaoClient(0x30 bytes)
    //   mCacaoClient = sp<CacaoClient>(client)
    //   ret = CacaoClient::connect(mCacaoClient, mService)
    //   if failed: disconnect(this); return ret
    // ══════════════════════════════════════════════════════════════

    int Cacao::connect()
    {
        getService();
        if (mService == nullptr)
        {
            return 0; // 原始返回 0 when no service
        }
        CacaoClient *client = new CacaoClient();
        mCacaoClient = client; // 使用 raw pointer（facade 保存 raw ptr）
        if (mCacaoClient == nullptr)
        {
            return (int)0xffffff9a; // -0x66
        }
        int ret = client->connect(mService);
        if (ret < 0)
        {
            disconnect();
        }
        return ret;
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::disconnect()
    // 原始 .so:
    //   getService()
    //   if mService && mCacaoClient:
    //     CacaoClient::disconnect()
    //     clear sp<CacaoClient>
    // ══════════════════════════════════════════════════════════════

    void Cacao::disconnect()
    {
        getService();
        if (mService != nullptr && mCacaoClient != nullptr)
        {
            mCacaoClient->disconnect();
            mCacaoClient.clear(); // sp<> release: allows RefBase to manage lifetime safely
        }
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::subscribeEvents()
    // ══════════════════════════════════════════════════════════════

    void Cacao::subscribeEvents(cacao::ICBProcessEventListener *listener, void *cookie)
    {
        if (mCacaoClient != nullptr)
        {
            mCacaoClient->subscribeEvents(listener, cookie);
        }
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::start/stop/setConfig/getConfig/process/processAsync/etc.
    // 原始 .so: 所有這些方法都透過 mCacaoClient vtable 分派
    // ══════════════════════════════════════════════════════════════

    int Cacao::start(const cacao::ProcessModeBase *mode)
    {
        if (mCacaoClient != nullptr)
        {
            return mCacaoClient->start(mode);
        }
        return 0;
    }

    int Cacao::stop()
    {
        if (mCacaoClient != nullptr)
        {
            return mCacaoClient->stop();
        }
        return 0;
    }

    int Cacao::setConfig(const cacao::ProcessConfigBase *cfg)
    {
        if (mCacaoClient == nullptr)
            return -0x65;
        return mCacaoClient->setConfig(cfg);
    }

    int Cacao::getConfig(cacao::ProcessConfigBase *cfg)
    {
        if (mCacaoClient == nullptr)
            return -0x65;
        return mCacaoClient->getConfig(cfg);
    }

    void Cacao::process(const cacao::ProcessParamBase *param,
                        cacao::ICBProcessResultListener *listener,
                        cacao::ProcessResultBase *result)
    {
        if (mCacaoClient != nullptr)
        {
            mCacaoClient->process(param, listener, result);
        }
    }

    void Cacao::processAsync(const cacao::ProcessParamBase *param,
                             android::Vector<cacao::ImageBuf *> *imageBufs,
                             cacao::ICBProcessResultListener *listener,
                             cacao::ProcessResultBase *result)
    {
        if (mCacaoClient != nullptr)
        {
            mCacaoClient->processAsync(param, imageBufs, listener, result);
        }
    }

    void Cacao::processAsyncWithBinder(const cacao::ProcessParamBase *param,
                                       const sp<IBinder> &binder,
                                       cacao::ICBProcessResultListener *listener,
                                       cacao::ProcessResultBase *result)
    {
        if (mCacaoClient != nullptr)
        {
            mCacaoClient->processAsyncWithBinder(param, binder, listener, result);
        }
    }

    int Cacao::cancel(void *cookie)
    {
        if (mCacaoClient == nullptr)
            return -0x65;
        return mCacaoClient->cancel(cookie);
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::getCaps()
    // 原始 .so:
    //   getService()
    //   if mService==null || isSameProcess: return 0/-1
    //   allocMemory(caps->getSerializedSize())
    //   caps->serialize(&sd)
    //   mService->getCaps(camIdx, mem, sd)
    //   if ok: caps->deserialize(&sd) (update sd.size/data from mem first)
    // ══════════════════════════════════════════════════════════════

    int Cacao::getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                       cacao::Caps *caps)
    {
        getService();
        if (mService == nullptr)
        {
            return 0;
        }
        if (mServicePid == getpid())
        {
            return 0; // same process fallback
        }
        if (caps == nullptr)
            return -0x67;

        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        // allocMemory for caps serialized data
        // 原版: vtable+0x10 = getSerializedSize, vtable+0x14 = serialize
        typedef uint32_t (*GetSzFn)(cacao::Caps *);
        typedef int (*SerFn)(cacao::Caps *, cacao::ISerialize::SerializedData *);
        void **vt = *reinterpret_cast<void ***>(caps);
        uint32_t sz = reinterpret_cast<GetSzFn>(vt[0x10 / 4])(caps);

        // 原版記憶體分配方式: sp<IMemory> mem = allocMemory_common(sz)
        sp<IMemory> mem = allocMemory_common(sz);
        if (mem == nullptr)
            return -0x66;
        sd.size = (uint32_t)mem->size();
        sd.data = mem->unsecurePointer();

        // caps->serialize(&sd): vtable+0x14
        int ret = reinterpret_cast<SerFn>(vt[0x14 / 4])(caps, &sd);
        if (ret < 0)
        {
            return ret;
        }

        // mService->getCaps(camIdx, mem, sd)
        ret = mService->getCaps(camIdx, mem, sd);
        if (ret != 0)
        {
            if (ret != -0x6e)
                ret = -0x6f;
        }
        if (ret == 0)
        {
            // 反序列化回傳結果: vtable+0x18
            // 原版 binary: 在 deserialize 前重置 sd.offset = 0
            sd.offset = 0;
            typedef int (*DesFn)(cacao::Caps *, cacao::ISerialize::SerializedData *);
            reinterpret_cast<DesFn>(vt[0x18 / 4])(caps, &sd);
        }
        return ret;
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::getJpegBufferSize()
    // 原始 .so: getService() → mService->getJpegBufferSize(size) if not same process
    // ══════════════════════════════════════════════════════════════

    int Cacao::getJpegBufferSize(cacao::ImageSize size)
    {
        getService();
        if (mService == nullptr)
            return 0;
        if (mServicePid == getpid())
            return 0;
        return mService->getJpegBufferSize(size);
    }

    // ══════════════════════════════════════════════════════════════
    // Cacao::isSameProcess()
    // 原始 .so: getService() → if mService && mServicePid == getpid() → true
    // ══════════════════════════════════════════════════════════════

    bool Cacao::isSameProcess()
    {
        getService();
        if (mService == nullptr)
            return false;
        return (mServicePid == (int)getpid());
    }

    // ══════════════════════════════════════════════════════════════
    // int getCaps/setConfig/getConfig with raw IMemory+SD
    // (直接呼叫 mService 版本，供 ProcessCtrlCapsFactory 等使用)
    // ══════════════════════════════════════════════════════════════

    int Cacao::getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                       const sp<IMemory> &mem,
                       cacao::ISerialize::SerializedData sd)
    {
        if (mCacaoClient == nullptr)
        {
            getService();
            if (mService == nullptr)
                return -0x65;
            return mService->getCaps(camIdx, mem, sd);
        }
        return -0x65;
    }

    // [根因已確認，20260801] 這兩個 raw (configId, mem, sd) overload 先前的實作
    // 完全沒用到自己的參數，直接呼叫 mCacaoClient 另一個 overload
    // （ProcessConfigBase*）並傳 nullptr，等於是無條件失敗的假實作。反編譯
    // libcacao_client.cpp（0000eb70 BpCacao::setConfig / 0000ec50
    // BpCacao::getConfig，兩者都是本檔案內既有的完整反編譯內容）確認這是走
    // Binder transaction code 4/5 直接送出 (configId, mem, sd) 的低階呼叫，
    // 跟 mCacaoClient->mCacao（sp<ICacao> Binder proxy）的
    // setConfig(int,...)/getConfig(int,...) 完全同構——改為直接透過這個
    // proxy 轉送，不再丟棄呼叫端傳入的參數。
    // 目前這兩個 overload 在本專案自己的呼叫路徑中沒有任何呼叫者（JNI 層
    // 只用 ProcessConfigBase* 版本），但屬於原版公開的 Cacao class API
    // surface，仍依原版行為修正。
    int Cacao::getConfig(int configId, const sp<IMemory> &mem,
                         cacao::ISerialize::SerializedData sd)
    {
        if (mCacaoClient == nullptr || mCacaoClient->mCacao == nullptr)
            return -0x65;
        return mCacaoClient->mCacao->getConfig(configId, mem, sd);
    }

    int Cacao::setConfig(int configId, const sp<IMemory> &mem,
                         cacao::ISerialize::SerializedData sd)
    {
        if (mCacaoClient == nullptr || mCacaoClient->mCacao == nullptr)
            return -0x65;
        return mCacaoClient->mCacao->setConfig(configId, mem, sd);
    }

} // namespace android
