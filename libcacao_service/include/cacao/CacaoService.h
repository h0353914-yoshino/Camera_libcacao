// CacaoService.h — android::CacaoService 實作類別宣告
// 依照 libcacao_service.so Ghidra 反編譯結果 100% 重建
// 物件佈局、vtable 偏移均從 Ghidra read_memory 驗證

#pragma once

#include <stdint.h>
#include <pthread.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <utils/List.h>
#include <utils/String16.h>
#include <utils/Vector.h>

#include <hidl/HidlSupport.h>
#include <hidl/ServiceManagement.h>
#include <android/hidl/manager/1.0/IServiceNotification.h>

#include <vendor/somc/hardware/camera/provider/1.0/IVendorSomcCameraProvider.h>
#include <vendor/somc/hardware/camera/cacao/3.0/ICacao.h>
#include <vendor/somc/hardware/camera/cacao/3.0/types.h>
#include <vendor/somc/hardware/camera/cacao/3.1/ICacao.h>

#include <cacao/cacao_pal.h>
#include <cacao/ProcessCtrlGatewayBase.h>
#include <cacao/ICacaoService.h>
#include <cacao/ICacao.h>
#include <cacao/ICacaoClient.h>
#include <cacao/ICacaoProcessCallback.h>

namespace cacao
{

    // Caps 序列化包裝
    // getCaps 中使用，將 HIDL CacaoCaps 轉換為 Binder 可傳輸的 SerializedData
    class Caps : public ObjectBase
    {
    public:
        Caps();
        virtual ~Caps();

        // 執行 4 段 memcpy，總計 0x262c bytes
        int serialize(cacao::ISerialize::SerializedData *sd);
        int deserialize(cacao::ISerialize::SerializedData *sd);

        // 佈局：0x262c bytes 的 caps 資料（多個 vec 展開後的 raw 資料）
        uint8_t data[0x2630]; // 足夠容納完整的 caps 序列化資料
    };

    // ProcessFactoryInternal — 工廠方法 delegate
    // 符號在 libcacao_service.so 中定義，轉發到 libcacao_pal.so 的 ProcessCtrlGatewayFactory
    class ProcessFactoryInternal
    {
    public:
        static ICacaoGateway *createProcess();
        static ProcessModeBase *createMode(cacao::ISerialize::SerializedData *sd);
        static ProcessParamBase *createParam(cacao::ISerialize::SerializedData *sd);
        static ProcessParamBase *createParam(ANativeWindow *window,
                                             cacao::ISerialize::SerializedData *sd);
        static ProcessResultBase *createResult(cacao::ISerialize::SerializedData *sd);
        static ProcessConfigBase *createConfig(unsigned int configId,
                                               cacao::ISerialize::SerializedData *sd);
    };

} // namespace cacao

namespace android
{

    namespace V3_0 = ::vendor::somc::hardware::camera::cacao::V3_0;
    namespace V3_1 = ::vendor::somc::hardware::camera::cacao::V3_1;
    namespace provider_V1_0 = ::vendor::somc::hardware::camera::provider::V1_0;
    using ::android::hardware::Return;

