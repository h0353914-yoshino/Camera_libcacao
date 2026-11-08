// CacaoClient.h — android::Cacao 客戶端 facade 與內部類別宣告
// 依照 libcacao_client.so (ARM 32-bit) 反編譯結果重建
// 物件佈局與靜態成員均從 Ghidra 反編譯驗證

#pragma once

#include <stdint.h>
#include <pthread.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <utils/String16.h>

#include <cacao/ProcessCtrlGatewayBase.h>
#include <cacao/ICacaoService.h>
#include <cacao/ICacao.h>
#include <cacao/ICacaoClient.h>
#include <cacao/ICacaoProcessCallback.h>

namespace android
{

    // ── android::ICacaoGateway ───────────────────────────────────
    // 抽象介面：佔用 CacaoClient 的第三個次要 vtable（+0x14）
    // 對應原始 .so 佈局中 start/stop/process 等方法的 -20 thunk 來源
    // 注意：此類不繼承虛擬基類，以符合 Ghidra 無 construction vtable 的觀察
    class ICacaoGateway
    {
    public:
        virtual ~ICacaoGateway() {}
        virtual int subscribeEvents(cacao::ICBProcessEventListener *listener, void *cookie) = 0;
        virtual int start(const cacao::ProcessModeBase *mode) = 0;
        virtual int stop() = 0;
        virtual int setConfig(const cacao::ProcessConfigBase *cfg) = 0;
        virtual int getConfig(cacao::ProcessConfigBase *cfg) = 0;
        virtual int process(const cacao::ProcessParamBase *param,
                            cacao::ICBProcessResultListener *listener,
                            cacao::ProcessResultBase *result) = 0;
        virtual int processAsync(const cacao::ProcessParamBase *param,
                                 android::Vector<cacao::ImageBuf *> *imageBufs,
                                 cacao::ICBProcessResultListener *listener,
                                 cacao::ProcessResultBase *result) = 0;
        virtual int processAsyncWithBinder(const cacao::ProcessParamBase *param,
                                           const sp<IBinder> &binder,
                                           cacao::ICBProcessResultListener *listener,
                                           cacao::ProcessResultBase *result) = 0;
        virtual int cancel(void *cookie) = 0;
    };


    // ── android::Cacao::CacaoProcessCallback ─────────────────────
    // 進度/結果回呼：包裝 ICBProcessResultListener，透過 Binder 回報處理結果
    // sizeof = 0x?? (根據 Ghidra 重建)

    // ── android::Cacao::CacaoClient ──────────────────────────────
    // 客戶端：ICacaoClient 實作
    // 也負責 forward start/stop/process 等操作給 ICacao proxy
    // sizeof = 0x30
    // 物件佈局（依 Ghidra ctor 驗證）:
    //   +0x00: ICacaoClient vtable (primary, BnCacaoClient chain)
    //   +0x04: BBinder embedded (from BnInterface<ICacaoClient>)
    //   +0x10: IBinder::DeathRecipient secondary vtable
    //   +0x14: ICacaoGateway-like secondary vtable (for start/stop/etc thunks)
    //   +0x18: sp<ICacao> mCacao (NULL init)
    //   +0x1c: pthread_mutex_t mLock
    //   +0x20: ICBProcessEventListener* mListener (NULL init)
    //   +0x24: void* mCookie (NULL init)
    //   +0x28: RefBase (virtual base, 8 bytes)
    // 總計 = 0x30 bytes

    // ── android::Cacao ────────────────────────────────────────────
    // 主要客戶端 facade（隱藏 Binder 細節）
    // sizeof = 0x08
    // 物件佈局（依 Ghidra ctor 驗證）:
    //   +0x00: vtable ptr
    //   +0x04: CacaoClient* mCacaoClient (sp<CacaoClient>)
    // 靜態成員（由 wrapper 透過 dlsym 存取）:
    //   mService: sp<ICacaoService>   (全域 singleton)
    //   mServicePid: int              (服務 PID，用於 isSameProcess 判斷)
    //   mServiceLock: pthread_mutex_t  (保護 mService 初始化)

    class Cacao
    {
    public:
        // ── 巢狀類別前向宣告 ──
        class CacaoClient;
        class CacaoProcessCallback;

        Cacao();
        virtual ~Cacao();

        // ── 靜態 singleton 狀態（由 wrapper libcacao_client.so 透過 dlsym 存取）
        static sp<ICacaoService> mService;
        static int mServicePid;
        static pthread_mutex_t mServiceLock;

        // ── 靜態方法 ──
        static void getService(); // 查找 cacao 服務並初始化 mService
        static Cacao *create();   // 建立 Cacao 並 connect

