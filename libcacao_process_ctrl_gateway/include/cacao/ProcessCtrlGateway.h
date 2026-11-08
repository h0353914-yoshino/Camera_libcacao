// ProcessCtrlGateway.h — cacao::ProcessCtrlGateway 主類別宣告
// 依照 libcacao_process_ctrl_gateway.so Ghidra 反編譯結果重建
// 包含 Callback 內嵌類別 (ICacaoCallback HIDL 實作)

#pragma once
#include <cacao/ProcessCtrlGatewayBase.h>
#include <vendor/somc/hardware/camera/cacao/3.0/ICacao.h>
#include <vendor/somc/hardware/camera/cacao/3.0/ICacaoCallback.h>
#include <vendor/somc/hardware/camera/cacao/3.1/ICacao.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>
#include <hidl/HidlSupport.h>
#include <binder/IBinder.h>
#include <system/window.h>

namespace cacao
{

    using android::sp;
    using android::hardware::hidl_handle;
    using android::hardware::hidl_string;
    using android::hardware::hidl_vec;
    namespace V30 = vendor::somc::hardware::camera::cacao::V3_0;
    namespace V31 = vendor::somc::hardware::camera::cacao::V3_1;

    /* 處理型別枚舉（cacao::ProcessType）—就算子類可報 */
    enum ProcessType : uint32_t
    {
        PROCESS_STILL = 0,
        PROCESS_BURST = 1,
        PROCESS_VIDEO_CAP = 2,
        PROCESS_PREPARE_VID = 3,
        PROCESS_TYPE_4 = 4,
        PROCESS_TYPE_5 = 5,
        PROCESS_TYPE_6 = 6,
        PROCESS_TYPE_7 = 7,
        PROCESS_TYPE_8 = 8,
        PROCESS_TYPE_9 = 9,
    };

    /* ─────────────────────────────────────────────────────────── */
    /* ResultMsg — PAL 訊息的 payload（progress/result 回調用）
     *  從 Ghidra 反編譯確認: onHandleResultInternal/onHandleProgressInternal
     *  接受 ResultMsg* 而非 PAL_Msg_t*，以匹配原始 .so symbol
     */
    /* ProcessCtrlVideoRecParam 已在 ProcessCtrlGatewayBase.h 中定義 */

    /* ─────────────────────────────────────────────────────────── */
    /* ProcessCtrlGateway
     *
     * 繼承 ProcessCtrlGatewayBase，新增欄位（從 Gateway_ctor 取得）:
     *  [...base fields up to +0x2f0...]
     *  +0x2f0  int mInitFlag  (from base)
     *  +0x2f4  sp<V30::ICacao>  mService       (V3.0)
     *  +0x2f8  sp<V31::ICacao>  mServiceV31    (V3.1, nullable)
     *  +0x2fc  PtrLockList<ResultItem>  mAsyncResults (for passRequest)
     *  +0x300  Mutex  mAsyncMutex
     *  +0x388  ...
     *  +0x398  PtrLockList<ResultItem>  mPrepareVideoResults
     *  +0x39c  Mutex  ...
     *  +0x424  ptr
     *  +0x434  Array<ANativeWindowBuffer*>  mBuffers
     *  +0x444  PtrLockList<ResultItem>  mFreeSwResults
     *  +0x448  Mutex  ...
     *  +0x4d0  ptr
     *  +0x4e0  PtrLockList<ResultItem>  mFreeVideoResults
     *  +0x4e4  Mutex
     *  +0x56c  ptr
     *  +0x570–0x5b4: sp<>/uint64 fields
     *  +0x57c  ANativeWindow* mSurface
     *  +0x5bc  uint32_t  mVideoMaxDur  (0x78)
     *  +0x5c0  uint32_t  mBufferCount  (10)
     *  +0x5c4  uint32_t  mBurstCount   (0x1e)
     *  +0x5d0  Request*  mVideoReq
     *  +0x5d4  Request*  mPrepareVideoReq
     *  +0x5dc  uint32_t  mVideoState
     *  +0x5e0  ptr
     *  +0x618  uint32_t
     *  +0x628  uint32_t
     *  +0x630  ptr
     *  +0x658  uint32_t (init=0)
     *  +0x65c  uint32_t (0x49742400 = session cookie)
     *  +0x660  uint32_t mConfiguredFlag
     */
    class ProcessCtrlGateway : public ProcessCtrlGatewayBase
    {
    public:
        /* ── 空的虛擬子物件（只有 vtable 指針，無資料成員）─────── */
        /* 對應參考 binary 中 Caps / SlowMotionLogger / FrameIds 內嵌類別 */
        class Caps
        {
        public:
            virtual ~Caps() = default;
        };
        class SlowMotionLogger
        {
        public:
            virtual ~SlowMotionLogger() = default;
        };
        class FrameIds
        {
        public:
            virtual ~FrameIds() = default;
        };

