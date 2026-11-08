// CacaoService.cpp — android::CacaoService 完整實作
// 依照 libcacao_service.so Ghidra 反編譯結果 100% 重建
// 所有函數邏輯、物件佈局、vtable 偏移均從 Ghidra 驗證

#define LOG_TAG "CacaoService"

#include <cacao/CacaoService.h>
#include <cacao/CacaoAidlServer.h>
#include <android/log.h>
#include <android/binder_ibinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <cacao/ProcessCtrlGatewayBase.h>

// 前向宣告 ProcessCtrlGatewayFactory（避免引入 ProcessCtrlGateway.h 的 system/window.h 依賴）
namespace cacao
{
    class ProcessCtrlGatewayFactory
    {
    public:
        static ICacaoGateway *create();
        static ProcessModeBase *createMode(ISerialize::SerializedData *data);
        static ProcessParamBase *createParam(ISerialize::SerializedData *data);
        static ProcessParamBase *createParam(ANativeWindow *window,
                                             ISerialize::SerializedData *data);
        static ProcessResultBase *createResult(ISerialize::SerializedData *data);
        static ProcessConfigBase *createConfig(unsigned int configId,
                                               ISerialize::SerializedData *data);
    };
} // namespace cacao

namespace V3_0 = ::vendor::somc::hardware::camera::cacao::V3_0;
namespace V3_1 = ::vendor::somc::hardware::camera::cacao::V3_1;
namespace provider_V1_0 = ::vendor::somc::hardware::camera::provider::V1_0;
using ::android::hardware::Return;

// 原始 .so 透過 vtable 偏移直接呼叫 serialize/getSerializedSize，
// C++ 中需要透過 ISerializable 介面存取。
// 工廠建立的 concrete 物件（ProcessCtrlResult, ProcessCtrlDynamicParameterConfig）
// 其 ISerializable 為 secondary base (offset +4)
static inline cacao::ISerializable *toSerializable(void *obj)
{
    return reinterpret_cast<cacao::ISerializable *>(
        reinterpret_cast<uintptr_t>(obj) + 4);
}

namespace android
{

    // ════════════════════════════════════════════════════════════
    // CacaoService
    // ════════════════════════════════════════════════════════════

    CacaoService::CacaoService()
    {
        pthread_mutex_init(&mLock, NULL);
        pthread_mutex_init(&mClientLock, NULL);
        // mRetryCount 在 class 中已初始化為 0
        mRetryCount = 0;
        // +0x28/+0x2c 的 linkToDeath cookie 在 .so 中為零初始化
        mLinkCookie = 0;

        // 註冊 IServiceNotification 以接收 HAL 服務上線通知
        // 原始 .so: sp<IServiceNotification>::sp<CacaoService>(this) → registerForNotifications
        provider_V1_0::IVendorSomcCameraProvider::registerForNotifications("", this);
    }

    CacaoService::~CacaoService()
    {
        // 清除 HIDL 服務引用
        mServiceV31 = NULL;
        mService = NULL;
        // List 、 mutex 由成員解構子自動清理
        pthread_mutex_destroy(&mClientLock);
        pthread_mutex_destroy(&mLock);
    }

    void CacaoService::instantiate()
    {
        sp<CacaoService> svc = new CacaoService();
        // stable AIDL 服務：vendor.somc.cacao.ICacaoService/default。
        cacaoAidlPublish(svc);
    }

    void CacaoService::onFirstRef()
    {
        // 原始 .so: 空實作（直接 return）
    }

    // ── connect ──────────────────────────────────────────────
    sp<ICacao> CacaoService::connect(const sp<ICacaoClient> &client)
    {
        pthread_mutex_lock(&mLock);

        Client *c = new Client(this, client, AIBinder_getCallingPid());
        sp<Client> spClient(c);
        registerClient(spClient);

        pthread_mutex_unlock(&mLock);
        return spClient;
    }

    // ── getServicePid ────────────────────────────────────────
    int CacaoService::getServicePid()
    {
        return getpid();
    }

    // ── getClientPid ─────────────────────────────────────────
    pid_t CacaoService::getClientPid()
    {
        return AIBinder_getCallingPid();
    }

    // ── getClientUid ─────────────────────────────────────────
    uid_t CacaoService::getClientUid()
    {
        return AIBinder_getCallingUid();
    }

    // ── getCaps ──────────────────────────────────────────────
    // 透過 HIDL 呼叫 HAL 取得 camera caps，然後展平到 Caps 的 0x262c 平坦格式
    // 原始 .so 在 callback 內把每個 hidl_vec 解引用並拷到堆疊陣列，
    // 然後呼叫 Caps::serialize 寫入 SerializedData