        // ── 實例方法（委派給 mCacaoClient） ──
        // 注意：以下方法在原始 Sony libcacao_client.so 中為 virtual，vtable 順序必須保持一致
        // 原始 _ZTVN7android5CacaoE vtable 佈局（11 個 vfunc）：
        //   [0] ~Cacao (complete)  [1] ~Cacao (deleting)
        //   [2] subscribeEvents  [3] start  [4] stop  [5] setConfig
        //   [6] getConfig  [7] process  [8] processAsync  [9] processAsyncWithBinder  [10] cancel
        int connect();
        void disconnect();
        virtual void subscribeEvents(cacao::ICBProcessEventListener *listener, void *cookie);
        virtual int start(const cacao::ProcessModeBase *mode);
        virtual int stop();
        virtual int setConfig(const cacao::ProcessConfigBase *cfg);
        virtual int getConfig(cacao::ProcessConfigBase *cfg);
        virtual void process(const cacao::ProcessParamBase *param,
                             cacao::ICBProcessResultListener *listener,
                             cacao::ProcessResultBase *result);
        virtual void processAsync(const cacao::ProcessParamBase *param,
                                  android::Vector<cacao::ImageBuf *> *imageBufs,
                                  cacao::ICBProcessResultListener *listener,
                                  cacao::ProcessResultBase *result);
        virtual void processAsyncWithBinder(const cacao::ProcessParamBase *param,
                                            const sp<IBinder> &binder,
                                            cacao::ICBProcessResultListener *listener,
                                            cacao::ProcessResultBase *result);
        virtual int cancel(void *cookie);
        static int getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                           cacao::Caps *caps);
        int getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                    const sp<IMemory> &mem,
                    cacao::ISerialize::SerializedData sd);
        int getConfig(int configId, const sp<IMemory> &mem,
                      cacao::ISerialize::SerializedData sd);
        int setConfig(int configId, const sp<IMemory> &mem,
                      cacao::ISerialize::SerializedData sd);
        static int getJpegBufferSize(cacao::ImageSize size);
        bool isSameProcess();

        // ── 欄位 ── (+0x04)
        sp<CacaoClient> mCacaoClient; // sp<> 管理，避免 linkToDeath sp<> 對決型後造成雙重析構

        // ── CacaoClient ──
        // 繼承順序決定 vtable 偏移：
        //   +0x00: ICacaoClient (primary)
        //   +0x14: ICacaoGateway（生成 _ZThn20_* thunks）
        class CacaoClient
            : public ICacaoClient,
              public ICacaoGateway
        {
        public:
            CacaoClient();
            virtual ~CacaoClient();

            // ── ICacaoClient 回呼（從 ICacaoClient） ──
            virtual void notifyEvent(int eventType,
                                     const sp<IMemory> &mem,
                                     cacao::ISerialize::SerializedData sd) override;

            // ── 非虛擬方法 ──
            int connect(const sp<ICacaoService> &svc);
            void disconnect();

            // ── ICacaoGateway override（生成次要 vtable thunks） ──
            virtual int subscribeEvents(cacao::ICBProcessEventListener *listener, void *cookie) override;
            virtual int start(const cacao::ProcessModeBase *mode) override;
            virtual int stop() override;
            virtual int setConfig(const cacao::ProcessConfigBase *cfg) override;
            virtual int getConfig(cacao::ProcessConfigBase *cfg) override;
            virtual int process(const cacao::ProcessParamBase *param,
                                cacao::ICBProcessResultListener *listener,
                                cacao::ProcessResultBase *result) override;
            virtual int processAsync(const cacao::ProcessParamBase *param,
                                     android::Vector<cacao::ImageBuf *> *imageBufs,
                                     cacao::ICBProcessResultListener *listener,
                                     cacao::ProcessResultBase *result) override;
            virtual int processAsyncWithBinder(const cacao::ProcessParamBase *param,
                                               const sp<IBinder> &binder,
                                               cacao::ICBProcessResultListener *listener,
                                               cacao::ProcessResultBase *result) override;
            virtual int cancel(void *cookie) override;

            // ── 記憶體管理 ──
            static sp<IMemory> allocMemory(size_t size);
            void freeMemory(sp<IMemory> &mem);
            void makeSerializedData(const sp<IMemory> &mem,
                                    cacao::ISerialize::SerializedData *sd);

            // +0x18: sp<ICacao> mCacao
            sp<ICacao> mCacao;
            // +0x1c: pthread_mutex_t mLock
            pthread_mutex_t mLock;
            // +0x20: ICBProcessEventListener* mListener
            cacao::ICBProcessEventListener *mListener;
            // +0x24: void* mCookie
            void *mCookie;
        };

        // ── CacaoProcessCallback ──
        // 包裝 ICBProcessResultListener，透過 Binder 轉送進度/結果
        // 實作 ICacaoProcessCallback
        // sizeof ≈ 0x1c (vtable, BBinder, mListener@0x10, mResult@0x14, RefBase@0x18)
        class CacaoProcessCallback : public ICacaoProcessCallback
        {
        public:
            CacaoProcessCallback(cacao::ICBProcessResultListener *listener,
                                 cacao::ProcessResultBase *result);
            virtual ~CacaoProcessCallback();

            // ICacaoProcessCallback 回呼（從 server 接收進度/結果通知）
            virtual void notifyProgress(const sp<IMemory> &mem,
                                        cacao::ISerialize::SerializedData sd) override;
            virtual void notifyResult(const sp<IMemory> &mem,
                                      cacao::ISerialize::SerializedData sd,
                                      android::Vector<cacao::ImageBuf *> *bufs) override;

            cacao::ICBProcessResultListener *mListener; // +0x10
            cacao::ProcessResultBase *mResult;          // +0x14
        };
    };

} // namespace android