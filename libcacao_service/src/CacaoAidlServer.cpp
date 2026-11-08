// CacaoAidlServer.cpp — server 端：以 stable AIDL（vendor.somc.cacao）對外提供 CacaoService
//
// 內部的 CacaoService / CacaoService::Client（BnCacaoService / BnCacao 的 C++ 實作，
// 參數仍是 sp<IMemory> + SerializedData）完全不動，這裡只在邊界轉換：
//   AidlServiceStub : aidl BnCacaoService   → 轉呼叫 sp<CacaoService>
//   AidlCacaoStub   : aidl BnCacao          → 轉呼叫 sp<ICacao>（CacaoService::Client）
//   AidlClientProxy : ICacaoClient          → 呼叫 app 端 aidl ICacaoClient（oneway）
//   AidlProcCbProxy : ICacaoProcessCallback → 呼叫 app 端 aidl ICacaoProcessCallback（oneway）

#define LOG_TAG "CacaoAidlServer"

#include <android/binder_interface_utils.h>
#include <android/binder_parcel_utils.h>
#include <android/binder_auto_utils.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/log.h>
#include <aidl/vendor/somc/cacao/BnCacao.h>
#include <aidl/vendor/somc/cacao/BnCacaoService.h>
#include <aidl/vendor/somc/cacao/ICacaoClient.h>
#include <aidl/vendor/somc/cacao/ICacaoProcessCallback.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <cutils/native_handle.h>
#include <utils/Vector.h>

#include <android/native_window_aidl.h>

#include <cacao/CacaoAidlServer.h>
#include <cacao/CacaoAidlUtil.h>
#include <cacao/CacaoService.h>
#include <cacao/ICacaoClient.h>
#include <cacao/ICacaoProcessCallback.h>

namespace
{
    namespace aidlns = aidl::vendor::somc::cacao;
    using namespace android;

    const char *kServiceName = "vendor.somc.cacao.ICacaoService/default";

    // ── 死亡通知 cookie ──────────────────────────────────────────
    struct DeathCookie
    {
        wp<CacaoService> svc;
        wp<ICacaoClient> client;
    };

    void onClientDied(void *cookie)
    {
        DeathCookie *c = static_cast<DeathCookie *>(cookie);
        sp<CacaoService> svc = c->svc.promote();
        sp<ICacaoClient> client = c->client.promote();
        if (svc != nullptr && client != nullptr)
        {
            svc->unregisterClient(client);
        }
    }

    AIBinder_DeathRecipient *clientRecipient()
    {
        static AIBinder_DeathRecipient *r = AIBinder_DeathRecipient_new(onClientDied);
        return r;
    }

    // ── server → app：事件 ───────────────────────────────────────
    class AidlClientProxy : public ICacaoClient
    {
    public:
        explicit AidlClientProxy(const std::shared_ptr<aidlns::ICacaoClient> &client)
            : mClient(client), mCookie(nullptr)
        {
        }

        ~AidlClientProxy() override { delete static_cast<DeathCookie *>(mCookie); }

        void notifyEvent(int eventType, const sp<IMemory> &mem,
                         cacao::ISerialize::SerializedData sd) override
        {
            mClient->notifyEvent(eventType, cacao_aidl::toBlob(mem, sd));
        }

        // 取代 IInterface::asBinder(client)->linkToDeath(service)
        void linkDeath(const sp<CacaoService> &svc)
        {
            DeathCookie *c = new DeathCookie();
            c->svc = svc;
            c->client = this;
            mCookie = c;
            AIBinder_linkToDeath(mClient->asBinder().get(), clientRecipient(), c);
        }

        void unlinkDeath()
        {
            if (mCookie != nullptr)
            {
                AIBinder_unlinkToDeath(mClient->asBinder().get(), clientRecipient(), mCookie);
            }
        }

    private:
        std::shared_ptr<aidlns::ICacaoClient> mClient;
        void *mCookie;
    };

