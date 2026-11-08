// CacaoAidlClient.cpp — app 端：以 stable AIDL（vendor.somc.cacao）連到 cacao 服務
//
// 對外 C++ 介面（android::ICacaoService / ICacao / ICacaoClient / ICacaoProcessCallback，
// 參數仍是 sp<IMemory> + SerializedData）完全不變，Cacao.cpp 與 JNI 不需要改。
// 這裡只把「線上傳輸」從手寫的 Parcel（BpCacao 等）換成 AIDL：
//   AidlCacaoService : ICacaoService   包 aidl ICacaoService
//   AidlCacao        : ICacao          包 aidl ICacao
//   ClientStub       : aidl BnCacaoClient            → 轉呼叫 sp<ICacaoClient>
//   ProcCbStub       : aidl BnCacaoProcessCallback   → 轉呼叫 sp<ICacaoProcessCallback>

#define LOG_TAG "CacaoAidlClient"

#include <android/binder_interface_utils.h>
#include <android/binder_parcel_utils.h>
#include <android/binder_auto_utils.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <log/log.h>
#include <aidl/vendor/somc/cacao/BnCacaoClient.h>
#include <aidl/vendor/somc/cacao/BnCacaoProcessCallback.h>
#include <aidl/vendor/somc/cacao/ICacao.h>
#include <aidl/vendor/somc/cacao/ICacaoService.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <cutils/native_handle.h>
#include <utils/Vector.h>

#include <android/native_window_aidl.h>

#include <cacao/CacaoAidlUtil.h>
#include <cacao/CacaoAidlClient.h>
#include <cacao/ICacao.h>
#include <cacao/ICacaoClient.h>
#include <cacao/ICacaoProcessCallback.h>
#include <cacao/ICacaoService.h>
#include <cacao/ProcessCtrlGatewayBase.h>

#include <unistd.h>

namespace
{
    namespace aidlns = aidl::vendor::somc::cacao;

    const char *kServiceName = "vendor.somc.cacao.ICacaoService/default";

    // ── server → app：事件回呼 ───────────────────────────────────
    class ClientStub : public aidlns::BnCacaoClient
    {
    public:
        explicit ClientStub(const android::sp<android::ICacaoClient> &client) : mClient(client) {}

        ndk::ScopedAStatus notifyEvent(int32_t eventType, const aidlns::CacaoBlob &data) override
        {
            android::sp<android::IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            mClient->notifyEvent(eventType, mem, sd);
            return ndk::ScopedAStatus::ok();
        }

    private:
        android::sp<android::ICacaoClient> mClient;
    };

    // ── server → app：process 進度／結果回呼 ─────────────────────
    class ProcCbStub : public aidlns::BnCacaoProcessCallback
    {
    public:
        explicit ProcCbStub(const android::sp<android::ICacaoProcessCallback> &cb) : mCb(cb) {}

        ndk::ScopedAStatus notifyProgress(const aidlns::CacaoBlob &data) override
        {
            android::sp<android::IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            mCb->notifyProgress(mem, sd);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus notifyResult(const aidlns::CacaoBlob &data) override
        {
            android::sp<android::IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            mCb->notifyResult(mem, sd, nullptr);
            return ndk::ScopedAStatus::ok();
        }

    private:
        android::sp<android::ICacaoProcessCallback> mCb;
    };

    std::shared_ptr<aidlns::ICacaoProcessCallback> wrapCallback(
        const android::sp<android::ICacaoProcessCallback> &cb)
    {
        if (cb == nullptr)
            return nullptr;
        return ndk::SharedRefBase::make<ProcCbStub>(cb);
    }