        /* ── ResultItem 內嵌類別（匹配原始 .so: ProcessCtrlGateway::ResultItem）── */
        class ResultItem
        {
        public:
            ResultItem();
            virtual ~ResultItem();
            void deinit();

        private:
            char _data[0x12c];
            friend class ProcessCtrlGateway;
        };

        /* ── ResultMsg 內嵌類別（匹配原始 .so: ProcessCtrlGateway::ResultMsg）── */
        struct ResultMsg
        {
            uint32_t msg_id;       // +0x00
            uint32_t _reserved[5]; // +0x04~0x17
            char resultData[0x48]; // +0x18 (V30::Result payload)
            /* resultData 佈局:
             *  [0x00] requestId (uint32_t) — pop/get key
             *  [0x08] err       (PAL_Err_t)
             *  [0x0C] progress  (int32_t)
             */
        };

        /* ── Callback 內嵌類別 ─────────────────────────────── */
        class Callback : public V30::ICacaoCallback
        {
        public:
            explicit Callback(ProcessCtrlGateway *parent);
            virtual ~Callback();

            /* ICacaoCallback 實作 */
            android::hardware::Return<void>
            handleResult(const V30::Result &res) override;
            android::hardware::Return<void>
            handleProgress(const V30::Result &res) override;
            android::hardware::Return<void>
            handleEvent(const V30::Event &ev) override;

        private:
            /* 原始 so 只額外保存 parent 指標；RefBase 由 HIDL IBase 鏈提供。 */
            ProcessCtrlGateway *mParent;
        };

        /* ── 建構/解構 ─────────────────────────────────────── */
        ProcessCtrlGateway();
        virtual ~ProcessCtrlGateway();

        /* ── 覆寫 GatewayBase 虛擬鉤子 ─────────────────────── */
        int onInit() override;
        int onDeinit() override;
        int onStart(const ProcessModeBase *mode) override;
        int onStop(bool isCancelAll) override;
        int onStopRequest() override;
        void onProcess(Request *req) override;
        void onHandleMsg(PAL_Msg_t *msg) override;
        int setConfig(const ProcessConfigBase *cfg) override;
        int getConfig(ProcessConfigBase *cfg) override;
        Request *onCreateRequest(const ProcessParamBase *,
                                 android::Vector<ImageBuf *> *,
                                 ICBProcessResultListener *,
                                 ProcessResultBase *,
                                 uint8_t, const PAL_Msg_t *) override;

        void printStackTrace();
        const char *getClassName() const override { return "ProcessCtrlGateway"; }

    private:
        /* ── 內部處理方法 ──────────────────────────────────── */
        int processInternal(ProcessType processType);
        int passRequest(Request *req);
        int prepareVideoRec(Request *req);
        int startVideoRec(Request *req);
        int stopVideoRec(Request *req);
        int triggerSuperSlow(Request *req);
        void handleFinishCamera();
        void popLast();
        int deinit_device();
        void handleVideoCapture(ResultItem *item);
        void tryVideoCaptureRequest();
        void returnBufferToSurface(ResultItem *item, uint8_t flags);
        int createNativeWindow(ProcessCtrlVideoRecParam *param);
        void deleteNativeWindow();
        int createFreeSwResultList();
        int processAsyncInternal(ProcessType processType);
        int copyProcessCtrlParam(const Request *req, V30::Param *out);
        ResultItem *createResultItemFromBuffer();
        int copyResultItem(ResultItem *dst, ResultItem *src);
        /* 匹配 REF: PtrLockList pop/get 比較器函式 */
        static bool matchResult(ResultItem *item, void *data);     // 對應 REF matchResult
        static bool matchOldFrameId(ResultItem *item, void *data); // 對應 REF matchOldFrameId
        uint32_t convert(ProcessType pt);
        void getService();
        int setConfig_v30(const ProcessConfigBase *cfg);
        int getConfig_v30(ProcessConfigBase *cfg) const;