    // ── server → app：process 進度／結果 ─────────────────────────
    class AidlProcCbProxy : public ICacaoProcessCallback
    {
    public:
        explicit AidlProcCbProxy(const std::shared_ptr<aidlns::ICacaoProcessCallback> &cb) : mCb(cb) {}

        void notifyProgress(const sp<IMemory> &mem, cacao::ISerialize::SerializedData sd) override
        {
            mCb->notifyProgress(cacao_aidl::toBlob(mem, sd));
        }

        void notifyResult(const sp<IMemory> &mem, cacao::ISerialize::SerializedData sd,
                          Vector<cacao::ImageBuf *> *bufs) override
        {
            mCb->notifyResult(cacao_aidl::toBlob(mem, sd));

            // 與 BpCacaoProcessCallback::notifyResult 相同：送出後就地清理 ImageBuf 的 native_handle。
            if (bufs != nullptr && bufs->size() > 0)
            {
                for (size_t i = 0; i < bufs->size(); i++)
                {
                    cacao::ImageBuf *buf = (*bufs)[i];
                    if (buf != nullptr)
                    {
                        void *nh_raw = buf->getNative();
                        if (nh_raw != nullptr)
                        {
                            native_handle_close(static_cast<const native_handle_t *>(nh_raw));
                            native_handle_delete(static_cast<native_handle_t *>(nh_raw));
                        }
                        delete buf;
                    }
                }
            }
        }

    private:
        std::shared_ptr<aidlns::ICacaoProcessCallback> mCb;
    };

    sp<ICacaoProcessCallback> unwrapCallback(const std::shared_ptr<aidlns::ICacaoProcessCallback> &cb)
    {
        if (cb == nullptr)
            return nullptr;
        return new AidlProcCbProxy(cb);
    }

    // ── app → server：ICacao ─────────────────────────────────────
    class AidlCacaoStub : public aidlns::BnCacao
    {
    public:
        explicit AidlCacaoStub(const android::sp<android::ICacao> &cacao) : mCacao(cacao) {}