    // 與 BpCacao::process 相同：用 vtable 取 ImageBuf 的尺寸／dataSpace／native handle。
    aidlns::CacaoImage toImage(cacao::ImageBuf *buf)
    {
        aidlns::CacaoImage img;
        img.width = 0;
        img.height = 0;
        img.dataSpace = 0;
        if (buf != nullptr)
        {
            typedef uint64_t (*GetImageSizeFn)(cacao::ImageBuf *);
            typedef int32_t (*GetSFn)(cacao::ImageBuf *);
            typedef const native_handle_t *(*GetNHFn)(cacao::ImageBuf *);
            void **vt = *reinterpret_cast<void ***>(buf);
            uint64_t imgSize = reinterpret_cast<GetImageSizeFn>(vt[7])(buf);
            img.width = (int32_t)(uint32_t)(imgSize);
            img.height = (int32_t)(uint32_t)(imgSize >> 32);
            img.dataSpace = reinterpret_cast<GetSFn>(vt[15])(buf);
            const native_handle_t *nh = reinterpret_cast<GetNHFn>(vt[4])(buf);
            if (nh != nullptr)
                img.handle = android::dupToAidl(nh);
        }
        return img;
    }

    // ── app → server：ICacao proxy ───────────────────────────────
    class AidlCacao : public android::ICacao
    {
    public:
        AidlCacao(const std::shared_ptr<aidlns::ICacao> &cacao,
                  const std::shared_ptr<aidlns::BnCacaoClient> &clientKeepAlive)
            : mCacao(cacao), mClientKeepAlive(clientKeepAlive)
        {
        }

        AIBinder *aibinder() const { return mCacao->asBinder().get(); }

        void disconnect() override { mCacao->disconnect(); }

        int start(const android::sp<android::IMemory> &mem,
                  cacao::ISerialize::SerializedData sd) override
        {
            int32_t ret = -1;
            auto st = mCacao->start(cacao_aidl::toBlob(mem, sd), &ret);
            return st.isOk() ? ret : -1;
        }

        int stop() override
        {
            int32_t ret = -1;
            auto st = mCacao->stop(&ret);
            return st.isOk() ? ret : -1;
        }

        int setConfig(int configId, const android::sp<android::IMemory> &mem,
                      cacao::ISerialize::SerializedData sd) override
        {
            int32_t ret = -1;
            auto st = mCacao->setConfig(configId, cacao_aidl::toBlob(mem, sd), &ret);
            return st.isOk() ? ret : -1;
        }

        int getConfig(int configId, const android::sp<android::IMemory> &mem,
                      cacao::ISerialize::SerializedData sd) override
        {
            int32_t ret = -1;
            auto st = mCacao->getConfig(configId, cacao_aidl::toBlob(mem, sd), &ret);
            return st.isOk() ? ret : -1;
        }

        int process(const android::sp<android::IMemory> &paramMem,
                    cacao::ISerialize::SerializedData paramSd,
                    android::Vector<cacao::ImageBuf *> bufs,
                    const android::sp<android::IMemory> &resultMem,
                    cacao::ISerialize::SerializedData resultSd,
                    int flags,
                    const android::sp<android::ICacaoProcessCallback> &callback) override
        {
            std::vector<aidlns::CacaoImage> images;
            for (size_t i = 0; i < bufs.size(); i++)
                images.push_back(toImage(bufs[i]));
            int64_t ret = -1;
            auto st = mCacao->process(cacao_aidl::toBlob(paramMem, paramSd), images,
                                      cacao_aidl::toBlob(resultMem, resultSd), flags,
                                      wrapCallback(callback), &ret);
            return st.isOk() ? (int)ret : -1;
        }

        int process(const android::sp<android::IMemory> &paramMem,
                    cacao::ISerialize::SerializedData paramSd,
                    ANativeWindow *window,
                    const android::sp<android::IMemory> &resultMem,
                    cacao::ISerialize::SerializedData resultSd,
                    int flags,
                    const android::sp<android::ICacaoProcessCallback> &callback) override
        {
            // 影片錄製：以 android.view.Surface（ANativeWindow）傳給 server。
            if (window == nullptr)
                return -1;
            aidl::android::view::Surface sw;
            sw.reset(window);
            int64_t ret = -1;
            auto st = mCacao->processToSurface(cacao_aidl::toBlob(paramMem, paramSd), sw,
                                               cacao_aidl::toBlob(resultMem, resultSd), flags,
                                               wrapCallback(callback), &ret);
            return st.isOk() ? (int)ret : -1;
        }

        int cancel(int cookie) override
        {
            int32_t ret = -1;
            auto st = mCacao->cancel(cookie, &ret);
            return st.isOk() ? ret : -1;
        }