    class CacaoService
        : public ICacaoService,
          public virtual hidl::manager::V1_0::IServiceNotification,
          public virtual hardware::hidl_death_recipient
    {
    public:
        class Client; // 前向宣告，完整定義在下方

        static void instantiate();

        CacaoService();
        virtual ~CacaoService();

        // ICacaoService 介面實作
        virtual sp<ICacao> connect(const sp<ICacaoClient> &client) override;
        virtual int getServicePid() override;
        virtual int getCaps(const ::cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                            const sp<IMemory> &mem,
                            ::cacao::ISerialize::SerializedData sd) override;
        virtual int getJpegBufferSize(::cacao::ImageSize size) override;

        // CacaoService 自身方法
        void registerClient(const sp<Client> &client);
        void unregisterClient(const sp<ICacaoClient> &client);

        virtual void onFirstRef();

        // HIDL 回呼
        virtual Return<void> onRegistration(
            const hardware::hidl_string &fqName,
            const hardware::hidl_string &name,
            bool preexisting) override;
        virtual void serviceDied(uint64_t cookie,
                                 const wp<hidl::base::V1_0::IBase> &who) override;

        // 內部工具方法
        pid_t getClientPid();
        uid_t getClientUid();
        void getInterfaces();

        // ── Client 巢狀類別 ──────────────────────
        class Client
            : public ICacao,
              public ::cacao::ICBProcessEventListener,
              public ::cacao::ICBProcessResultListener
        {
        public:
            // ── Request 巢狀類別 ─────────────
            class Request : public RefBase
            {
            public:
                Request(const sp<ICacaoProcessCallback> &cb,
                        ::cacao::ICBProcessResultListener *listener);
                virtual ~Request();

                void onHandleProgress(const ::cacao::ProcessResultBase *result);
                void onHandleResult(const ::cacao::ProcessResultBase *result);
                void setBufVector(Vector<::cacao::ImageBuf *> *bufs);
                void setResult(::cacao::ProcessResultBase *result);
                ::cacao::ProcessResultBase *getResult() const { return mResult; }
                Vector<::cacao::ImageBuf *> *getBufVector() const { return mBufVector; }
                sp<ICacaoProcessCallback> getCallback() const { return mCallback; }
                int getErrCode() const { return 0; }

            private:
                sp<ICacaoProcessCallback> mCallback;          // +0x04
                ::cacao::ICBProcessResultListener *mListener; // +0x08
                Vector<::cacao::ImageBuf *> *mBufVector;      // +0x0C
                ::cacao::ProcessResultBase *mResult;          // +0x10
            };

            // sizeof(Client) = 0x48
            Client(const sp<CacaoService> &service,
                   const sp<ICacaoClient> &client,
                   int callingPid);
            virtual ~Client();

            // ICacao 介面實作
            virtual void disconnect() override;
            virtual int start(const sp<IMemory> &mem,
                              ::cacao::ISerialize::SerializedData sd) override;
            virtual int stop() override;
            virtual int setConfig(int configId, const sp<IMemory> &mem,
                                  ::cacao::ISerialize::SerializedData sd) override;
            virtual int getConfig(int configId, const sp<IMemory> &mem,
                                  ::cacao::ISerialize::SerializedData sd) override;
            virtual int process(const sp<IMemory> &paramMem,
                                ::cacao::ISerialize::SerializedData paramSd,
                                Vector<::cacao::ImageBuf *> bufs,
                                const sp<IMemory> &resultMem,
                                ::cacao::ISerialize::SerializedData resultSd,
                                int flags,
                                const sp<ICacaoProcessCallback> &callback) override;
            virtual int process(const sp<IMemory> &paramMem,
                                ::cacao::ISerialize::SerializedData paramSd,
                                ANativeWindow *window,
                                const sp<IMemory> &resultMem,
                                ::cacao::ISerialize::SerializedData resultSd,
                                int flags,
                                const sp<ICacaoProcessCallback> &callback) override;
            virtual int cancel(int cookie) override;

            // ICBProcessEventListener
            virtual void onHandleEvent(const ::cacao::ProcessEventBase *ev) override;

            // 記憶體管理
            static sp<IMemory> allocMemory(size_t size);
            void freeMemory(sp<IMemory> &mem);
            void makeSerializedData(const sp<IMemory> &mem,
                                    ::cacao::ISerialize::SerializedData *sd);

            void serviceDied();

            // getClient — 對應原廠 Client::getClient() const（Ghidra 0x1aed2，
            // 實作就是回傳 this+0x28 的 mClient）。registerClient/unregisterClient
            // 都是先呼叫它、再對回傳值取 asBinder，用 App 端的 ICacaoClient binder
            // 當作 linkToDeath 與比對的基準。原廠把它放在 vtable 裡（虛擬），
            // 這裡因為所有呼叫點都在本函式庫內，用直接存取器即可，行為等價。
            sp<ICacaoClient> getClient() const { return mClient; }

        private:
            // ICBProcessResultListener — 這些被 Request 和 gateway 呼叫
            virtual void onHandleProgress(const ::cacao::ProcessResultBase *result) override;
            virtual void onHandleResult(const ::cacao::ProcessResultBase *result) override;
            virtual ICBProcessResultListener *addRef() override { return this; }
            virtual void onResult(PAL_Err_t err) override {}
            virtual void onProgress(int progress) override {}

            // ICBProcessEventListener
            // virtual void onEvent(::cacao::ProcessEventBase* ev) override; // 已在上面

            // 欄位佈局 (sizeof = 0x48):
            // +0x00 ICacao vtable ptr (primary — BnCacao chain)
            // +0x04 BBinder
            // +0x10 ICBProcessEventListener vtable ptr
            // +0x14 ICBProcessResultListener vtable ptr
            pthread_mutex_t mMutexDisconnect; // +0x18
            pthread_mutex_t mMutexClient;     // +0x1c
            pthread_mutex_t mMutexRequest;    // +0x20
            sp<CacaoService> mService;        // +0x24
            sp<ICacaoClient> mClient;         // +0x28
            int mCallingPid;                  // +0x2c
            void *mProcess;                   // +0x30 — ProcessCtrlGatewayBase*（::cacao::ICacaoGateway*）
            uint8_t mDisconnected;            // +0x34
            // padding +0x35..+0x37
            // +0x38 List vtable ptr
            List<Request *> mRequestList; // +0x38..+0x44
            // total = 0x48
        };

    private:
        // CacaoService 物件佈局 (Ghidra 驗證):
        // +0x00   ICacaoService vtable ptr (primary)
        // +0x04   BBinder (embedded)
        // +0x10   secondary vtable ptr chain (IServiceNotification, hidl_death_recipient)
        pthread_mutex_t mLock;       // +0x14
        pthread_mutex_t mClientLock; // +0x18
        List<sp<Client>> mClients;   // +0x1c..+0x23
        // +0x24   retry counter for serviceDied
        int mRetryCount; // +0x24
        // +0x28/+0x2c：反編譯 android::CacaoService::getInterfaces()/serviceDied()
        // (so_32 @ 0x00019dd4 / 0x0001ad80) 確認這是一組 64-bit linkToDeath cookie
        // （低位在 +0x28、高位在 +0x2c），每次成功連上 provider 時遞增並傳給
        // linkToDeath；serviceDied() 收到通知時會比對傳入 cookie 與這裡存的值，
        // 不符就視為過期通知直接忽略。原本誤標為「unused sp / padding」。
        uint64_t mLinkCookie;         // +0x28
        sp<V3_0::ICacao> mService;    // +0x30
        sp<V3_1::ICacao> mServiceV31; // +0x34
    };

} // namespace android