        void onHandleProgressInternal(ResultMsg *msg);
        void onHandleResultInternal(ResultMsg *msg);
        void onHandleSuperSlowDoneInternal();

        /* ── 成員欄位（佈局從 ctor/dtor 反編譯取得）─────────── */
        sp<V30::ICacao> mService;    // +0x2f4
        sp<V31::ICacao> mServiceV31; // +0x2f8

        /* ResultItem 追蹤清單
         * 各 PtrLockList = 0x9c bytes (vtable+Mutex+PtrList)
         * mAsyncResults:  +0x2fc → +0x397
         * mPrepareResults: +0x398 → +0x433
         * mBuffers:        +0x434 → +0x443 (Array=0x10 bytes)
         * mFreeSwResults:  +0x444 → +0x4df
         * mFreeVideoResults: +0x4e0 → +0x57b */
        cacao_pal::PtrLockList<ResultItem> mAsyncResults;     // +0x2fc
        cacao_pal::PtrLockList<ResultItem> mPrepareResults;   // +0x398
        cacao_pal::Array<ANativeWindowBuffer *> mBuffers;     // +0x434
        cacao_pal::PtrLockList<ResultItem> mFreeSwResults;    // +0x444
        cacao_pal::PtrLockList<ResultItem> mFreeVideoResults; // +0x4e0

        /* Video 狀態欄位（+0x57c 起）*/
        ANativeWindow *mSurface; // +0x57c
        char _pad1[0x38];        // +0x580..+0x5b7 (各種 video 欄位)
        Caps mCaps;              // +0x5b8 (vtable only, 4B)

        uint32_t mVideoMaxDur; // +0x5bc = 0x78
        uint32_t mBufferCount; // +0x5c0 = 10
        uint32_t mBurstCount;  // +0x5c4 = 30

        char _padVideo[8]; // +0x5c8..+0x5cf

        Request *mVideoReq;        // +0x5d0
        Request *mPrepareVideoReq; // +0x5d4
        char _padVidState[4];      // +0x5d8..+0x5db
        uint32_t mVideoState;      // +0x5dc

        /* _padSession 拆分為子物件+padding（匹配 reference ctor） */
        SlowMotionLogger mSlowMotLogger1; // +0x5e0 (vtable only, 4B)
        char _padSlowMot1[0x14];          // +0x5e4..+0x5f7
        SlowMotionLogger mSlowMotLogger2; // +0x5f8 (vtable only, 4B)
        char _padSlowMot2[0x34];          // +0x5fc..+0x62f
        FrameIds mFrameIds;               // +0x630 (vtable only, 4B)
        char _padFrameIds[0x28];          // +0x634..+0x65b

        uint32_t mSessionId;         // +0x65c = 0x49742400
        uint32_t mConfiguredFlag;    // +0x660
        uint32_t mSuperSlowFrameCnt; // +0x664 super-slow counter (REF: static var in handleVideoCapture case2)
    };

    /* ISerialize 已在 ProcessCtrlGatewayBase.h 中定義 */

    /* ─────────────────────────────────────────────────────────── */
    /* ProcessCtrlGatewayFactory
     *
     * 工廠類別，依照原始 .so 中 6 個 Factory 方法的反編譯邏輯實作。
     * 各 create* 方法使用外部 serialized data 建構對應物件。
     */
    class ProcessCtrlGatewayFactory
    {
    public:
        /* create() — 分配並初始化 ProcessCtrlGateway，回傳 ICacaoGateway* */
        static ICacaoGateway *create();

        /* createMode/Param/Result/Config — 依序列化資料建構對應物件 */
        static ProcessModeBase *createMode(ISerialize::SerializedData *data);
        static ProcessParamBase *createParam(ISerialize::SerializedData *data);
        static ProcessParamBase *createParam(ANativeWindow *window,
                                             ISerialize::SerializedData *data);
        static ProcessResultBase *createResult(ISerialize::SerializedData *data);
        static ProcessConfigBase *createConfig(unsigned int configId,
                                               ISerialize::SerializedData *data);
    };

} // namespace cacao