        ndk::ScopedAStatus disconnect() override
        {
            mCacao->disconnect();
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus start(const aidlns::CacaoBlob &mode, int32_t *ret) override
        {
            sp<IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(mode, &mem, &sd);
            *ret = mCacao->start(mem, sd);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus stop(int32_t *ret) override
        {
            *ret = mCacao->stop();
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus setConfig(int32_t configId, const aidlns::CacaoBlob &data, int32_t *ret) override
        {
            sp<IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            *ret = mCacao->setConfig(configId, mem, sd);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus getConfig(int32_t configId, const aidlns::CacaoBlob &data, int32_t *ret) override
        {
            sp<IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            *ret = mCacao->getConfig(configId, mem, sd);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus process(const aidlns::CacaoBlob &param,
                                   const std::vector<aidlns::CacaoImage> &images,
                                   const aidlns::CacaoBlob &result, int32_t flags,
                                   const std::shared_ptr<aidlns::ICacaoProcessCallback> &callback,
                                   int64_t *ret) override
        {
            sp<IMemory> paramMem, resultMem;
            cacao::ISerialize::SerializedData paramSd, resultSd;
            cacao_aidl::fromBlob(param, &paramMem, &paramSd);
            cacao_aidl::fromBlob(result, &resultMem, &resultSd);

            Vector<cacao::ImageBuf *> bufs;
            for (const auto &img : images)
            {
                native_handle_t *nh = nullptr;
                if (!img.handle.fds.empty() || !img.handle.ints.empty())
                    nh = android::dupFromAidl(img.handle);
                // 與 BnCacao::onTransact(code 6) 相同：ObjectBase::operator new(0x28) + 帶 handle 的建構子，
                // native_handle 由 ImageBuf／回呼清理流程接手。
                void *raw = cacao::ObjectBase::operator new(0x28);
                if (raw != nullptr)
                {
                    cacao::ImageBuf *buf = ::new (raw) cacao::ImageBuf(
                        static_cast<void *>(nh),
                        cacao::ImageSize{(uint32_t)img.width, (uint32_t)img.height},
                        static_cast<cacao::DataSpace>(img.dataSpace),
                        (cacao::ImageFormat)0);
                    bufs.push_back(buf);
                }
            }

            *ret = mCacao->process(paramMem, paramSd, bufs, resultMem, resultSd, flags,
                                   unwrapCallback(callback));
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus processToSurface(const aidlns::CacaoBlob &param,
                                            const aidl::android::view::Surface &output,
                                            const aidlns::CacaoBlob &result, int32_t flags,
                                            const std::shared_ptr<aidlns::ICacaoProcessCallback> &callback,
                                            int64_t *ret) override
        {
            *ret = -1;
            ANativeWindow *w = output.get();
            if (w == nullptr)
                return ndk::ScopedAStatus::ok();

            sp<IMemory> paramMem, resultMem;
            cacao::ISerialize::SerializedData paramSd, resultSd;
            cacao_aidl::fromBlob(param, &paramMem, &paramSd);
            cacao_aidl::fromBlob(result, &resultMem, &resultSd);
            *ret = mCacao->process(paramMem, paramSd, w, resultMem, resultSd, flags,
                                   unwrapCallback(callback));
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus cancel(int32_t cookie, int32_t *ret) override
        {
            *ret = mCacao->cancel(cookie);
            return ndk::ScopedAStatus::ok();
        }

    private:
        android::sp<android::ICacao> mCacao;
    };

    // ── app → server：ICacaoService ──────────────────────────────
    class AidlServiceStub : public aidlns::BnCacaoService
    {
    public:
        explicit AidlServiceStub(const sp<CacaoService> &svc) : mSvc(svc) {}

        ndk::ScopedAStatus connect(const std::shared_ptr<aidlns::ICacaoClient> &client,
                                   std::shared_ptr<aidlns::ICacao> *ret) override
        {
            sp<ICacaoClient> proxy = new AidlClientProxy(client);
            android::sp<android::ICacao> cacao = mSvc->connect(proxy);
            if (cacao == nullptr)
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
            *ret = ndk::SharedRefBase::make<AidlCacaoStub>(cacao);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus getServicePid(int32_t *ret) override
        {
            *ret = mSvc->getServicePid();
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus getCaps(int32_t cameraIndex, const aidlns::CacaoBlob &data, int32_t *ret) override
        {
            sp<IMemory> mem;
            cacao::ISerialize::SerializedData sd;
            cacao_aidl::fromBlob(data, &mem, &sd);
            cacao::ProcessCtrlCaps::CameraIndex idx;
            idx.index = cameraIndex;
            *ret = mSvc->getCaps(idx, mem, sd);
            return ndk::ScopedAStatus::ok();
        }

        ndk::ScopedAStatus getJpegBufferSize(int32_t width, int32_t height, int32_t *ret) override
        {
            cacao::ImageSize size;
            size.width = (uint32_t)width;
            size.height = (uint32_t)height;
            *ret = mSvc->getJpegBufferSize(size);
            return ndk::ScopedAStatus::ok();
        }

    private:
        sp<CacaoService> mSvc;
    };

    std::shared_ptr<AidlServiceStub> gStub;

} // namespace

namespace android
{

    bool cacaoAidlPublish(const sp<CacaoService> &svc)
    {
        gStub = ndk::SharedRefBase::make<AidlServiceStub>(svc);
        binder_status_t st = AServiceManager_addService(gStub->asBinder().get(), kServiceName);
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "addService %s -> %d", kServiceName, (int)st);
        return st == STATUS_OK;
    }

    bool cacaoAidlIsClient(const sp<ICacaoClient> &client)
    {
        return client != nullptr;
    }

    void cacaoAidlLinkClientDeath(const sp<ICacaoClient> &client, const sp<CacaoService> &svc)
    {
        if (cacaoAidlIsClient(client))
            static_cast<AidlClientProxy *>(client.get())->linkDeath(svc);
    }

    void cacaoAidlUnlinkClientDeath(const sp<ICacaoClient> &client)
    {
        if (cacaoAidlIsClient(client))
            static_cast<AidlClientProxy *>(client.get())->unlinkDeath();
    }

} // namespace android