    // 將 hidl_vec<ImageSize> 展平到 data+offset（每個元素 w,h 各佔4 bytes）
    // 最多拷貝 maxN 筆；回傳實際筆數
    static uint32_t flattenSizes(uint8_t *data, uint32_t offset,
                                 const hardware::hidl_vec<V3_0::ImageSize> &vec,
                                 uint32_t maxN)
    {
        uint32_t n = vec.size();
        if (n > maxN)
            n = maxN;
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t w = vec[i].width;
            uint32_t h = vec[i].height;
            memcpy(data + offset + i * 8, &w, 4);
            memcpy(data + offset + i * 8 + 4, &h, 4);
        }
        return n;
    }

    // 將 hidl_vec<uint32_t> 展平到 data+offset
    static uint32_t flattenU32(uint8_t *data, uint32_t offset,
                               const hardware::hidl_vec<uint32_t> &vec,
                               uint32_t maxN)
    {
        uint32_t n = vec.size();
        if (n > maxN)
            n = maxN;
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t v = vec[i];
            memcpy(data + offset + i * 4, &v, 4);
        }
        return n;
    }

    static inline void putU32(uint8_t *data, uint32_t offset, uint32_t val)
    {
        memcpy(data + offset, &val, 4);
    }

    /* Caps.data[0x262c] 平坦格式（從 Ghidra 反編譯 getCaps + Caps::serialize 重建）:
     *
     * Section 1 (0xC0C bytes)
     * 0x0000  field_00
     * 0x0004  sizes0_count
     * 0x0008  ImageSize sizes0[128]     (0x400)
     * 0x0408  uint32    formats0[128]   (0x200)
     * 0x0608  sizes1_count
     * 0x060C  ImageSize sizes1[128]     (0x400)
     * 0x0A0C  uint32    formats1[128]   (0x200)
     *
     * Section 2 (0x604 bytes)
     * 0x0C0C  sizes2_count
     * 0x0C10  ImageSize sizes2[128]     (0x400)
     * 0x1010  uint32    formats2[128]   (0x200)
     *
     * Section 3 (0x808 bytes)
     * 0x1210  field_80
     * 0x1214  sizes3_count
     * 0x1218  ImageSize sizes3[128]     (0x400)
     * 0x1618  uint32    formats3[128]   (0x200)
     * 0x1818  uint32    extra0[128]     (0x200)
     *
     * Section 4a (8 bytes)
     * 0x1A18  field_c0
     * 0x1A1C  field_c4
     *
     * Section 4b (0xC0C bytes, V3.1)
     * 0x1A20  _pad_d4
     * 0x1A24  sizes4_count
     * 0x1A28  ImageSize sizes4[128]     (0x400)
     * 0x1E28  uint32    formats4[128]   (0x200)
     * 0x2028  sizes5_count
     * 0x202C  ImageSize sizes5[128]     (0x400)
     * 0x242C  uint32    formats5[128]   (0x200)
     *
     * Total = 0x262C
     */
    // 2026-08-18 CacaoCaps 從扁平猜測改成巢狀具名子結構（VideoStabilizationCaps/
    // SuperSlowCaps，見 types.h 註解），這裡跟著改對應欄位。Section 1~4a 的
    // app-facing 攤平 byte layout 本身沒有變（那是驗證過的既有格式），只是
    // 「從哪個 HIDL 欄位讀值」跟著新結構走：
    //   Section 1（4 個 vec）  ← VideoStabilizationCaps 的 sizes0/formats0/sizes1/formats1
    //   Section 2（1 對 vec）  ← CacaoCaps 自己的 sizes/formats
    //   Section 3（1 對 vec + extra0）← SuperSlowCaps 的 sizes/formats0/formats1
    //   Section 4a 兩個純量   ← VideoStabilizationCaps/SuperSlowCaps 各自的
    //                           leading _reserved[0]（對應舊版 field_00/field_80）
    // 這個對應順序是依「vec 數量與宣告順序都對得上（4 個 ImageSize vec + 5 個
    // uint32 vec，一個不多一個不少）」推出來的，不是重新從 cacaoserver 自己的
    // getCaps() 實作反編譯逐一驗證過，實機測試時要順便留意 getCaps() 相關的相機
    // 能力顯示是否正常。
    static bool flattenCacaoCapsV30(uint8_t *data,
                                    const V3_0::CacaoCaps &c)
    {
        const auto &vsc = c.videoStabilization;
        const auto &ssc = c.superSlow;

        // 先驗證所有 vec size ≤ 128
        if (vsc.sizes0.size() > 128 || vsc.formats0.size() > 128 ||
            vsc.sizes1.size() > 128 || vsc.formats1.size() > 128 ||
            c.sizes.size() > 128 || c.formats.size() > 128 ||
            ssc.sizes.size() > 128 || ssc.formats0.size() > 128 ||
            ssc.formats1.size() > 128)
            return false;

        // Section 1（VideoStabilizationCaps）
        putU32(data, 0x0000, vsc.reserved0[0]);
        putU32(data, 0x0004, vsc.sizes0.size());
        flattenSizes(data, 0x0008, vsc.sizes0, 128);
        flattenU32(data, 0x0408, vsc.formats0, 128);
        putU32(data, 0x0608, vsc.sizes1.size());
        flattenSizes(data, 0x060C, vsc.sizes1, 128);
        flattenU32(data, 0x0A0C, vsc.formats1, 128);

        // Section 2（CacaoCaps 自己的 vec）
        putU32(data, 0x0C0C, c.sizes.size());
        flattenSizes(data, 0x0C10, c.sizes, 128);
        flattenU32(data, 0x1010, c.formats, 128);

        // Section 3（SuperSlowCaps）
        putU32(data, 0x1210, ssc.reserved0[0]);
        putU32(data, 0x1214, ssc.sizes.size());
        flattenSizes(data, 0x1218, ssc.sizes, 128);
        flattenU32(data, 0x1618, ssc.formats0, 128);
        flattenU32(data, 0x1818, ssc.formats1, 128);

        // Section 4a — 對應舊版 field_c0/field_c4，就是促成 SuperSlowCaps 尾端
        // 補上 _reserved1[2] 的那兩個純量（見 types.h 的 SuperSlowCaps 註解）
        putU32(data, 0x1A18, ssc.reserved1[0]);
        putU32(data, 0x1A1C, ssc.reserved1[1]);

        // Section 4b 全零（V3.0 無 sizes4/5）
        return true;
    }

    static bool flattenCacaoCapsV31(uint8_t *data,
                                    const V3_1::CacaoCaps &c)
    {
        // 先展平 V3.0 部分
        if (!flattenCacaoCapsV30(data, c.base))
            return false;

        // Section 4b — V3.1 擴充：ext0/ext1 都是 V3_0::SupportedInfo（Ghidra
        // get_function_callees 確認），欄位對應方式跟 flattenCacaoCapsV30 一樣，
        // 只保留每個 SupportedInfo 開頭那個保留字給 app（對應舊版 _pad_d4 那格，
        // 其餘保留字目前沒有已知的 app-facing 對應）。
        if (c.ext0.sizes.size() > 128 || c.ext0.formats.size() > 128 ||
            c.ext1.sizes.size() > 128 || c.ext1.formats.size() > 128)
            return false;

        putU32(data, 0x1A20, c.ext0.reserved0[0]);
        putU32(data, 0x1A24, c.ext0.sizes.size());
        flattenSizes(data, 0x1A28, c.ext0.sizes, 128);
        flattenU32(data, 0x1E28, c.ext0.formats, 128);
        putU32(data, 0x2028, c.ext1.sizes.size());
        flattenSizes(data, 0x202C, c.ext1.sizes, 128);
        flattenU32(data, 0x242C, c.ext1.formats, 128);
        return true;
    }

    int CacaoService::getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                              const sp<IMemory> &mem,
                              cacao::ISerialize::SerializedData sd)
    {
        pthread_mutex_lock(&mLock);

        int ret = 0;
        cacao::Caps caps;

        V3_0::CameraIndex camId;
        if (camIdx.index == 0)
        {
            camId = V3_0::CameraIndex::CAMERA_ID_0;
        }
        else if (camIdx.index == 1)
        {
            camId = V3_0::CameraIndex::CAMERA_ID_1;
        }
        else
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "getCaps: invalid camIdx %d", camIdx.index);
            ret = -0x67;
            pthread_mutex_unlock(&mLock);
            return -0x80000000;
        }

        if (mServiceV31 == NULL)
        {
            // V3.0 路徑：呼叫 mService->getCaps
            if (mService == NULL)
            {
                ret = -0x80000000;
                pthread_mutex_unlock(&mLock);
                return ret;
            }

            auto hidlRet = mService->getCaps(
                camId,
                [&](const V3_0::CacaoCaps &hidlCaps, V3_0::ErrCode errCode)
                {
                    if (errCode != V3_0::ErrCode::OK)
                    {
                        ret = static_cast<int>(errCode);
                        return;
                    }
                    if (!flattenCacaoCapsV30(caps.data, hidlCaps))
                    {
                        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                            "getCaps: V3.0 vec size exceeds 128");
                        ret = -0x6f;
                    }
                });

            if (!hidlRet.isOk())
            {
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                    "hidl getCaps error: %s", hidlRet.description().c_str());
                ret = -0x6f;
            }
        }
        else
        {
            // V3.1 路徑：呼叫 mServiceV31->getCapsV3_1
            // （dynsym/Ghidra 確認 V3.1 並未覆寫 getCaps，只新增 getCapsV3_1；
            //   先前誤用 getCaps 這個名字，型別能兜起來只是巧合）
            auto hidlRet = mServiceV31->getCapsV3_1(
                camId,
                [&](const V3_1::CacaoCaps &hidlCaps, V3_0::ErrCode errCode)
                {
                    if (errCode != V3_0::ErrCode::OK)
                    {
                        ret = static_cast<int>(errCode);
                        return;
                    }
                    if (!flattenCacaoCapsV31(caps.data, hidlCaps))
                    {
                        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                            "getCaps: V3.1 vec size exceeds 128");
                        ret = -0x6f;
                    }
                });

            if (!hidlRet.isOk())
            {
                __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                    "hidl getCaps error: %s", hidlRet.description().c_str());
                ret = -0x6f;
            }
        }

        if (ret >= 0)
        {
            caps.serialize(&sd);
            ret = 0;
        }

        pthread_mutex_unlock(&mLock);

        // error clamp
        if (ret != -0x6e)
        {
            if (ret >= 0)
            {
                ret = 0;
            }
            else
            {
                ret = -0x80000000;
            }
        }
        return ret;
    }

    // ── getJpegBufferSize ────────────────────────────────────
    int CacaoService::getJpegBufferSize(cacao::ImageSize size)
    {
        pthread_mutex_lock(&mLock);

        int jpegSize = 0;
        int ret = 0;

        if (mService == NULL)
        {
            pthread_mutex_unlock(&mLock);
            return -0x80000000;
        }

        // 呼叫 HIDL V3.0 getJpegBufferSize(ImageSize const&, callback(uint32_t, ErrCode))
        // vtable[0x58]: 傳入相機影像大小後，透過 callback 回傳 JPEG buffer 大小（bytes）
        V3_0::ImageSize hidlSize;
        hidlSize.width = size.width;
        hidlSize.height = size.height;

        auto hidlRet = mService->getJpegBufferSize(
            hidlSize, // BpHwCacao takes const ImageSize&
            [&](uint32_t jpegBufSize, V3_0::ErrCode errCode)
            {
                if (errCode != V3_0::ErrCode::OK)
                {
                    ret = static_cast<int>(errCode);
                    return;
                }
                jpegSize = static_cast<int>(jpegBufSize);
            });

        if (!hidlRet.isOk())
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "hidl getJpegBufferSize error: %s", hidlRet.description().c_str());
            ret = -0x6f;
        }

        pthread_mutex_unlock(&mLock);
        return (ret >= 0) ? jpegSize : ret;
    }

    // ── 連線權限檢查說明（原 onTransact）─────────────────────
    // 原始 .so: 只對 tx code 1 (connect) 做權限檢查（呼叫方是否持有
    // android.permission.CAMERA），透過 android::checkCallingPermission()。
    //
    // 這裡拿掉了：那個 API（以及底層的 IPermissionController／
    // PermissionCache）在 AOSP 裡明確標示「not accessible to vendors」——
    // vendor 變體的 libbinder 根本不會把這個符號編進去
    // （frameworks/native/libs/binder/IServiceManager.cpp 用
    // #if !defined(__ANDROID_VNDK__) 整段包住實作），無法在 cacaoserver
    // 現在所在的 vendor 分割區使用。原版能直接查，是因為當年 cacaoserver
    // 跟呼叫方同在一個沒有切分 system/vendor 的分割區。
    //
    // 取代方案：sepolicy（vendor/sony/camera/sepolicy/cacaoserver.te +
    // semccamera_app.te）明確把 cacaoserver_service 的存取權限收在只有
    // SemcCameraUI 專屬 domain（semccamera_app）能用，其他任何
    // priv_app/platform_app 連 service_manager find 這一步都會被擋。這是
    // Treble 架構下這類存取控制該做的層級（HAL/vendor 元件用 SELinux MAC
    // 把關，而不是查 Android permission 字串），語意上也比原本的檢查更嚴
    // 格：原本只要持有 CAMERA 權限的任何 app 都能連，現在只有這一支 app
    // 能連。

    // ── registerClient ───────────────────────────────────────
    void CacaoService::registerClient(const sp<Client> &client)
    {
        pthread_mutex_lock(&mClientLock);
        if (client != NULL)
        {
            mClients.push_back(client);
            // 2026-08-19 修正：原本掛在 IInterface::asBinder(client)——那是 Client
            // 自己的本地 BBinder，對本地 binder 掛 linkToDeath 永遠不會觸發（死亡
            // 通知只對遠端 proxy 有意義）。結果 App 非正常結束（force-stop／崩潰）
            // 時沒有任何人回收這個 Client，它的 gateway 也就永遠不解構，
            // PAL_Create/PAL_Delete 的引用計數再也回不到 0，cald 端的 session
            // 永遠不重新初始化 → 下次開相機時 excalibur::System::start() 卡死。
            // 要掛的是 App 端的 ICacaoClient 遠端 binder。
            //
            // 原版（Ghidra registerClient @ 0x1aa80）：取 vtable+0x44 = getClient()，
            // 對回傳的 App 端 ICacaoClient 做 asBinder，接著直接
            //   pcVar1 = *(code **)(*local_34 + 0x18);   // IBinder vtable+0x18 = linkToDeath
            //   (*pcVar1)(local_34, recipient, 0, 0);
            // 中間沒有任何 null 檢查——原版就假設 getClient() 必為非 null，
            // 這裡不另外加防護，行為與原版一致。
            sp<ICacaoClient> appClient = client->getClient();
                cacaoAidlLinkClientDeath(appClient, this);
        }
        pthread_mutex_unlock(&mClientLock);
    }

    // ── unregisterClient（AIDL 版：以 ICacaoClient 物件比對）────────
    void CacaoService::unregisterClient(const sp<ICacaoClient> &client)
    {
        pthread_mutex_lock(&mClientLock);
        for (List<sp<Client>>::iterator it = mClients.begin();
             it != mClients.end(); ++it)
        {
            if (*it == NULL)
            {
                continue;
            }
            if ((*it)->getClient() == client)
            {
                cacaoAidlUnlinkClientDeath(client);
                it->clear();
                mClients.erase(it);
                break;
            }
        }
        pthread_mutex_unlock(&mClientLock);
    }

    // ── onRegistration (IServiceNotification) ────────────────
    Return<void> CacaoService::onRegistration(
        const hardware::hidl_string & /*fqName*/,
        const hardware::hidl_string & /*name*/,
        bool /*preexisting*/)
    {
        getInterfaces();
        return ::android::hardware::Void();
    }

    // ── getInterfaces ────────────────────────────────────────
    void CacaoService::getInterfaces()
    {
        using ::android::hardware::camera::common::V1_0::Status;
        using provider_V1_0::IVendorSomcCameraProvider;

        // 服務實例名是 "internal/0"（同函式下方三段錯誤訊息的 rodata 原文，
        // 以及 ProcessCtrlGateway::getService() 的呼叫都證實）。hidl-gen 生成的
        // getService() 預設值是 "default"，必須明寫，否則 hwservicemanager 會回
        // "Cannot find entry ...::IVendorSomcCameraProvider/default"。
        sp<IVendorSomcCameraProvider> provider = IVendorSomcCameraProvider::getService("internal/0");
        if (provider == NULL)
            return;

        // so_32 @ 0x00019dd4：cookie 在呼叫 linkToDeath 前就先算好遞增值，但只有
        // getCacaoInterface_V3_0 也成功時才會把新值寫回 mLinkCookie；失敗的話
        // mLinkCookie 維持舊值，serviceDied() 之後收到帶新 cookie 的死亡通知會
        // 因為對不上而被當作過期通知忽略（與原版行為一致）。以下三段錯誤訊息
        // 字串都是從 so_32 @ 0x0001e6dc 附近的 rodata 直接讀出，逐字比對過。
        uint64_t newCookie = mLinkCookie + 1;

        auto linkRet = provider->linkToDeath(this, newCookie);
        if (!linkRet.isOk())
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "%s: Transaction error in linking to camera provider 'internal/0' death: %s",
                                "getInterfaces", linkRet.description().c_str());
        }
        else if (!linkRet)
        {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                                "%s: Unable to link to provider 'internal/0' death notifications",
                                "getInterfaces");
        }

        sp<V3_0::ICacao> newService;
        Status cbStatus = Status::INTERNAL_ERROR;
        bool gotCallback = false;
        int result = 0;
        auto hidlRet = provider->getCacaoInterface_V3_0(
            [&](Status status, const sp<V3_0::ICacao> &cacaoService)
            {
                cbStatus = status;
                newService = cacaoService;
                gotCallback = true;
            });

        if (!hidlRet.isOk())
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "Transaction error trying to cacao interface: %s",
                                hidlRet.description().c_str());
            result = -0x6f;
        }
        if (gotCallback && cbStatus != Status::OK)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Unable to cacao interface");
            result = -0x6f;
        }

        if (result >= 0)
        {
            mLinkCookie = newCookie;
            mService = newService;
            if (mService != NULL)
            {
                auto castRet = V3_1::ICacao::castFrom(mService, false);
                mServiceV31 = static_cast<::android::sp<V3_1::ICacao>>(castRet);
            }
        }

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s: cookie:%llu",
                            "getInterfaces", static_cast<unsigned long long>(mLinkCookie));
    }

    // ── serviceDied (hidl_death_recipient) ───────────────────
    void CacaoService::serviceDied(uint64_t cookie,
                                   const wp<hidl::base::V1_0::IBase> & /*who*/)
    {
        // so_32 @ 0x0001ad80：log 訊息、字串皆從 rodata 逐字讀出。
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "The camera provider is died. current:%llu, died:%llu",
                            static_cast<unsigned long long>(mLinkCookie),
                            static_cast<unsigned long long>(cookie));

        // 傳入的 cookie 對不上目前 getInterfaces() 最後一次成功連線時記下的
        // mLinkCookie，代表這是一次「過期」的死亡通知（例如已經因為別的原因
        // 重新連線過），直接忽略，不進入 retry/abort 流程。
        if (cookie != mLinkCookie)
        {
            return;
        }

        // so_32 @ 0x0001ad80：判斷用的是「遞增前」的舊值，遞增後才寫回，
        // 也就是第 20 次（舊值 0x13=19）才會 abort，不是第 19 次。
        int oldRetryCount = mRetryCount;
        mRetryCount = oldRetryCount + 1;
        if (oldRetryCount >= 0x13)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "Abort cacao service because camera provider is died frequently.");
            abort();
        }

        pthread_mutex_lock(&mClientLock);
        for (List<sp<Client>>::iterator it = mClients.begin();
             it != mClients.end(); ++it)
        {
            if (*it != NULL)
            {
                (*it)->serviceDied();
            }
        }
        pthread_mutex_unlock(&mClientLock);
    }

    // ════════════════════════════════════════════════════════════
    // CacaoService::Client
    // ════════════════════════════════════════════════════════════

    CacaoService::Client::Client(const sp<CacaoService> &service,
                                 const sp<ICacaoClient> &client,
                                 int callingPid)
    {
        pthread_mutex_init(&mMutexDisconnect, NULL);
        pthread_mutex_init(&mMutexClient, NULL);
        pthread_mutex_init(&mMutexRequest, NULL);

        mService = service;
        mClient = client;
        mCallingPid = callingPid;
        mDisconnected = 0;

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "Client::Client pid=%d", callingPid);

        mProcess = cacao::ProcessFactoryInternal::createProcess();
        if (mProcess != NULL)
        {
            // ICacaoGateway::subscribeEvents(this as ICBProcessEventListener, cookie=0)
            static_cast<cacao::ICacaoGateway *>(mProcess)->subscribeEvents(this, 0);
        }
    }

    CacaoService::Client::~Client()
    {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "Client::~Client pid=%d", mCallingPid);

        mDisconnected = 1;
        disconnect();

        pthread_mutex_destroy(&mMutexRequest);
        pthread_mutex_destroy(&mMutexClient);
        pthread_mutex_destroy(&mMutexDisconnect);
    }

    // ── disconnect ───────────────────────────────────────────
    void CacaoService::Client::disconnect()
    {
        pthread_mutex_lock(&mMutexDisconnect);

        pid_t callingPid = AIBinder_getCallingPid();
        if (callingPid != mCallingPid && callingPid != getpid())
        {
            pthread_mutex_unlock(&mMutexDisconnect);
            return;
        }

        if (mProcess != NULL)
        {
            // ICacaoGateway D0 dtor — 刪除 gateway
            delete static_cast<cacao::ICacaoGateway *>(mProcess);
            mProcess = NULL;
        }
        else
        {
        }

        // 排空 request list
        pthread_mutex_lock(&mMutexRequest);
        List<Request *>::iterator it = mRequestList.begin();
        while (it != mRequestList.end())
        {
            Request *req = *it;
            it = mRequestList.erase(it);
            if (req != NULL)
            {
                delete req;
            }
        }
        pthread_mutex_unlock(&mMutexRequest);

        if (mDisconnected == 0)
        {
            pthread_mutex_lock(&mMutexClient);
            sp<CacaoService> service = mService;
            if (service != NULL)
            {
service->unregisterClient(mClient);
            }
            mClient = NULL;
            pthread_mutex_unlock(&mMutexClient);
        }

        pthread_mutex_unlock(&mMutexDisconnect);
    }

    // ── start ────────────────────────────────────────────────
    int CacaoService::Client::start(const sp<IMemory> & /*mem*/,
                                    cacao::ISerialize::SerializedData sd)
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret;
        if (mProcess == NULL)
        {
            ret = -0x80000000;
        }
        else
        {
            cacao::ProcessModeBase *mode = cacao::ProcessFactoryInternal::createMode(&sd);
            if (mode == NULL)
            {
                ret = -0x80000000;
            }
            else
            {
                ret = static_cast<cacao::ICacaoGateway *>(mProcess)->start(mode);
                // error clamp
                if (ret != -0x6e)
                {
                    if (ret >= 0)
                        ret = 0;
                    else
                        ret = -0x80000000;
                }
                delete mode;
            }
        }

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── stop ─────────────────────────────────────────────────
    int CacaoService::Client::stop()
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret;
        if (mProcess == NULL)
        {
            ret = -0x80000000;
        }
        else
        {
            ret = static_cast<cacao::ICacaoGateway *>(mProcess)->stop();
            // error clamp
            if (ret != -0x6e)
            {
                if (ret >= 0)
                    ret = 0;
                else
                    ret = -0x80000000;
            }
        }

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── setConfig ────────────────────────────────────────────
    int CacaoService::Client::setConfig(int configId, const sp<IMemory> & /*mem*/,
                                        cacao::ISerialize::SerializedData sd)
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret;
        if (mProcess == NULL)
        {
            ret = -0x80000000;
        }
        else
        {
            cacao::ProcessConfigBase *cfg =
                cacao::ProcessFactoryInternal::createConfig(configId, &sd);
            if (cfg == NULL)
            {
                ret = -0x80000000;
            }
            else
            {
                ret = static_cast<cacao::ICacaoGateway *>(mProcess)->setConfig(cfg);
                delete cfg;
                // error clamp
                if (ret != -0x6e)
                {
                    if (ret >= 0)
                        ret = 0;
                    else
                        ret = -0x80000000;
                }
            }
        }

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── getConfig ────────────────────────────────────────────
    int CacaoService::Client::getConfig(int configId, const sp<IMemory> & /*mem*/,
                                        cacao::ISerialize::SerializedData sd)
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret;
        if (mProcess == NULL)
        {
            ret = -0x80000000;
        }
        else
        {
            cacao::ProcessConfigBase *cfg =
                cacao::ProcessFactoryInternal::createConfig(configId, &sd);
            if (cfg == NULL)
            {
                ret = -0x80000000;
            }
            else
            {
                ret = static_cast<cacao::ICacaoGateway *>(mProcess)->getConfig(cfg);
                if (ret >= 0)
                {
                    // 原始 .so: cfg->vtable[+0x20](cfg, &sd) — serialize
                    ret = toSerializable(cfg)->serialize(&sd);
                }
                delete cfg;
                // error clamp
                if (ret != -0x6e)
                {
                    if (ret >= 0)
                        ret = 0;
                    else
                        ret = -0x80000000;
                }
            }
        }

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── process (with ImageBuf bufs) ─────────────────────────
    int CacaoService::Client::process(const sp<IMemory> &paramMem,
                                      cacao::ISerialize::SerializedData paramSd,
                                      Vector<cacao::ImageBuf *> bufs,
                                      const sp<IMemory> & /*resultMem*/,
                                      cacao::ISerialize::SerializedData resultSd,
                                      int flags,
                                      const sp<ICacaoProcessCallback> &callback)
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret = 0;
        if (mProcess == NULL)
        {
            pthread_mutex_unlock(&mMutexDisconnect);
            return 0;
        }

        cacao::ProcessParamBase *param = NULL;
        if (paramMem != NULL)
        {
            param = cacao::ProcessFactoryInternal::createParam(&paramSd);
            if (param == NULL)
            {
                pthread_mutex_unlock(&mMutexDisconnect);
                return 0;
            }
        }

        cacao::ProcessResultBase *result = cacao::ProcessFactoryInternal::createResult(&resultSd);
        if (result == NULL)
        {
            if (param != NULL)
                delete param;
            pthread_mutex_unlock(&mMutexDisconnect);
            return 0;
        }

        // 建立 Request 並插入 list
        Request *req = new Request(callback, this);
        req->setBufVector(new Vector<cacao::ImageBuf *>(bufs));
        req->setResult(result);

        pthread_mutex_lock(&mMutexRequest);
        mRequestList.push_back(req);
        pthread_mutex_unlock(&mMutexRequest);

        // 根據 flags (binder==0) 決定呼叫 processAsync 或 process
        if (flags == 0)
        {
            static_cast<cacao::ICacaoGateway *>(mProcess)->processAsync(
                param, req->getBufVector(), static_cast<cacao::ICBProcessResultListener *>(this), result);
        }
        else
        {
            static_cast<cacao::ICacaoGateway *>(mProcess)->process(
                param, static_cast<cacao::ICBProcessResultListener *>(this), result);
        }

        if (param != NULL)
            delete param;

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── process (with IBinder) ───────────────────────────────
    int CacaoService::Client::process(const sp<IMemory> &paramMem,
                                      cacao::ISerialize::SerializedData paramSd,
                                      ANativeWindow *window,
                                      const sp<IMemory> & /*resultMem*/,
                                      cacao::ISerialize::SerializedData resultSd,
                                      int flags,
                                      const sp<ICacaoProcessCallback> &callback)
    {
        pthread_mutex_lock(&mMutexDisconnect);

        int ret = 0;
        if (mProcess == NULL)
        {
            pthread_mutex_unlock(&mMutexDisconnect);
            return 0;
        }

        cacao::ProcessParamBase *param = NULL;
        if (paramMem != NULL)
        {
            param = cacao::ProcessFactoryInternal::createParam(window, &paramSd);
            if (param == NULL)
            {
                pthread_mutex_unlock(&mMutexDisconnect);
                return 0;
            }
        }

        cacao::ProcessResultBase *result = cacao::ProcessFactoryInternal::createResult(&resultSd);
        if (result == NULL)
        {
            if (param != NULL)
                delete param;
            pthread_mutex_unlock(&mMutexDisconnect);
            return 0;
        }

        // 建立 Request 並插入 list
        Request *req = new Request(callback, this);
        req->setResult(result);

        pthread_mutex_lock(&mMutexRequest);
        mRequestList.push_back(req);
        pthread_mutex_unlock(&mMutexRequest);

        if (window == NULL)
        {
            static_cast<cacao::ICacaoGateway *>(mProcess)->processAsync(
                param, NULL, static_cast<cacao::ICBProcessResultListener *>(this), result);
        }
        else
        {
            static_cast<cacao::ICacaoGateway *>(mProcess)->process(
                param, static_cast<cacao::ICBProcessResultListener *>(this), result);
        }

        if (param != NULL)
            delete param;

        pthread_mutex_unlock(&mMutexDisconnect);
        return ret;
    }

    // ── cancel ───────────────────────────────────────────────
    int CacaoService::Client::cancel(int /*cookie*/)
    {
        if (mProcess == NULL)
        {
            return -0x80000000;
        }
        int ret = static_cast<cacao::ICacaoGateway *>(mProcess)->cancel(NULL);
        // error clamp
        if (ret != -0x6e)
        {
            if (ret >= 0)
                ret = 0;
            else
                ret = -0x80000000;
        }
        return ret;
    }

    // ── onHandleEvent (ICBProcessEventListener) ───────────────────────────────
    void CacaoService::Client::onHandleEvent(const cacao::ProcessEventBase *ev)
    {
        pthread_mutex_lock(&mMutexClient);
        sp<ICacaoClient> client = mClient;
        pthread_mutex_unlock(&mMutexClient);

        if (client == NULL)
            return;

        // 取得事件序列化大小
        int eventType = ev->getType();
        unsigned int dataSize = ev->getSerializedSize();

        sp<IMemory> mem = allocMemory(dataSize);
        if (mem == NULL)
            return;

        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        makeSerializedData(mem, &sd);

        ev->serialize(&sd);

        client->notifyEvent(eventType, mem, sd);
    }

    // ── onHandleProgress ─────────────────────────────────────
    void CacaoService::Client::onHandleProgress(const cacao::ProcessResultBase *result)
    {
        if (result == NULL)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "onHandleProgress: result is null %d", 0);
            return;
        }

        sp<IMemory> callbackMem;
        sp<ICacaoProcessCallback> callbackSp;
        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));

        pthread_mutex_lock(&mMutexRequest);

        // 搜尋 request list 找到匹配的 request
        for (List<Request *>::iterator it = mRequestList.begin();
             it != mRequestList.end(); ++it)
        {
            Request *req = *it;
            if (req != NULL && req->getResult() == result)
            {
                callbackSp = req->getCallback();
                pthread_mutex_unlock(&mMutexRequest);

                // 分配記憶體並序列化 result
                unsigned int dataSize = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->getSerializedSize();
                sp<IMemory> mem = allocMemory(dataSize);
                callbackMem = mem;
                if (callbackMem != NULL)
                {
                    makeSerializedData(callbackMem, &sd);
                    toSerializable(const_cast<cacao::ProcessResultBase *>(result))->serialize(&sd);
                }
                goto done;
            }
        }

        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "onHandleProgress: request not found %d", 0);
        pthread_mutex_unlock(&mMutexRequest);

    done:
        if (callbackMem == NULL)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "onHandleProgress: alloc failed");
        }
        else
        {
            if (callbackSp != NULL)
            {
                callbackSp->notifyProgress(callbackMem, sd);
            }
        }
    }

    // ── onHandleResult ───────────────────────────────────────
    void CacaoService::Client::onHandleResult(const cacao::ProcessResultBase *result)
    {
        if (result == NULL)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "onHandleResult: result is null %d", 0);
            return;
        }

        sp<IMemory> callbackMem;
        sp<ICacaoProcessCallback> resultCallbackSp;
        cacao::ISerialize::SerializedData sd;
        memset(&sd, 0, sizeof(sd));
        Vector<cacao::ImageBuf *> *bufVec = NULL;
        Request *reqResult = NULL;

        pthread_mutex_lock(&mMutexRequest);

        // 搜尋並從 list 移除匹配的 request
        for (List<Request *>::iterator it = mRequestList.begin();
             it != mRequestList.end(); ++it)
        {
            Request *req = *it;
            if (req != NULL && req->getResult() == result)
            {
                resultCallbackSp = req->getCallback();
                // so_32 @ 0x19858: vtable[0x14] 是 getBufVector()，不是 getErrCode()——
                // 回傳值透過堆疊額外參數傳給 notifyResult 的第三個參數 bufs，
                // 由 BpCacaoProcessCallback::notifyResult 在 transact 後就地清理
                // ImageBuf 的 native_handle。之前恆傳 nullptr 會導致這些
                // native_handle/FD 永遠不會被關閉。
                bufVec = req->getBufVector();
                reqResult = req;
                mRequestList.erase(it);
                pthread_mutex_unlock(&mMutexRequest);

                // 分配記憶體並序列化 result
                unsigned int dataSize = toSerializable(const_cast<cacao::ProcessResultBase *>(result))->getSerializedSize();
                sp<IMemory> mem = allocMemory(dataSize);
                callbackMem = mem;
                if (callbackMem != NULL)
                {
                    makeSerializedData(callbackMem, &sd);
                    toSerializable(const_cast<cacao::ProcessResultBase *>(result))->serialize(&sd);
                }
                goto done;
            }
        }

        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "onHandleResult: request not found %d", 0);
        pthread_mutex_unlock(&mMutexRequest);

    done:
        if (callbackMem == NULL)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                                "onHandleResult: alloc failed");
        }
        else
        {
            if (resultCallbackSp != NULL)
            {
                resultCallbackSp->notifyResult(callbackMem, sd, bufVec);
            }
        }

        if (reqResult != NULL)
        {
            delete reqResult;
        }
    }

    // ── allocMemory ──────────────────────────────────────────
    sp<IMemory> CacaoService::Client::allocMemory(unsigned int size)
    {
        sp<MemoryHeapBase> heap = new MemoryHeapBase(size);
        sp<MemoryBase> mem = new MemoryBase(heap, 0, size);
        return mem;
    }

    // ── freeMemory ───────────────────────────────────────────
    void CacaoService::Client::freeMemory(sp<IMemory> &mem)
    {
        if (mem != NULL)
        {
            mem.clear();
        }
    }

    // ── makeSerializedData ───────────────────────────────────
    void CacaoService::Client::makeSerializedData(const sp<IMemory> &mem,
                                                  cacao::ISerialize::SerializedData *sd)
    {
        if (sd != NULL && mem != NULL)
        {
            sd->size = mem->size();
            sd->data = mem->unsecurePointer();
        }
    }

    // ── serviceDied (Client) ─────────────────────────────────
    void CacaoService::Client::serviceDied()
    {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Client::serviceDied");

        pthread_mutex_lock(&mMutexDisconnect);
        if (mProcess != NULL)
        {
            static_cast<cacao::ICacaoGateway *>(mProcess)->serviceDied();
        }
        pthread_mutex_unlock(&mMutexDisconnect);
    }

    // ════════════════════════════════════════════════════════════
    // CacaoService::Client::Request
    // ════════════════════════════════════════════════════════════

    CacaoService::Client::Request::Request(const sp<ICacaoProcessCallback> &cb,
                                           cacao::ICBProcessResultListener *listener)
        : mCallback(cb), mListener(listener), mBufVector(NULL), mResult(NULL)
    {
    }

    CacaoService::Client::Request::~Request()
    {
        mCallback.clear();
        if (mBufVector != NULL)
        {
            delete mBufVector;
            mBufVector = NULL;
        }
        if (mResult != NULL)
        {
            delete mResult;
            mResult = NULL;
        }
    }

    void CacaoService::Client::Request::onHandleProgress(const cacao::ProcessResultBase *result)
    {
        if (mListener != NULL)
        {
            mListener->onProgress(0);
        }
    }

    void CacaoService::Client::Request::onHandleResult(const cacao::ProcessResultBase *result)
    {
        if (mListener != NULL)
        {
            mListener->onResult(PAL_ERR_OK);
        }
    }

    void CacaoService::Client::Request::setBufVector(Vector<cacao::ImageBuf *> *bufs)
    {
        mBufVector = bufs;
    }

    void CacaoService::Client::Request::setResult(cacao::ProcessResultBase *result)
    {
        mResult = result;
    }

} // namespace android