    private:
        std::shared_ptr<aidlns::ICacao> mCacao;
        std::shared_ptr<aidlns::BnCacaoClient> mClientKeepAlive;
    };

    // ── app → server：ICacaoService proxy ────────────────────────
    class AidlCacaoService : public android::ICacaoService
    {
    public:
        explicit AidlCacaoService(const std::shared_ptr<aidlns::ICacaoService> &svc) : mSvc(svc) {}

        android::sp<android::ICacao> connect(const android::sp<android::ICacaoClient> &client) override
        {
            auto stub = ndk::SharedRefBase::make<ClientStub>(client);
            std::shared_ptr<aidlns::ICacao> cacao;
            auto st = mSvc->connect(stub, &cacao);
            if (!st.isOk() || cacao == nullptr)
                return nullptr;
            return new AidlCacao(cacao, stub);
        }

        int getServicePid() override
        {
            int32_t pid = 0;
            auto st = mSvc->getServicePid(&pid);
            return st.isOk() ? pid : 0;
        }

        int getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                    const android::sp<android::IMemory> &mem,
                    cacao::ISerialize::SerializedData sd) override
        {
            int32_t ret = -1;
            auto st = mSvc->getCaps(camIdx.index, cacao_aidl::toBlob(mem, sd), &ret);
            return st.isOk() ? ret : -1;
        }

        int getJpegBufferSize(cacao::ImageSize size) override
        {
            int32_t ret = -1;
            auto st = mSvc->getJpegBufferSize((int32_t)size.width, (int32_t)size.height, &ret);
            return st.isOk() ? ret : -1;
        }

        AIBinder *aibinder() const { return mSvc->asBinder().get(); }

    private:
        std::shared_ptr<aidlns::ICacaoService> mSvc;
    };

    // ── 死亡通知 ─────────────────────────────────────────────────
    void onServiceDied(void *cookie)
    {
        auto fn = reinterpret_cast<void (*)()>(cookie);
        if (fn)
            fn();
    }

    void onCacaoDied(void * /*cookie*/) { ALOGI("cacao died"); }

    AIBinder_DeathRecipient *serviceRecipient()
    {
        static AIBinder_DeathRecipient *r = AIBinder_DeathRecipient_new(onServiceDied);
        return r;
    }

    AIBinder_DeathRecipient *cacaoRecipient()
    {
        static AIBinder_DeathRecipient *r = AIBinder_DeathRecipient_new(onCacaoDied);
        return r;
    }

} // namespace

namespace android
{

    // 與原本 getService 相同：最多重試 10 次、每次 500ms。
    sp<ICacaoService> cacaoAidlGetService(void (*onDied)())
    {
        ndk::SpAIBinder binder;
        for (unsigned int i = 1; i <= 10; i++)
        {
            binder.set(AServiceManager_checkService(kServiceName));
            if (binder.get() != nullptr)
                break;
            usleep(500000);
            ALOGI("getService: waiting for cacao service... (%u)", i);
        }
        if (binder.get() == nullptr)
            return nullptr;
        std::shared_ptr<aidlns::ICacaoService> svc = aidlns::ICacaoService::fromBinder(binder);
        if (svc == nullptr)
            return nullptr;
        sp<AidlCacaoService> proxy = new AidlCacaoService(svc);
        if (onDied != nullptr)
            AIBinder_linkToDeath(binder.get(), serviceRecipient(), reinterpret_cast<void *>(onDied));
        return proxy;
    }

    bool cacaoAidlIsCacao(const sp<ICacao> &cacao)
    {
        return cacao != nullptr;
    }

    void cacaoAidlLinkCacaoDeath(const sp<ICacao> &cacao)
    {
        if (!cacaoAidlIsCacao(cacao))
            return;
        AIBinder_linkToDeath(static_cast<AidlCacao *>(cacao.get())->aibinder(), cacaoRecipient(), nullptr);
    }

    void cacaoAidlUnlinkCacaoDeath(const sp<ICacao> &cacao)
    {
        if (!cacaoAidlIsCacao(cacao))
            return;
        AIBinder_unlinkToDeath(static_cast<AidlCacao *>(cacao.get())->aibinder(), cacaoRecipient(), nullptr);
    }

} // namespace android