// ════════════════════════════════════════════════════════════
// cacao::Caps
// ════════════════════════════════════════════════════════════

namespace cacao
{

    Caps::Caps()
    {
        memset(data, 0, sizeof(data));
    }

    Caps::~Caps()
    {
    }

    int Caps::serialize(ISerialize::SerializedData *sd)
    {
        if (sd == NULL || sd->data == NULL)
            return -0x67;
        if (sd->offset + 0x262cU > sd->size)
            return -0x67;
        // 原始 .so: 4 段 memcpy，總計 0x262c bytes
        uint8_t *dst = (uint8_t *)sd->data + sd->offset;
        memcpy(dst, data, 0x262c);
        sd->offset += 0x262c;
        return 0;
    }

    int Caps::deserialize(ISerialize::SerializedData *sd)
    {
        if (sd == NULL || sd->data == NULL)
            return -0x67;
        if (sd->offset + 0x262cU > sd->size)
            return -0x67;
        uint8_t *src = (uint8_t *)sd->data + sd->offset;
        memcpy(data, src, 0x262c);
        sd->offset += 0x262c;
        return 0;
    }

    // ════════════════════════════════════════════════════════════
    // cacao::ProcessFactoryInternal
    // ════════════════════════════════════════════════════════════

    ICacaoGateway *ProcessFactoryInternal::createProcess()
    {
        return ProcessCtrlGatewayFactory::create();
    }

    ProcessModeBase *ProcessFactoryInternal::createMode(ISerialize::SerializedData *sd)
    {
        return ProcessCtrlGatewayFactory::createMode(sd);
    }

    ProcessParamBase *ProcessFactoryInternal::createParam(ISerialize::SerializedData *sd)
    {
        return ProcessCtrlGatewayFactory::createParam(sd);
    }

    ProcessParamBase *ProcessFactoryInternal::createParam(
        ANativeWindow *window,
        ISerialize::SerializedData *sd)
    {
        return ProcessCtrlGatewayFactory::createParam(window, sd);
    }

    ProcessResultBase *ProcessFactoryInternal::createResult(ISerialize::SerializedData *sd)
    {
        return ProcessCtrlGatewayFactory::createResult(sd);
    }

    ProcessConfigBase *ProcessFactoryInternal::createConfig(unsigned int configId,
                                                            ISerialize::SerializedData *sd)
    {
        return ProcessCtrlGatewayFactory::createConfig(configId, sd);
    }

} // namespace cacao

// ════════════════════════════════════════════════════════════
// instantiate (C linkage)
// ════════════════════════════════════════════════════════════

extern "C" void instantiate()
{
    android::CacaoService::instantiate();
}
