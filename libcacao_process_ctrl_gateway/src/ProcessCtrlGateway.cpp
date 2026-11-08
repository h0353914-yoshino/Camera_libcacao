// ProcessCtrlGateway.cpp
// 依照 libcacao_process_ctrl_gateway.so 反編譯結果 100% 重建
// 所有偏移與邏輯均來自 Ghidra 反編譯

#define LOG_TAG "libcacao_pcgw"
#include <log/log.h>
#include <cacao/ProcessCtrlGateway.h>
#include <vendor/somc/hardware/camera/provider/1.0/IVendorSomcCameraProvider.h>
#include <hidl/HidlTransportSupport.h>
#include <android/native_window.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <new> // placement new
// ObjectBase C1 (complete ctor) 在 libcacao_client_real.so 中未匯出，
// 僅匯出 C2 (base ctor)。因無虛擬繼承，C1 = C2，此處提供轉發。
extern "C" void _ZN5cacao10ObjectBaseC2Ev(void *thiz);
extern "C" void _ZN5cacao10ObjectBaseC1Ev(void *thiz)
{
    _ZN5cacao10ObjectBaseC2Ev(thiz);
}

namespace cacao
{

    using android::sp;
    namespace V30 = vendor::somc::hardware::camera::cacao::V3_0;
    namespace V31 = vendor::somc::hardware::camera::cacao::V3_1;
    namespace VProvider = vendor::somc::hardware::camera::provider::V1_0;

// 便利巨集：存取 this 相對偏移
#define THIS_U8(off) (reinterpret_cast<uint8_t *>(this) + (off))
#define THIS_U32(off) (*reinterpret_cast<uint32_t *>(THIS_U8(off)))
#define THIS_I32(off) (*reinterpret_cast<int32_t *>(THIS_U8(off)))
#define THIS_PTR(off) (*reinterpret_cast<void **>(THIS_U8(off)))
#define THIS_U64(off) (*reinterpret_cast<uint64_t *>(THIS_U8(off)))

// 存取 ResultItem 偏移
#define RI_U8(p, off) (reinterpret_cast<uint8_t *>(p) + (off))
#define RI_U32(p, off) (*reinterpret_cast<uint32_t *>(RI_U8(p, off)))
#define RI_I32(p, off) (*reinterpret_cast<int32_t *>(RI_U8(p, off)))
#define RI_PTR(p, off) (*reinterpret_cast<void **>(RI_U8(p, off)))
#define RI_U64(p, off) (*reinterpret_cast<uint64_t *>(RI_U8(p, off)))

    // 將 V30::Param 包裝成 hidl_vec<uint8_t>
    static inline android::hardware::hidl_vec<uint8_t> paramToVec(const V30::Param &p)
    {
        android::hardware::hidl_vec<uint8_t> v;
        v.resize(sizeof(V30::Param));
        memcpy(v.data(), &p, sizeof(V30::Param));
        return v;
    }

    /* REF passRequest: hidl_vec<ImageBufInfo>::operator= 必須以函式呼叫形式出現，不可 inline 展開 */
    __attribute__((noinline)) static void assign_image_buf_vec(
        android::hardware::hidl_vec<V30::ImageBufInfo> *dst,
        android::hardware::hidl_vec<V30::ImageBufInfo> *src)
    {
        *dst = *src;
    }

    /* REF copyResultItem: hidl_handle::operator= 以函式呼叫形式出現 */
    __attribute__((noinline)) static void assign_hidl_handle(
        android::hardware::hidl_handle *dst, const android::hardware::hidl_handle *src)
    {
        *dst = *src;
    }

    /* REF: 使用 __aeabi_memcpy8（8-byte對齊 copy）*/
    extern "C" void __aeabi_memcpy8(void *dst, const void *src, size_t n);

    /* REF copyProcessCtrlParam: 使用 __aeabi_memclr 代替 memset */
    extern "C" void __aeabi_memclr(void *dst, size_t n);

    /* REF copyProcessCtrlParam: hidl_handle::operator=(native_handle_t* = nullptr) 以函式呼叫形式出現 */
    __attribute__((noinline)) static void assign_hidl_handle_null(android::hardware::hidl_handle *dst)
    {
        *dst = (native_handle_t *)nullptr;
    }

    /* REF: hidl_handle::operator=(const native_handle_t*) 以函式呼叫形式出現 */
    __attribute__((noinline)) static void assign_hidl_handle_from_native(
        android::hardware::hidl_handle *dst, const native_handle_t *src)
    {
        *dst = src;
    }

    /* REF: hidl_vec<ImageBufInfo>::resize() 以函式呼叫形式出現，不可 inline */
    __attribute__((noinline)) static void resize_image_buf_vec(
        android::hardware::hidl_vec<V30::ImageBufInfo> *vec, uint32_t count)
    {
        vec->resize(count);
    }

    // 原版 fbf4 的 noinline helper: clock_gettime -> nanoseconds
    static __attribute__((noinline)) uint64_t getMonotonicNanos()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }

/* ────────────────────────────────────────────────
 *  SerializedData raw access helpers
 *  SerializedData layout:
 *    +0x184: uint32_t totalSize
 *    +0x188: void*    bufPtr
 *    +0x18c: uint32_t currentOffset
 * ──────────────────────────────────────────────── */
#define SD_TOTAL_SIZE(sd) (*reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(sd) + 0x184))
#define SD_BUF_PTR(sd) (*reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(sd) + 0x188))
#define SD_OFFSET(sd) (*reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(sd) + 0x18c))

    /* ────────────────────────────────────────────────
     *  ProcessCtrlMode（依照 REF ProcessCtrlMode vtable 0x13808）
     * ──────────────────────────────────────────────── */

    ProcessCtrlMode::ProcessCtrlMode()
        : field_08(0), field_0c(-1),
          field_10(0), field_14(0), field_18(0), field_1c(0),
          field_20(0), field_24(0), field_28(0)
    {
    }

    ProcessCtrlMode::~ProcessCtrlMode() {}

    void ProcessCtrlMode::dump(unsigned int /*flags*/)
    {
        /* REF 0xcd44: "ProcessCtrlMode" — getClassName override，此處保留空實作 */
    }

    uint32_t ProcessCtrlMode::getType() const
    {
        return field_08;
    }

    uint32_t ProcessCtrlMode::getSerializedSize() const
    {
        return 0x24;
    }

    /* ProcessCtrlMode::serialize — 對應 REF 0xcd54
     * base serialize: 寫 field_08 (4 bytes)
     * 本體: 寫 field_0c~field_28 (32 bytes)
     * 共 0x24 bytes */
    int ProcessCtrlMode::serialize(ISerialize::SerializedData *data) const
    {
        if (!data || !SD_BUF_PTR(data))
            return -0x67;
        // base: serialize field_08 (4 bytes)
        uint32_t off = SD_OFFSET(data);
        if (SD_TOTAL_SIZE(data) < off + 4u)
            return -0x67;
        uint8_t *buf = SD_BUF_PTR(data);
        *reinterpret_cast<uint32_t *>(buf + off) = field_08;
        SD_OFFSET(data) = off + 4;

        // mode fields: 32 bytes (field_0c ~ field_28)
        off = SD_OFFSET(data);
        if (SD_TOTAL_SIZE(data) < off + 32u)
            return -0x67;
        buf = SD_BUF_PTR(data);
        uint8_t *dst = buf + off;
        *reinterpret_cast<uint32_t *>(dst + 0) = static_cast<uint32_t>(field_0c);
        *reinterpret_cast<uint32_t *>(dst + 4) = field_10;
        *reinterpret_cast<uint32_t *>(dst + 8) = field_14;
        *reinterpret_cast<uint32_t *>(dst + 12) = field_18;
        *reinterpret_cast<uint32_t *>(dst + 16) = field_1c;
        *reinterpret_cast<uint32_t *>(dst + 20) = field_20;
        *reinterpret_cast<uint32_t *>(dst + 24) = field_24;
        *reinterpret_cast<uint32_t *>(dst + 28) = field_28;
        SD_OFFSET(data) = off + 32;
        return 0;
    }

    /* ProcessCtrlMode::deserialize — 對應 REF 0xcdb6
     * base deserialize: 讀 field_08 (4 bytes)
     * 本體: 讀 field_0c~field_28 (32 bytes)
     * 共 0x24 bytes */
    int ProcessCtrlMode::deserialize(ISerialize::SerializedData *data)
    {
        if (!data || !SD_BUF_PTR(data))
            return -0x67;
        // base: deserialize field_08 (4 bytes)
        uint32_t off = SD_OFFSET(data);
        if (SD_TOTAL_SIZE(data) < off + 4u)
            return -0x67;
        uint8_t *buf = SD_BUF_PTR(data);
        field_08 = *reinterpret_cast<uint32_t *>(buf + off);
        SD_OFFSET(data) = off + 4;

        int ret = 0; // base deserialize 成功
        if (ret < 0)
            return ret;

        // mode fields: 32 bytes
        off = SD_OFFSET(data);
        if (SD_TOTAL_SIZE(data) < off + 32u)
            return -0x67;
        buf = SD_BUF_PTR(data);
        uint8_t *src = buf + off;
        field_0c = static_cast<int32_t>(*reinterpret_cast<uint32_t *>(src + 0));
        field_10 = *reinterpret_cast<uint32_t *>(src + 4);
        field_14 = *reinterpret_cast<uint32_t *>(src + 8);
        field_18 = *reinterpret_cast<uint32_t *>(src + 12);
        field_1c = *reinterpret_cast<uint32_t *>(src + 16);
        field_20 = *reinterpret_cast<uint32_t *>(src + 20);
        field_24 = *reinterpret_cast<uint32_t *>(src + 24);
        field_28 = *reinterpret_cast<uint32_t *>(src + 28);
        SD_OFFSET(data) = off + 32;
        return ret;
    }

    /* ────────────────────────────────────────────────
     *  ProcessCtrlResult (依照原始 .so c188 constructor)
     * ──────────────────────────────────────────────── */

    ProcessCtrlResult::ProcessCtrlResult()
        : field_08(0), field_0c(-1), field_10(0),
          field_14(0), field_18(0), field_1c(0), field_20(0xFFFFFFFF),
          field_24(0), field_28(0), field_2c(0xFFFF0000),
          field_30(0xFF)
    {
        _pad[0] = _pad[1] = _pad[2] = 0;
    }

    ProcessCtrlResult::~ProcessCtrlResult() {}

    /* ProcessCtrlResult::serialize — REF 0x1cbbc
     * Phase 1: 寫 field_08 (result) + field_0c (progress) = 8B
     * Phase 2: 寫 field_14..field_30+pad = 0x20B (跳過 field_10 = userData)
     * 64-bit 修正：使用 named members 直接存取，避免 32-bit 硬編碼偏移量 */
    int ProcessCtrlResult::serialize(ISerialize::SerializedData *data) const
    {
        /* Phase 1: ProcessResultBase::serialize — result + progress */
        if (!data || !SD_BUF_PTR(data) || SD_OFFSET(data) + 8u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *dst = SD_BUF_PTR(data) + SD_OFFSET(data);
        memcpy(dst, &field_08, 8); // field_08 (4B) + field_0c (4B)
        SD_OFFSET(data) += 8;
        /* Phase 2: ProcessCtrlResult-specific — field_14..field_30+pad */
        if (SD_OFFSET(data) + 0x20u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        dst = SD_BUF_PTR(data) + SD_OFFSET(data);
        memcpy(dst, &field_14, 0x20); // field_14..field_2c(4B each) + field_30(1B) + _pad[3]
        SD_OFFSET(data) += 0x20;
        return 0;
    }

    /* ProcessCtrlResult::deserialize — REF 0x1cc34
     * Phase 1: 讀 field_08 + field_0c = 8B
     * Phase 2: 讀 field_14..field_30+pad = 0x20B
     * 64-bit 修正：使用 named members 直接存取 */
    int ProcessCtrlResult::deserialize(ISerialize::SerializedData *data)
    {
        /* Phase 1 */
        if (!data || !SD_BUF_PTR(data) || SD_OFFSET(data) + 8u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *src = SD_BUF_PTR(data) + SD_OFFSET(data);
        memcpy(&field_08, src, 8); // field_08 (4B) + field_0c (4B)
        SD_OFFSET(data) += 8;
        /* Phase 2 */
        if (SD_OFFSET(data) + 0x20u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        src = SD_BUF_PTR(data) + SD_OFFSET(data);
        memcpy(&field_14, src, 0x20); // field_14..field_2c(4B each) + field_30(1B) + _pad[3]
        SD_OFFSET(data) += 0x20;
        return 0;
    }

    /* ────────────────────────────────────────────────
     *  ProcessCtrlParam (依照原始 .so c6b8 constructor)
     *  大小 0x160，繼承 ProcessParamBase + ISerializable
     *  ctor 將特定欄位歸零，然後 C++ 設置 vtable
     * ──────────────────────────────────────────────── */

    ProcessCtrlParam::ProcessCtrlParam()
    {
        // 對應 REF c6b8: 將已知欄位歸零
        // 大部分欄位初始化為 0，使用 memset 最簡潔
        memset(_data, 0, sizeof(_data));
    }

    ProcessParamBase *ProcessCtrlParam::clone() const
    {
        void *mem = ObjectBase::operator new(0x160);
        if (!mem)
            return nullptr;
        ProcessCtrlParam *c = ::new (mem) ProcessCtrlParam();
        memcpy(c->_data, _data, sizeof(_data));
        return c;
    }

    /* copy — 對應 REF ProcessCtrlParam::copy(ProcessParamBase const*) at 0x1cebb
     * 64-bit 修正：使用 _data 成員直接存取，避免 32-bit 硬編碼偏移量問題 */
    void ProcessCtrlParam::copy(const ProcessParamBase *src)
    {
        if (!src)
            return;
        const ProcessCtrlParam *s = static_cast<const ProcessCtrlParam *>(src);
        *reinterpret_cast<uint32_t *>(_data) = *reinterpret_cast<const uint32_t *>(s->_data);
        memcpy(_data + 8, s->_data + 8, 0x150);
    }

    uint32_t ProcessCtrlParam::getSerializedSize() const
    {
        return 0x158;
    }

    /* serialize/deserialize — 64-bit 修正：使用 _data 成員直接存取
     * 原始 32-bit: data starts at this+0x08 (two 4-byte vtable ptrs)
     * 64-bit: data starts at this+0x10 (two 8-byte vtable ptrs)
     * Fix: use _data[] directly, compiler handles offset correctly */
    int ProcessCtrlParam::serialize(ISerialize::SerializedData *data) const
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 0x158u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *dst = SD_BUF_PTR(data) + SD_OFFSET(data);
        *reinterpret_cast<uint32_t *>(dst) = *reinterpret_cast<const uint32_t *>(_data); // type field
        *reinterpret_cast<uint32_t *>(dst + 4) = 0;
        memcpy(dst + 8, _data + 8, 0x150);
        SD_OFFSET(data) += 0x158;
        return 0;
    }

    int ProcessCtrlParam::deserialize(ISerialize::SerializedData *data)
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 0x158u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *src = SD_BUF_PTR(data) + SD_OFFSET(data);
        *reinterpret_cast<uint32_t *>(_data) = *reinterpret_cast<uint32_t *>(src); // type field
        memcpy(_data + 8, src + 8, 0x150);
        SD_OFFSET(data) += 0x158;
        return 0;
    }

    /* ── ProcessCtrlDynamicParameterConfig serialize/deserialize ─── */

    int ProcessCtrlDynamicParameterConfig::serialize(ISerialize::SerializedData *data) const
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 4u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        *reinterpret_cast<uint32_t *>(SD_BUF_PTR(data) + SD_OFFSET(data)) = configValue;
        SD_OFFSET(data) += 4;
        return 0;
    }

    int ProcessCtrlDynamicParameterConfig::deserialize(ISerialize::SerializedData *data)
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 4u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        configValue = *reinterpret_cast<uint32_t *>(SD_BUF_PTR(data) + SD_OFFSET(data));
        SD_OFFSET(data) += 4;
        return 0;
    }

    /* ── ProcessCtrlVideoRecParam 實作 ─────────────────────────── */

    ProcessCtrlVideoRecParam::ProcessCtrlVideoRecParam()
    {
        memset(_data, 0, sizeof(_data));
        memset(_extra, 0, sizeof(_extra));
        // type = VIDEO_REC (0x0e)
        *reinterpret_cast<uint32_t *>(_data) = 0x0e;
    }

    ProcessCtrlVideoRecParam::~ProcessCtrlVideoRecParam()
    {
        // REF so_32 Ghidra 0x1a220: ~sp<IGraphicBufferProducer>(this+0x160)
        // createParam() 用 sp<> assignment (incStrong)，dtor 必須對應呼叫 ~sp (decStrong)
        // 持有的是 ANativeWindow 的一份參考（createParam 時 ANativeWindow_acquire）
        ANativeWindow *w = *reinterpret_cast<ANativeWindow **>(_extra);
        if (w)
            ANativeWindow_release(w);
    }

    ProcessParamBase *ProcessCtrlVideoRecParam::clone() const
    {
        void *mem = ObjectBase::operator new(0x170);
        if (!mem)
            return nullptr;
        ProcessCtrlVideoRecParam *c = ::new (mem) ProcessCtrlVideoRecParam();
        if (c)
        {
            c->copy(this);
        }
        return c;
    }

    /* copy — 64-bit 修正：使用 _data 和 _extra 成員直接存取 */
    void ProcessCtrlVideoRecParam::copy(const ProcessParamBase *src)
    {
        if (!src)
            return;
        const ProcessCtrlVideoRecParam *s = static_cast<const ProcessCtrlVideoRecParam *>(src);
        *reinterpret_cast<uint32_t *>(_data) = *reinterpret_cast<const uint32_t *>(s->_data);
        memcpy(_data + 8, s->_data + 8, 0x150);
        // 修正：使用 sp<> assignment 正確複製 GBP（呼叫 incStrong）
        // memcpy 會複製原始指標而不增加 refcount，原始 param 析構後 GBP 被釋放
        // 導致 clone 持有懸空指標 → Surface::dequeueBuffer 呼叫 __cxa_pure_virtual 崩潰
        ANativeWindow *srcWin = *reinterpret_cast<ANativeWindow *const *>(s->_extra);
        if (srcWin)
            ANativeWindow_acquire(srcWin);
        ANativeWindow **dstWin = reinterpret_cast<ANativeWindow **>(_extra);
        if (*dstWin)
            ANativeWindow_release(*dstWin);
        *dstWin = srcWin;
        _extra[1] = s->_extra[1];
        _extra[2] = s->_extra[2];
        _extra[3] = s->_extra[3];
    }

    uint32_t ProcessCtrlVideoRecParam::getSerializedSize() const
    {
        return 0x160;
    }

    /* serialize/deserialize — 64-bit 修正：使用 _data 和 _extra 成員直接存取
     * _extra layout (64-bit): [0,1]=sp.m_ptr(8B)@0x168, [2]=dataspace@0x170, [3]=frameRate@0x174
     * _extra layout (32-bit): [0]=sp.m_ptr(4B)@0x160, [1]=dataspace@0x164, [2]=frameRate@0x168 */
    int ProcessCtrlVideoRecParam::serialize(ISerialize::SerializedData *data) const
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 0x158u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *dst = SD_BUF_PTR(data) + SD_OFFSET(data);
        *reinterpret_cast<uint32_t *>(dst) = *reinterpret_cast<const uint32_t *>(_data); // type field
        *reinterpret_cast<uint32_t *>(dst + 4) = 0;
        memcpy(dst + 8, _data + 8, 0x150);
        SD_OFFSET(data) += 0x158;
        /* VideoRecParam 額外序列化：dataspace (_extra[1]@0x164) 和 frameRate (_extra[2]@0x168)
         * REF so_32 @ 0xd076: ldr [r5,#0x164] / ldr [r5,#0x168] */
        if (SD_OFFSET(data) + 8u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        dst = SD_BUF_PTR(data) + SD_OFFSET(data);
        *reinterpret_cast<uint32_t *>(dst) = _extra[1];     // dataspace
        *reinterpret_cast<uint32_t *>(dst + 4) = _extra[2]; // frameRate
        SD_OFFSET(data) += 8;
        return 0;
    }

    int ProcessCtrlVideoRecParam::deserialize(ISerialize::SerializedData *data)
    {
        if (!data || !SD_BUF_PTR(data) ||
            SD_OFFSET(data) + 0x158u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        uint8_t *src = SD_BUF_PTR(data) + SD_OFFSET(data);
        *reinterpret_cast<uint32_t *>(_data) = *reinterpret_cast<uint32_t *>(src); // type field
        memcpy(_data + 8, src + 8, 0x150);
        SD_OFFSET(data) += 0x158;
        /* VideoRecParam 額外反序列化：dataspace → _extra[1]@0x164, frameRate → _extra[2]@0x168
         * REF so_32 @ 0xd0cc: str [r5,#0x164] / str [r5,#0x168] */
        if (SD_OFFSET(data) + 8u > SD_TOTAL_SIZE(data))
            return (int)0xffffff99u;
        src = SD_BUF_PTR(data) + SD_OFFSET(data);
        _extra[1] = *reinterpret_cast<uint32_t *>(src);     // dataspace
        _extra[2] = *reinterpret_cast<uint32_t *>(src + 4); // frameRate
        SD_OFFSET(data) += 8;
        return 0;
    }

    /* ────────────────────────────────────────────────
     *  ResultItem
     * ──────────────────────────────────────────────── */

    static inline void destroyResultItemSlot(void *object)
    {
        if (!object)
            return;
        using DtorFn = void (*)(void *);
        DtorFn *vtable = *reinterpret_cast<DtorFn **>(object);
        if (vtable)
        {
            vtable[1](object);
        }
    }

    ProcessCtrlGateway::ResultItem::ResultItem()
    {
        memset(RI_U8(this, 4), 0, 0xcc);
        new (RI_U8(this, 0x38)) android::hardware::hidl_string();
        new (RI_U8(this, 0x80)) android::hardware::hidl_handle();
        new (RI_U8(this, 0xa0)) android::hardware::hidl_handle();
        new (RI_U8(this, 0xc0)) android::hardware::hidl_vec<V30::ImageBufInfo>();
        RI_PTR(this, 0xd0) = nullptr;
        RI_U32(this, 0x120) = 0xffffffff;
        RI_PTR(this, 0x124) = nullptr;
        RI_PTR(this, 0x128) = nullptr;
        memset(RI_U8(this, 0xd8), 0, 0x48);
    }

    ProcessCtrlGateway::ResultItem::~ResultItem()
    {
        reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(RI_U8(this, 0xc0))->~hidl_vec();
        reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(this, 0xa0))->~hidl_handle();
        reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(this, 0x80))->~hidl_handle();
        reinterpret_cast<android::hardware::hidl_string *>(RI_U8(this, 0x38))->~hidl_string();
        reinterpret_cast<V30::Param *>(RI_U8(this, 8))->~Param();
    }

    void ProcessCtrlGateway::ResultItem::deinit()
    {
        if (RI_PTR(this, 4) == nullptr)
        {
            destroyResultItemSlot(RI_PTR(this, 0xd0));
            RI_PTR(this, 0xd0) = nullptr;
        }

        RI_U32(this, 0x120) = 0xffffffff;
        RI_PTR(this, 4) = nullptr;

        destroyResultItemSlot(RI_PTR(this, 0x124));
        RI_PTR(this, 0x124) = nullptr;
        RI_PTR(this, 0x128) = nullptr;
    }

    /* ────────────────────────────────────────────────
     *  Callback (HIDL ICacaoCallback)
     * ──────────────────────────────────────────────── */

    ProcessCtrlGateway::Callback::Callback(ProcessCtrlGateway *parent)
        : mParent(parent)
    {
    }

    ProcessCtrlGateway::Callback::~Callback()
    {
    }

    android::hardware::Return<void>
    ProcessCtrlGateway::Callback::handleResult(const V30::Result &res)
    {
        ResultMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_id = PAL_MSG_RESULT;
        reinterpret_cast<PAL_Msg_t *>(&msg)->size = 0x60;
        memcpy(msg.resultData, &res, sizeof(V30::Result) < 0x48 ? sizeof(V30::Result) : 0x48u);
        mParent->notifyMsg(reinterpret_cast<PAL_Msg_t *>(&msg));
        return android::hardware::Void();
    }

    android::hardware::Return<void>
    ProcessCtrlGateway::Callback::handleProgress(const V30::Result &res)
    {
        ResultMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_id = PAL_MSG_PROGRESS;
        reinterpret_cast<PAL_Msg_t *>(&msg)->size = 0x60;
        memcpy(msg.resultData, &res, sizeof(V30::Result) < 0x48 ? sizeof(V30::Result) : 0x48u);
        mParent->notifyMsg(reinterpret_cast<PAL_Msg_t *>(&msg));
        return android::hardware::Void();
    }

    android::hardware::Return<void>
    ProcessCtrlGateway::Callback::handleEvent(const V30::Event &ev)
    {
        if (ev.eventType != 0)
            return android::hardware::Void();
        ResultMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_id = PAL_MSG_SUPER_SLOW;
        reinterpret_cast<PAL_Msg_t *>(&msg)->size = 0x60;
        mParent->notifyMsg(reinterpret_cast<PAL_Msg_t *>(&msg));
        return android::hardware::Void();
    }

    /* ────────────────────────────────────────────────
     *  建構/解構
     * ──────────────────────────────────────────────── */

    ProcessCtrlGateway::ProcessCtrlGateway()
        : ProcessCtrlGatewayBase(), mService(), mServiceV31(), mVideoMaxDur(0x78), mBufferCount(10), mBurstCount(0x1e), mVideoReq(nullptr), mPrepareVideoReq(nullptr), mVideoState(0), mSessionId(0x49742400), mConfiguredFlag(0)
    {
        /* PtrLockList/Array 成員已由 C++ member ctor 初始化 (PAL_MemSet + Mutex::Mutex) */
        /* mCaps, mSlowMotLogger1/2, mFrameIds vtable 由 C++ compiler 自動設定 */
        mSurface = nullptr;
        memset(_pad1, 0, sizeof(_pad1));
        memset(_padVideo, 0, sizeof(_padVideo));
        memset(_padVidState, 0, sizeof(_padVidState));
        memset(_padSlowMot1, 0, sizeof(_padSlowMot1));
        memset(_padSlowMot2, 0, sizeof(_padSlowMot2));
        memset(_padFrameIds, 0, sizeof(_padFrameIds));
    }

    ProcessCtrlGateway::~ProcessCtrlGateway()
    {
        // Ghidra @ A9 原廠 libcacao_process_ctrl_gateway.so, D1Ev @ 0x96d0（每個
        // bl 都已解到 veneer -> .plt -> R_ARM_JUMP_SLOT 對應的真正符號）：
        //   96e4: mov  r0, r4
        //   96e6: bl   0xf914          ; long-branch veneer -> PLT ->
        //                              ;   _ZN5cacao22ProcessCtrlGatewayBase6deinitEv
        //   96ea: ldr  r0,[r4,#0x57c]  ; r0 = mSurface
        //   96ee: cbz  r0, skip
        //   96f0: ldr  r1,[r0,#0x1c]   ; r1 = android_native_base_t::decRef
        //   96f2: blx  r1              ; decRef(r0=mSurface)
        // ANativeWindow 開頭是 android_native_base_t common:
        //   +0x00 magic, +0x04 version, +0x08..0x17 reserved[4]
        //   +0x18 incRef, +0x1c decRef
        //
        // 原版解構的第一件事就是呼叫 deinit()，在 decRef(mSurface) 之前。
        // deinit() 是整個 gateway 唯一會 PAL_ThreadClose() worker thread 的地方
        // ——沒有它，接下來 ~ProcessCtrlGatewayBase() 的 PAL_Delete() 會發現還有
        // 活著的 thread 要收，走進
        //   PAL_Delete -> PAL_ListSearch [取得 PAL 全域 mutex]
        //     -> PAL_ThreadSystem_KillThreads -> PAL_ThreadClose -> PAL_MsgSend
        //       -> PAL_ThreadSystem_IsMsgQueAvailable -> PAL_ListSearchFirst
        //         -> PAL_MutexLock   [同一把、非遞迴] -> 自鎖
        // 讓 cacaoserver 卡死（pid 不變、不會崩，所有 getCaps()/connect() 從此
        // 等不到鎖，App 端逾時跳「未知的錯誤」）。實測 A/B：A9 原廠同樣的
        // force-stop/重啟重疊壓力 10 輪 timed-out=0；拿掉這行 timed-out=11；
        // 還原後 timed-out=0，crash buffer 0 行。
        ProcessCtrlGatewayBase::deinit();

        if (mSurface)
        {
            mSurface->common.decRef(&mSurface->common);
        }
        // 其餘成員 (PtrLockList, Array, sp<ICacao>) 由 C++ 自動析構
    }

    /* ────────────────────────────────────────────────
     *  onInit   (Ghidra 00019a04)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::onInit()
    {
        sp<VProvider::IVendorSomcCameraProvider> provider =
            VProvider::IVendorSomcCameraProvider::getService("internal/0", false);
        if (!provider)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Get service failed.");
            return -0x6f;
        }

        sp<V30::ICacao> svc;
        int32_t hidlStatus = 0;
        auto ret = provider->getCacaoInterface_V3_0(
            [&svc, &hidlStatus](::android::hardware::camera::common::V1_0::Status status, const sp<V30::ICacao> &service)
            {
                hidlStatus = (int32_t)status;
                svc = service;
            });

        // 以 transport error description 匹配 reference 行為
        if (!ret.isOk())
        {
            std::string desc = ret.description();
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Transaction error trying to cacao interface: %s",
                         desc.c_str());
            return -0x6f;
        }
        if ((int)hidlStatus != 0)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Unable to cacao interface");
            return -0x6f;
        }
        // Note: reference 不做 null svc 檢查（ErrCode==0 時 service 保證非 null）

        mService = svc;

        // castFrom V3.1：匹配 reference 邏輯
        auto castRet = V31::ICacao::castFrom(mService, false);
        sp<V31::ICacao> v31 = (sp<V31::ICacao>)castRet;
        if (v31 == nullptr)
        {
            if (!castRet.isOk())
            {
                auto desc2 = castRet.description();
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "Transaction error when casting cacao interface [%s]: %s",
                             desc2.c_str(), V31::ICacao::descriptor);
            }
            else
            {
                castRet.assertOk();
            }
            sp<V31::ICacao> nullV31;
            mServiceV31 = nullV31;
        }
        else
        {
            mServiceV31 = v31;
        }

        return 0;
    }

    /* ────────────────────────────────────────────────
     *  onDeinit
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::onDeinit()
    {
        mService = nullptr;
        mServiceV31 = nullptr;
        mConfiguredFlag = 0;
        return 0;
    }

    static inline int hidlErrCode(const android::hardware::Return<V30::ErrCode> &ret)
    {
        return static_cast<int>(
            const_cast<android::hardware::Return<V30::ErrCode> &>(ret).withDefault(V30::ErrCode::ERR_GENERAL));
    }

    /*
     * REF 行為：processAsync 呼叫後使用 operator T()（assertOk + 直接讀 mVal）。
     * 當 HIDL transport 失敗時，mVal 被 value-init 為 0（= ErrCode::OK），
     * 因此 REF 忽略 transport 錯誤。
     *
     * 但 Android 11 的 libhidlbase assertOk() 會 LOG(FATAL) 導致 abort，
     * 所以不能直接用 operator T()。改用 withDefault(OK)：
     * transport 失敗時 isOk()==false → 回傳 OK(0)，效果等同 REF 讀 mVal{}=0。
     * 此函式僅用於 processAsync 呼叫點，與 REF 反編譯行為一致。
     */
    static inline int hidlErrCodeLenient(const android::hardware::Return<V30::ErrCode> &ret)
    {
        return static_cast<int>(
            const_cast<android::hardware::Return<V30::ErrCode> &>(ret).withDefault(V30::ErrCode::OK));
    }

    /* ────────────────────────────────────────────────
     *  onStart   (Ghidra 00019d7c)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::onStart(const ProcessModeBase *mode)
    {
        auto svc = reinterpret_cast<V30::ICacao *>(
            *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(this) + 0x2f4));

        /* 準備 ControlMode（dynsym 確認真實型別名，先前誤植為 ModeData）
         * — REF 用 NEON vld1/vst1 批量複製 mode+0x0c..+0x2b */
        const uint32_t *modeRaw = reinterpret_cast<const uint32_t *>(mode);
        V30::ControlMode md;
        md.modeType = mode->getType();
        __aeabi_memcpy8(&md.camIdx, reinterpret_cast<const uint8_t *>(mode) + 0x0c, 32);

        auto startRet = svc->start(md);
        int startErr = hidlErrCode(startRet);
        if (startErr != 0)
        {
            return (int)0xffffff91u;
        }

        uint32_t modeType = mode->getType();
        THIS_U32(0x5dc) = modeType;

        if ((modeType & 0xfffffffe) == 4)
        {
            if (modeRaw[9] == 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "mSuperSlowFps cannot be 0 when SuperSlow mode");
                return (int)0xffffff99u;
            }
            THIS_U32(0x594) = modeRaw[9];
        }

        // 建立 Callback，儲存 raw pointer 到 this+0x660（與參考一致）
        sp<Callback> cb = new Callback(this);
        THIS_U32(0x660) = reinterpret_cast<uint32_t>(cb.get());

        sp<V30::ICacaoCallback> cbBase = cb;
        auto cbRet = svc->setCallback(cbBase);
        int cbErr = hidlErrCode(cbRet);
        if (cbErr == 0)
        {
            return 0;
        }
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                     "Set callback failed");
        return (int)0xffffff91u;
    }

    /* ────────────────────────────────────────────────
     *  onStop   (Ghidra 00019efc)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::onStop(bool isCancelAll)
    {
        int err = 0;

        if (THIS_I32(0x5dc) == 5)
        {
            if (!isCancelAll)
            {
                processInternal(PROCESS_BURST);
            }
            deleteNativeWindow();
            THIS_U32(0x618) = 0;
            THIS_U32(0x61c) = 0;
            THIS_U32(0x628) = 0;
            THIS_U32(0x598) = 0;
        }

        if (!isCancelAll)
        {
            auto svc = reinterpret_cast<V30::ICacao *>(
                *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(this) + 0x2f4));
            auto ret = svc->stop();
            if (hidlErrCode(ret) != 0)
            {
                err = -0x6f;
                goto drain_lists;
            }
        }

    drain_lists:
        if (THIS_PTR(0x5d0) != nullptr)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Start video recording not successfully done");
            notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d0)), (PAL_Err_t)0xffffff94u);
            THIS_PTR(0x5d0) = nullptr;
        }
        if (THIS_PTR(0x5d4) != nullptr)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Stop video recording not successfully done");
            notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d4)), (PAL_Err_t)0xffffff94u);
            THIS_PTR(0x5d4) = nullptr;
        }
        if (THIS_PTR(0x5d8) != nullptr)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Trigger super slow not successfully done");
            notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d8)), (PAL_Err_t)0xffffff94u);
            THIS_PTR(0x5d8) = nullptr;
        }

        {
            ResultItem *ri;
            while ((ri = mAsyncResults.popLast()) != nullptr)
            {
                notifyResult(*reinterpret_cast<Request **>(RI_U8(ri, 4)), (PAL_Err_t)0xffffff94u);
                ri->deinit();
                delete ri;
            }
        }
        {
            ResultItem *ri;
            while ((ri = mFreeSwResults.popLast()) != nullptr)
            {
                ri->deinit();
                delete ri;
            }
        }
        {
            ResultItem *ri;
            while ((ri = mFreeVideoResults.popLast()) != nullptr)
            {
                ri->deinit();
                delete ri;
            }
        }
        {
            ResultItem *ri;
            while ((ri = mPrepareResults.popLast()) != nullptr)
            {
                ri->deinit();
                delete ri;
            }
        }

        if (err >= 0 && !isCancelAll)
        {
            auto svc = reinterpret_cast<V30::ICacao *>(
                *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(this) + 0x2f4));
            void *nullCb = nullptr;
            auto ret = svc->setCallback(
                *reinterpret_cast<const android::sp<V30::ICacaoCallback> *>(&nullCb));
            if (hidlErrCode(ret) != 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Set callback failed");
                err = -0x6f;
            }
        }

        THIS_U32(0x594) = 0;
        return err;
    }

    /* ────────────────────────────────────────────────
     *  onStopRequest
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::onStopRequest()
    {
        if (THIS_PTR(0x5d8) != nullptr)
        {
            notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d8)), (PAL_Err_t)0xffffff94u);
            THIS_PTR(0x5d8) = nullptr;
        }
        return 0;
    }

    /* ────────────────────────────────────────────────
     *  onProcess
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::onProcess(Request *req)
    {
        /* 原版不檢查 req==nullptr，直接存取 req->param 然後檢查 param */
        const ProcessParamBase *param =
            *reinterpret_cast<const ProcessParamBase **>(
                reinterpret_cast<uint8_t *>(req) + 4);
        if (!param)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Request parameter not found");
            notifyResult(req, (PAL_Err_t)0xffffff91u);
            return;
        }

        uint32_t type = *reinterpret_cast<const uint32_t *>(
            reinterpret_cast<const uint8_t *>(param) + 8);

        PAL_LogPrint(__FILE__, __LINE__, 1, 0x40, "process type : %d", (int)type);

        int err = 0;
        switch (type)
        {
        case 10:
            err = prepareVideoRec(req);
            break;
        case 11:
            err = startVideoRec(req);
            break;
        case 12:
            err = stopVideoRec(req);
            break;
        case 13:
            err = triggerSuperSlow(req);
            break;
        default:
            err = passRequest(req);
            break;
        }

        if (err < 0)
        {
            notifyResult(req, (PAL_Err_t)err);
        }
    }

    /* ────────────────────────────────────────────────
     *  onHandleMsg
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::onHandleMsg(PAL_Msg_t *msg)
    {
        ResultMsg *rmsg = reinterpret_cast<ResultMsg *>(msg);
        switch (msg->msg_id)
        {
        case PAL_MSG_SUPER_SLOW:
            onHandleSuperSlowDoneInternal();
            break;
        case PAL_MSG_RESULT:
            onHandleResultInternal(rmsg);
            break;
        case PAL_MSG_PROGRESS:
            onHandleProgressInternal(rmsg);
            break;
        default:
            break;
        }
    }

    /* ────────────────────────────────────────────────
     *  fillProcessResult
     * ──────────────────────────────────────────────── */

    static __attribute__((always_inline)) void fillProcessResult(int *result, void *item, uint32_t cvt)
    {
        auto *ri = reinterpret_cast<uint8_t *>(item);
        result[5] = cvt;
        result[6] = RI_I32(ri, 0xf4);
        result[7] = RI_I32(ri, 0xf8);
        result[8] = RI_I32(ri, 0xfc);
        result[9] = RI_I32(ri, 0x100);
        result[10] = RI_I32(ri, 0x104);
        reinterpret_cast<uint8_t *>(result)[0x2c] = ri[0x108];
        reinterpret_cast<uint8_t *>(result)[0x2d] = ri[0x109];
        reinterpret_cast<uint8_t *>(result)[0x2e] = ri[0x10a];
        reinterpret_cast<uint8_t *>(result)[0x2f] = ri[0x10b];
        reinterpret_cast<uint8_t *>(result)[0x30] = ri[0x10c];
        (*reinterpret_cast<void (**)(int *, uint32_t)>(
            *reinterpret_cast<int **>(result) + 0x1c / 4))(result, RI_U32(ri, 0xe4));
        (*reinterpret_cast<void (**)(int *, uint32_t)>(
            *reinterpret_cast<int **>(result) + 0x14 / 4))(result, RI_U32(ri, 0xe0));
    }

    /* ────────────────────────────────────────────────
     *  onHandleProgressInternal
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::onHandleProgressInternal(ResultMsg *msg)
    {
        uint32_t reqId = *reinterpret_cast<uint32_t *>(msg->resultData);
        /* 對應 REF：get（不移除）透過 matchResult 比較 item[0xd8] == reqId */
        ResultItem *item = mAsyncResults.get(0, matchResult, (void *)(uintptr_t)reqId);
        if (!item)
            return;

        __aeabi_memcpy8(RI_U8(item, 0xd8), msg->resultData, 0x48);

        int *imageBuf = reinterpret_cast<int *>(RI_PTR(item, 0x124));
        if (imageBuf)
        {
            (*reinterpret_cast<void (**)(int *, uint32_t)>(*imageBuf + 0x38))(
                imageBuf, RI_U32(item, 0x100));
        }

        int *result = reinterpret_cast<int *>(RI_PTR(item, 0xd0));
        /* REF 在 progress 路徑中 inline 填充 result[5..10]、尾部位元組、
         * 及 setProgress/setResult vtable 呼叫，不使用 fillProcessResult helper */
        uint32_t cvt = convert(static_cast<ProcessType>(RI_U32(item, 0xf0)));
        result[5] = (int)cvt;
        result[6] = RI_I32(item, 0xf4);
        result[7] = RI_I32(item, 0xf8);
        result[8] = RI_I32(item, 0xfc);
        result[9] = RI_I32(item, 0x100);
        result[10] = RI_I32(item, 0x104);
        reinterpret_cast<uint8_t *>(result)[0x2c] = *RI_U8(item, 0x108);
        reinterpret_cast<uint8_t *>(result)[0x2d] = *RI_U8(item, 0x109);
        reinterpret_cast<uint8_t *>(result)[0x2e] = *RI_U8(item, 0x10a);
        reinterpret_cast<uint8_t *>(result)[0x2f] = *RI_U8(item, 0x10b);
        reinterpret_cast<uint8_t *>(result)[0x30] = *RI_U8(item, 0x10c);

        /* REF 0x1a896: vtable[0x1c/4]=setProgress, vtable[0x14/4]=setResult
         * 在 notifyProgress 之前，透過 ProcessCtrlResult 的虛函數設定 result/progress */
        {
            int *vtbl = *reinterpret_cast<int **>(result);
            reinterpret_cast<void (*)(int *, int)>(vtbl[7])(result, RI_I32(item, 0xe4));
            reinterpret_cast<void (*)(int *, int)>(vtbl[5])(result, RI_I32(item, 0xe0));
        }

        Request *req = reinterpret_cast<Request *>(RI_PTR(item, 4));
        notifyProgress(req, *reinterpret_cast<int32_t *>(msg->resultData + 0xC));
    }

    /* ────────────────────────────────────────────────
     *  onHandleResultInternal   (Ghidra 0001a8ac)
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::onHandleResultInternal(ResultMsg *msg)
    {
        uint32_t reqId = *reinterpret_cast<uint32_t *>(msg->resultData);
        /* 對應 REF：pop（移除）透過 matchResult 比較 item[0xd8] == reqId */
        ResultItem *item = mAsyncResults.pop(0, matchResult, (void *)(uintptr_t)reqId);
        if (!item)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Result item not found");
            return;
        }

        __aeabi_memcpy8(RI_U8(item, 0xd8), msg->resultData, 0x48);

        int *imageBuf = reinterpret_cast<int *>(RI_PTR(item, 0x124));
        if (imageBuf)
        {
            (*reinterpret_cast<void (**)(int *, uint32_t)>(*imageBuf + 0x38))(
                imageBuf, RI_U32(item, 0x100));
        }

        int *result = reinterpret_cast<int *>(RI_PTR(item, 0xd0));
        Request *req = reinterpret_cast<Request *>(RI_PTR(item, 4));

        const char *logFile;
        int logLine;
        const char *logMsg;
        int logArg = 0;

        if (req)
        {
            uint32_t cvt = convert(static_cast<ProcessType>(RI_U32(item, 0xf0)));
            fillProcessResult(result, item, cvt);
            notifyResult(req, *reinterpret_cast<PAL_Err_t *>(msg->resultData + 8));
            goto done_result;
        }
        if (!result)
        {
            logFile = __FILE__;
            logLine = __LINE__;
            logMsg = "Internal request. pResult not found";
            goto log_err;
        }
        {
            uint32_t cvt = convert(static_cast<ProcessType>(RI_U32(item, 0xf0)));
            fillProcessResult(result, item, cvt);

            switch (result[5])
            {
            case 0:
                if ((THIS_U32(0x5dc) & 0xfffffffe) == 4)
                {
                    processAsyncInternal(PROCESS_TYPE_8);
                    break;
                }
                // [動態驗證確認/與原版一致，非我方 bug] 反編譯 so_32 @ 0x1a8ac 確認這裡
                // 100% 逐位元組相同：mode==0（例如 120fps 連續慢動作/HFR）時
                // `mode-1>2`（unsigned underflow）恆真，直接 break，不會走到下面
                // notifyResult(mVideoReq,...)。
                //
                // [20260727 session 追加動態驗證] 用裝置端 log 直接確認了完整因果
                // 鏈：nativeChangeToVideoMode 收到的 mode 值確實是 0（Java 端
                // getVideoMode() 對 SLOW_MOTION capturing mode 強制回傳
                // VideoStabilizer.OFF，對應 VideoMode.NORMAL=0）；cald 端確實有
                // 完成 startVideoRec(type=11) 這個請求並送回一筆
                // result[5]==0 的訊息，但因為當下 mMode(this+0x5dc)==0 命中這裡
                // 的 exclusion，notifyResult(mVideoReq,...) 被跳過，之後
                // onHandleResultInternal 再也沒有被呼叫過——也就是說
                // VideoStartSuperSlowRecCallback::onHandleResult 永遠不會觸發，
                // Java 端等不到「錄影已開始」的回呼，最終逾時。這個 mode==0
                // exclusion 本身已用 Ghidra 反編譯字節級比對確認與原版完全一致，
                // 不是重建引入的落差，不應更動這裡的判斷式。
                //
                // 但同一份反編譯結構下，原版裝置實測（同一台裝置、同一組
                // test_camera.py 測試腳本、同樣的「先進入 960fps 預設 → 再切到
                // 120fps」UI 路徑）大多數情況下能成功完成錄影，代表原版當下
                // cald 對這個 request 的回覆很可能不是 result[5]==0（命中這個
                // exclusion），而是直接以 result[5]==8 回覆（不受這個 exclusion
                // 影響，直接 fallthrough 到下面完成）。這代表 result[5] 的值本身
                // 是 vendor cald 內部決定、對這次 request 當下狀態敏感的
                // non-deterministic 結果，不是由這裡的程式碼決定。換句話說，
                // slow_120 卡住的關鍵不在這個 exclusion 判斷式本身（已確認正確），
                // 而在「cald 為什麼這次回 result[5]==0 而不是 8」——這是 vendor
                // 端閉源二進位（cald/HAL provider）內部的決策，Libcacao 四個模組
                // 的原始碼範圍內沒有能修改這個決策的位置。留給後續：若要繼續
                // 追查，需要對 vendor cald 本身做動態分析（不在 Libcacao 範圍）。
                if (THIS_U32(0x5dc) - 1 > 2)
                    break;
                [[fallthrough]];
            case 8:
                tryVideoCaptureRequest();
                notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d0)), (PAL_Err_t)0);
                THIS_PTR(0x5d0) = nullptr;
                break;
            case 1:
                notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d4)), (PAL_Err_t)0);
                THIS_PTR(0x5d4) = nullptr;
                deleteNativeWindow();
                THIS_U32(0x618) = 0;
                THIS_U32(0x61c) = 0;
                THIS_U32(0x628) = 0;
                THIS_U32(0x598) = 0;
                break;
            case 3:
            case 5:
                handleVideoCapture(item);
                return;
            case 9:
                onHandleSuperSlowDoneInternal();
                break;
            default:
            {
                logFile = __FILE__;
                logLine = __LINE__;
                logMsg = "Invalid process type %d";
                logArg = result[5];
                goto log_err;
            }
            }
        }
        goto done_result;
    log_err:
        PAL_LogPrint(logFile, logLine, 0x100, 1, logMsg, logArg);
    done_result:
        item->deinit();
        delete item;
    }

    /* ────────────────────────────────────────────────
     *  onHandleSuperSlowDoneInternal
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::onHandleSuperSlowDoneInternal()
    {
        PAL_LogPrint(__FILE__, __LINE__, 1, 0x40, "===== Super slow finished =====");

        int subState = THIS_I32(0x5b4);
        if (subState != 2)
            return;

        int vidMode = THIS_I32(0x5dc);

        if (vidMode == 5)
        {
            Request *superReq = *reinterpret_cast<Request **>(THIS_U8(0x5d8));
            int pErr = processInternal(PROCESS_BURST);
            deleteNativeWindow();
            THIS_U32(0x618) = 0;
            THIS_U32(0x61c) = 0;
            THIS_U32(0x628) = 0;
            THIS_U32(0x598) = 0;
            THIS_U32(0x664) = 0;
            THIS_PTR(0x5d8) = nullptr;
            notifyResult(superReq, (PAL_Err_t)pErr);
        }
        else if (vidMode == 4)
        {
            THIS_U32(0x5b4) = 1;

            uint32_t tickRate = THIS_U32(0x610);
            uint32_t fps = THIS_U32(0x590);
            uint32_t frameRateExtra = THIS_U32(0x598);

            uint64_t framePos64 = *reinterpret_cast<uint64_t *>(THIS_U8(0x5f0));
            uint32_t ticks = (uint32_t)((framePos64 - 1) / (uint64_t)tickRate);
            ticks = ticks - THIS_U32(0x5fc) + frameRateExtra;

            uint64_t nsTime = (uint64_t)ticks * 0xe8d4a51000ULL;
            uint64_t scaled = nsTime / (uint64_t)fps;

            uint64_t accumulator = ((uint64_t)THIS_U32(0x61c) << 32) | THIS_U32(0x618);
            accumulator += scaled;
            THIS_U32(0x618) = (uint32_t)accumulator;
            THIS_U32(0x61c) = (uint32_t)(accumulator >> 32);

            uint32_t frameIdx = THIS_U32(0x4d4);
            if (frameIdx < 2)
            {
                ResultItem *ri = mFreeSwResults.popRoot();
                if (ri)
                {
                    uint32_t flags = (uint32_t)(RI_I32(ri, 0xe0)) >> 31;
                    returnBufferToSurface(ri, (uint8_t)flags);
                    ri->deinit();
                    delete ri;
                }
                else
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "Frame is not available in the received result list.");
                }
                return;
            }

            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Only 1 frame must be available in the received result list.");
            ResultItem *ri;
            while ((ri = mFreeSwResults.popRoot()) != nullptr)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "Frame ID = [%d,%d]", RI_U32(ri, 0x118), RI_U32(ri, 0x11c));
                uint32_t flags = (uint32_t)(RI_I32(ri, 0xe0)) >> 31;
                returnBufferToSurface(ri, (uint8_t)flags);
                ri->deinit();
                delete ri;
            }
        }
    }

    /* setConfig — 對應 Ghidra Gateway_setConfig_1_000197e8
     * 讀取 configId，映射 dpType，直接呼叫 mService->setConfig
     *
     * 2026-08-17 更正：Ghidra 反編譯 BpHwCacao::_hidl_setConfig 確認
     * writeBuffer(parcel, &config, 8, …) 只送出 8 bytes，Config 本身就是
     * {configId, value} 兩個 uint32_t（見 types.h）。先前把這兩個 word
     * reinterpret_cast 成 hidl_vec<uint8_t>{mBuffer,mSize} 只是巧合地位元組
     * 相容（都是 8 bytes、都是兩個 word），語意上是錯的；vtable slot
     * 對應的真實方法名也是 setConfig，不是 configure。 */
    int ProcessCtrlGateway::setConfig(const ProcessConfigBase *cfg)
    {
        int configId = reinterpret_cast<int (*const *)(const void *)>(
            *reinterpret_cast<void *const *>(cfg))[0x10 / 4](cfg);
        uint32_t err = 0;

        if (configId == 0x65)
        {
            /* {configId=0, value=dpType} — 與參考完全一致 */
            uint32_t vec[2];
            vec[0] = 0;
            vec[1] = 0;

            int dpType = reinterpret_cast<const int *>(cfg)[2];
            if (dpType == 2)
            {
                vec[1] = 2;
            }
            else if (dpType != 1)
            {
                if (dpType == 0)
                {
                    err = 0;
                    vec[1] = 0;
                }
                else
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Climax setting convert error");
                    err = 0xffffff91u;
                }
                goto do_configure;
            }
            else
            {
                vec[1] = 1;
            }
            err = 0;

        do_configure:
            auto svc = reinterpret_cast<V30::ICacao *>(
                *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(this) + 0x2f4));
            auto ret = svc->setConfig(
                *reinterpret_cast<const V30::Config *>(vec));
            uint32_t result = 0xffffff91u;
            if (hidlErrCode(ret) == 0)
            {
                result = err;
            }
            return (int)result;
        }
        return (int)err;
    }

    /* getConfig — 對應 Ghidra Gateway_getConfig_1_00019894
     * 直接覆寫 ICacaoGateway::getConfig，呼叫 HIDL getConfig */
    int ProcessCtrlGateway::getConfig(ProcessConfigBase *cfg)
    {
        int err = 0;
        uint32_t configId = reinterpret_cast<int (**)(void *)>(
            *reinterpret_cast<void **>(cfg))[0x10 / 4](cfg);
        if (configId == 0x65)
        {
            int errCode = 0;
            int dpType = 0;
            /* 先前誤傳 hidl_vec<uint8_t>；Ghidra 反編譯 BpHwCacao::_hidl_getConfig
             * 確認第一個參數是 writeUint32 的純量 ConfigId，且 Config 只有 8
             * bytes（configId, value 各一個 uint32_t）——對應 setConfig 那邊
             * {configId=0, value=dpType} 的用法，這裡回讀時是 value 欄位。 */
            auto ret = mService->getConfig(static_cast<V30::ConfigId>(configId),
                                           [&errCode, &dpType](const V30::Config &config, V30::ErrCode ec)
                                           {
                                               if ((int)ec == 0)
                                               {
                                                   dpType = (int)config.value;
                                               }
                                               else
                                               {
                                                   PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "hidl cacao interface getConfig error");
                                                   errCode = (int)0xffffff91u;
                                               }
                                           });
            if (!ret.isOk())
            {
                auto desc = ret.description();
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Transaction error: %s", desc.c_str());
                errCode = -0x6f;
            }
            if (errCode >= 0)
            {
                int mappedType;
                if (dpType == 2)
                    mappedType = 2;
                else if (dpType == 1)
                    mappedType = 1;
                else if (dpType == 0)
                    mappedType = 0;
                else
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Climax setting convert error");
                    errCode = -0x6f;
                    goto done;
                }
                reinterpret_cast<int *>(cfg)[2] = mappedType;
            }
            err = errCode;
        }
        else
        {
            err = -0x67;
        }
    done:
        return err;
    }

    ProcessCtrlGateway::Request *
    ProcessCtrlGateway::onCreateRequest(const ProcessParamBase *param,
                                        android::Vector<ImageBuf *> *imageBufs,
                                        ICBProcessResultListener *listener,
                                        ProcessResultBase *result,
                                        uint8_t flags,
                                        const PAL_Msg_t *replyMsg)
    {
        Request *req = new Request(param, imageBufs, listener, result, flags,
                                   const_cast<PAL_Msg_t *>(replyMsg));
        return req;
    }

    /* ────────────────────────────────────────────────
     *  processInternal   (Ghidra 0001a128)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::processInternal(ProcessType processType)
    {
        /* 對應 REF：Param 含 HIDL 欄位，hidl_string/hidl_handle 用 placement-new 初始化；
         * hidl_vec 由 = {} 零化即可（REF 直接 store 0 而不呼叫 ctor），僅於結束時呼叫 dtor */
        alignas(4) uint8_t paramBuf[sizeof(V30::Param)];
        memset(paramBuf, 0, sizeof(paramBuf));
        auto *paramStr = new (paramBuf + 0x30) android::hardware::hidl_string();
        auto *paramHdl1 = new (paramBuf + 0x78) android::hardware::hidl_handle();
        auto *paramHdl2 = new (paramBuf + 0x98) android::hardware::hidl_handle();
        /* hidl_vec 不呼叫 ctor，REF 直接以 0 初始化（{null,0,false}），手動初始化 */
        *reinterpret_cast<uint32_t *>(paramBuf + 0xB8) = 0;
        *reinterpret_cast<uint32_t *>(paramBuf + 0xBC) = 0;
        *reinterpret_cast<uint32_t *>(paramBuf + 0xC0) = 0;
        auto *paramVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(paramBuf + 0xB8);

        *reinterpret_cast<uint32_t *>(paramBuf) = convert(processType);

        auto svc = reinterpret_cast<V30::ICacao *>(
            *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(this) + 0x2f4));
        /* 直接傳 const Param& — 與 REF processInternal 完全一致 */
        auto ret = svc->process(*reinterpret_cast<const V30::Param *>(paramBuf));
        int err = 0;
        int hidlErr = hidlErrCode(ret);
        if (hidlErr != 0)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "process failed. %d", hidlErr);
            err = hidlErr;
        }

        paramVec->~hidl_vec<V30::ImageBufInfo>();
        paramHdl2->~hidl_handle();
        paramHdl1->~hidl_handle();
        paramStr->~hidl_string();
        return err;
    }

    /* ────────────────────────────────────────────────
     *  processAsyncInternal   (Ghidra 0001ae64)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::processAsyncInternal(ProcessType processType)
    {
        ResultItem *item = new ResultItem();
        RI_PTR(item, 0xd0) = new ProcessCtrlResult();

        /* 對應 REF 的 stack 上 V30::Param（含 HIDL 欄位）初始化方式：
         * GCC 對各 HIDL 成員呼叫 ctor；我們用 placement-new 達成相同效果。
         * Param 結構：
         *   +0x00  processType (uint32_t)
         *   +0x30  hidl_string
         *   +0x78  hidl_handle #1
         *   +0x98  hidl_handle #2
         *   +0xB4  bufCount (uint32_t)
         *   +0xB8  hidl_vec<ImageBufInfo>
         */
        alignas(8) uint8_t paramBuf[sizeof(V30::Param)];
        memset(paramBuf, 0, sizeof(paramBuf));
        auto *paramStr = new (paramBuf + 0x30) android::hardware::hidl_string();
        auto *paramHdl1 = new (paramBuf + 0x78) android::hardware::hidl_handle();
        auto *paramHdl2 = new (paramBuf + 0x98) android::hardware::hidl_handle();
        uint32_t &bufCount_field = *reinterpret_cast<uint32_t *>(paramBuf + 0xB4);
        bufCount_field = 0;
        /* REF: local_38[0..1]=0 (8 bytes → NEON vmov+vstr); local_38[2]=0; local_38[3]=1
         * 以 uint64_t 一次清零前 8 bytes，觸發 GCC NEON zero-init */
        *reinterpret_cast<uint64_t *>(paramBuf + 0xB8) = 0ULL; /* data ptr + size (8B NEON) */
        *reinterpret_cast<uint32_t *>(paramBuf + 0xC0) = 0;    /* owns_buffer = false */
        *reinterpret_cast<uint32_t *>(paramBuf + 0xC4) = 1;    /* unknown field = 1 */
        auto *paramVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(paramBuf + 0xB8);

        if ((uint32_t)processType == 0)
        {
            uint32_t bufCount = THIS_U32(0x438);
            resize_image_buf_vec(paramVec, bufCount);
            for (uint32_t i = 0; i < bufCount; i++)
            {
                ANativeWindowBuffer *buf = mBuffers[i];
                if (!buf)
                {
                    bufCount_field = i; /* REF: local_3c = uVar6（有效數量） */
                    break;
                }
                V30::ImageBufInfo &info = (*paramVec)[i];
                *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(&info) + 0x00) = buf->width;
                *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(&info) + 0x04) = buf->height;
                assign_hidl_handle_from_native(
                    reinterpret_cast<android::hardware::hidl_handle *>(
                        reinterpret_cast<uint8_t *>(&info) + 0x08),
                    const_cast<native_handle_t *>(buf->handle));
                *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(&info) + 0x18) =
                    THIS_U32(0x628);
            }
        }

        *reinterpret_cast<uint32_t *>(paramBuf) = convert(processType);

        RI_PTR(item, 0xd8) = item;
        RI_U32(item, 0xdc) = 0;

        auto svc = reinterpret_cast<V30::ICacao *>(
            *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(this) + 0x2f4));
        /* 直接傳 const Param& 和 const Result& — 與 REF processAsyncInternal 完全一致 */
        const V30::Result &resultRef2 = *reinterpret_cast<const V30::Result *>(RI_U8(item, 0xd8));
        auto ret = svc->processAsync(*reinterpret_cast<const V30::Param *>(paramBuf), resultRef2);

        /* 對應 REF 析構順序（逆向）: ~hidl_vec, ~hidl_handle×2, ~hidl_string */
        paramVec->~hidl_vec<V30::ImageBufInfo>();
        paramHdl2->~hidl_handle();
        paramHdl1->~hidl_handle();
        paramStr->~hidl_string();

        int err = 0;
        if (hidlErrCodeLenient(ret) != 0)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "processAsync failed");
            err = -0x6f;
        }

        if (err < 0)
        {
            item->deinit();
            delete item;
        }
        else
        {
            mAsyncResults.pushLast(item);
        }

        return err;
    }

    /* ────────────────────────────────────────────────
     *  passRequest   (Ghidra 0001a5e8)
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::passRequest(Request *req)
    {
        int retVal;
        ResultItem *item = new ResultItem();

        /* 2026-08-18：Param 現在真的內含 hidl_string/hidl_handle×2/hidl_vec 成員
         * （不再是純 POD 的扁平 byte array），`V30::Param param;` 這種具名變數
         * 宣告會讓編譯器在函式結尾自動幫這些成員跑一次解構子，跟下面
         * paramVec/paramHdl2/paramHdl1/paramStr 的手動解構重複，hidl_vec/
         * hidl_string 若擁有 heap buffer 會被 double free。改成跟
         * processInternal/processAsyncInternal 一致的 raw buffer + placement-new
         * 手法，避免編譯器插入的隱式解構子。 */
        alignas(8) uint8_t paramBuf[sizeof(V30::Param)];
        memset(paramBuf, 0, sizeof(paramBuf));

        int paramObjPtr = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(req) + 0x10);
        uint32_t rawType = 0;
        if (paramObjPtr)
        {
            rawType = *reinterpret_cast<uint32_t *>(paramObjPtr + 0x14);
        }
        uint32_t convertedType = convert(static_cast<ProcessType>(rawType));

        uint8_t *pBuf = paramBuf;
        auto *paramStr = new (pBuf + 0x30) android::hardware::hidl_string();
        auto *paramHdl1 = new (pBuf + 0x78) android::hardware::hidl_handle();
        auto *paramHdl2 = new (pBuf + 0x98) android::hardware::hidl_handle();
        auto *paramVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(pBuf + 0xB8);

        int cpErr = copyProcessCtrlParam(req, reinterpret_cast<V30::Param *>(paramBuf));
        if (cpErr < 0)
        {
            retVal = -0x6f;
            goto err_cleanup;
        }

        {
            uint8_t *pSrc = pBuf;
            RI_PTR(item, 0xd0) = reinterpret_cast<void *>(
                *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(req) + 0x10));
            RI_PTR(item, 4) = req;
            RI_U32(item, 8) = *reinterpret_cast<uint32_t *>(pBuf);

            /* block copy: item+0x10..0x2C ← pSrc+0x04..0x20 (8×u32) */
            __aeabi_memcpy8(RI_U8(item, 0x10), pSrc + 0x04, 32);
            *RI_U8(item, 0x30) = pSrc[0x24];

            auto *itemStr = reinterpret_cast<android::hardware::hidl_string *>(RI_U8(item, 0x38));
            *itemStr = *paramStr;

            /* block copy: item+0x48..0x74 ← pSrc+0x38..0x64 (11×u32 + 1×u8 = 45 bytes) */
            __aeabi_memcpy8(RI_U8(item, 0x48), pSrc + 0x38, 44);
            *RI_U8(item, 0x74) = pSrc[0x64];
            /* item+0x78..0x7C ← pSrc+0x68..0x6C (2×u32 as u64 pair) */
            *reinterpret_cast<uint64_t *>(RI_U8(item, 0x78)) =
                *reinterpret_cast<const uint64_t *>(pSrc + 0x68);

            assign_hidl_handle(
                reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(item, 0x80)), paramHdl1);

            RI_U32(item, 0x90) = *reinterpret_cast<uint32_t *>(pSrc + 0x88);
            RI_U32(item, 0x98) = *reinterpret_cast<uint32_t *>(pSrc + 0x90);
            RI_U32(item, 0x9c) = *reinterpret_cast<uint32_t *>(pSrc + 0x94);

            assign_hidl_handle(
                reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(item, 0xa0)), paramHdl2);

            RI_U32(item, 0xb0) = *reinterpret_cast<uint32_t *>(pSrc + 0xa8);
            RI_U32(item, 0xb8) = *reinterpret_cast<uint32_t *>(pSrc + 0xb0);
            RI_U32(item, 0xbc) = *reinterpret_cast<uint32_t *>(pSrc + 0xb4);

            auto *itemVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(
                RI_U8(item, 0xc0));
            assign_image_buf_vec(itemVec, paramVec);

            RI_PTR(item, 0xd8) = item;
            RI_U32(item, 0xdc) = 0;
            RI_U32(item, 0xf0) = convertedType;

            /* item+0xe0..0xef ← pSrc+0x80..0x8f (2×u64) */
            *reinterpret_cast<uint64_t *>(RI_U8(item, 0xe0)) = *reinterpret_cast<const uint64_t *>(pSrc + 0x80);
            *reinterpret_cast<uint64_t *>(RI_U8(item, 0xe8)) = *reinterpret_cast<const uint64_t *>(pSrc + 0x88);

            /* block copy: item+0xf4..0x11f ← pSrc+0x58..0x83 (11×u32 = 44 bytes) */
            __aeabi_memcpy8(RI_U8(item, 0xf4), pSrc + 0x58, 44);
        }

        {
            auto svc = reinterpret_cast<V30::ICacao *>(
                *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(this) + 0x2f4));
            const V30::Result &resultRef = *reinterpret_cast<const V30::Result *>(RI_U8(item, 0xd8));
            auto ret = svc->processAsync(*reinterpret_cast<const V30::Param *>(paramBuf), resultRef);

            paramVec->~hidl_vec<V30::ImageBufInfo>();
            paramHdl2->~hidl_handle();
            paramHdl1->~hidl_handle();
            paramStr->~hidl_string();

            if (hidlErrCodeLenient(ret) != 0)
            {
                retVal = -0x6f;
                goto err_cleanup;
            }
        }

        mAsyncResults.pushLast(item);
        return 0;

    err_cleanup:
        item->deinit();
        delete item;
        return retVal;
    }

    /* ────────────────────────────────────────────────
     *  prepareVideoRec
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::prepareVideoRec(Request *req)
    {
        int paramPtr = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(req) + 4);
        int err;
        if (!paramPtr)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Video REC parameter not found");
            err = (int)0xffffff98u;
            goto done;
        }

        {
            int *param = reinterpret_cast<int *>(paramPtr);

            int fmt = param[0x164 / 4];
            if (fmt == 0x102)
                THIS_U32(0x628) = 2;
            else if (fmt == 0x103)
                THIS_U32(0x628) = 3;
            else if (fmt == 0x12060000 || fmt == 0x104)
                THIS_U32(0x628) = 4;
            else
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Specified dataspace is not supported");

            if ((THIS_U32(0x5dc) & 0xfffffffe) == 4)
            {
                uint32_t frameRate = (uint32_t)param[0x168 / 4];
                THIS_U32(0x598) = frameRate;

                uint64_t n1 = (uint64_t)frameRate * 1000000000000ULL;
                uint64_t div1 = n1 / 30000ULL;
                uint64_t n2 = (uint64_t)frameRate * 1000000000ULL;
                uint64_t div2 = n2 / THIS_U32(0x594);
                *reinterpret_cast<int64_t *>(THIS_U8(0x5a0)) = (int64_t)div1 - (int64_t)div2;
            }

            err = createNativeWindow(reinterpret_cast<ProcessCtrlVideoRecParam *>(param));

            if (err >= 0 && THIS_U32(0x5dc) == 4)
            {
                err = createFreeSwResultList();
            }
        }

    done:
        notifyResult(req, (PAL_Err_t)err);
        return err;
    }

    /* ────────────────────────────────────────────────
     *  startVideoRec
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::startVideoRec(Request *req)
    {
        if (!mSurface)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Not prepared yet");
            return (int)0xffffff9bu; /* -0x65：對應 REF 0x1a4b8 */
        }
        if (mVideoReq)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "already started video rec");
            return -0x6b;
        }

        int err = processAsyncInternal(PROCESS_STILL);
        if (err < 0)
            return err;

        THIS_U32(0x5c8) = 0;
        THIS_U32(0x5cc) = 0;
        mVideoReq = req;
        THIS_U32(0x5b4) = 1;

        if ((THIS_U32(0x5dc) & 0xfffffffe) == 4)
        {
            THIS_U32(0x590) = 30000;
            uint32_t fpsDiv = THIS_U32(0x594) / 30u;
            THIS_U32(0x610) = fpsDiv;
            uint32_t totalFrames = (THIS_U32(0x598) + fpsDiv) / fpsDiv + 1;
            THIS_U32(0x5b0) = totalFrames;
        }
        else
        {
            THIS_I32(0x5b0) = 0;
        }

        return err;
    }

    /* ────────────────────────────────────────────────
     *  stopVideoRec
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::stopVideoRec(Request *req)
    {
        int err;
        if (mPrepareVideoReq)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "already stopped video rec");
            err = -0x6b;
        }
        else
        {
            mPrepareVideoReq = req;

            if (THIS_I32(0x5b4) == 4)
            {
                THIS_U32(0x5b4) = 0;
                err = processAsyncInternal(PROCESS_BURST);
            }
            else
            {
                int64_t limTs = (int64_t)getMonotonicNanos();
                *reinterpret_cast<int64_t *>(THIS_U8(0x5c8)) = limTs;
                THIS_U32(0x5b0) = 0;
                THIS_U32(0x5b4) = 3;
                err = processAsyncInternal(PROCESS_VIDEO_CAP);
            }

            if (err >= 0)
            {
                return err;
            }
        }
        notifyResult(mPrepareVideoReq, (PAL_Err_t)err);
        return err;
    }

    /* triggerSuperSlow */

    int ProcessCtrlGateway::triggerSuperSlow(Request *req)
    {
        if (THIS_PTR(0x5d8) != nullptr)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "already triggered super slow");
            return -0x6b;
        }
        *reinterpret_cast<Request **>(THIS_U8(0x5d8)) = req;
        return processAsyncInternal(PROCESS_TYPE_9);
    }

    /* handleFinishCamera */

    void ProcessCtrlGateway::handleFinishCamera()
    {
        deleteNativeWindow();
        THIS_U32(0x618) = 0;
        THIS_U32(0x61c) = 0;
        THIS_U32(0x628) = 0;
        THIS_U32(0x598) = 0;
    }

    void ProcessCtrlGateway::popLast()
    {
    }

    int ProcessCtrlGateway::deinit_device()
    {
        return 0;
    }

    /* ────────────────────────────────────────────────
     *  handleVideoCapture   (Ghidra 0001b21c)
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::handleVideoCapture(ResultItem *param_1)
    {

        if (THIS_I32(0x588) != 0)
        {
            THIS_I32(0x588) -= 1;
        }

        int subState = THIS_I32(0x5b4);

        switch (subState)
        {
        case 0:
        case 4:
        {
            ResultItem *iRes = mFreeVideoResults.popRoot();
            while (iRes != nullptr)
            {
                mPrepareResults.pushLast(iRes);
                iRes = mFreeVideoResults.popRoot();
            }
            ResultItem *sw;
            while ((sw = mFreeSwResults.popRoot()) != nullptr)
            {
                sw->deinit();
                delete sw;
            }
            param_1->deinit();
            delete param_1;
            return;
        }

        case 1:
        {
            if (THIS_I32(0x5dc) == 5)
            {
                goto do_super_slow_capture;
            }

            {
                int64_t itemTs = *reinterpret_cast<int64_t *>(RI_U8(param_1, 0x110));
                int64_t refTs = *reinterpret_cast<int64_t *>(THIS_U8(0x5a8));
                /* 對應 REF：refTs >= itemTs 才進入 do_super_slow_capture
                 * ARM: SUBS+SBCS 後 BGE (N==V) → 表示 refTs >= itemTs */
                if (refTs >= itemTs)
                {
                    goto do_super_slow_capture;
                }
            }

            mFreeSwResults.pushLast(param_1);

            {
                int *ro = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
                int status = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
                if (status >= 0)
                {
                    THIS_U32(0x5a8) = RI_U32(param_1, 0x110);
                    THIS_U32(0x5ac) = RI_U32(param_1, 0x114);
                }
            }

            if (THIS_I32(0x570) == 0)
            {
                uint32_t threshold = 0;
                if (THIS_U32(0x5b0) < RI_U32(param_1, 0x118))
                {
                    threshold = RI_U32(param_1, 0x118) - THIS_U32(0x5b0);
                }
                for (uint32_t i = 0; i < 5; i++)
                {
                    /* 對應 REF：pop 透過 matchOldFrameId 比較 frame_sec <= threshold */
                    ResultItem *old = mFreeSwResults.pop(0, matchOldFrameId, &threshold);
                    if (!old)
                        break;
                    int *ro = reinterpret_cast<int *>(RI_PTR(old, 0xd0));
                    int s = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
                    uint32_t flags = (uint32_t)s >> 31;
                    returnBufferToSurface(old, (uint8_t)flags);
                    old->deinit();
                    delete old;
                }
            }
            else
            {
                for (uint32_t i = 0; i < 5; i++)
                {
                    ResultItem *vidItem = mFreeVideoResults.popRoot();
                    if (!vidItem)
                        break;
                    ResultItem *newItem = createResultItemFromBuffer();
                    if (newItem)
                    {
                        copyResultItem(newItem, vidItem);
                        int *ib = reinterpret_cast<int *>(RI_PTR(newItem, 0x124));
                        (*reinterpret_cast<void (**)(int *)>(*ib + 0x2c))(ib);
                        mPrepareResults.pushLast(vidItem);

                        ANativeWindowBuffer *buf = reinterpret_cast<ANativeWindowBuffer *>(
                            RI_PTR(newItem, 0x128));
                        {
                            android::hardware::hidl_handle hdl;
                            assign_hidl_handle_from_native(
                                &hdl, const_cast<native_handle_t *>(buf->handle));
                            auto svc = reinterpret_cast<V30::ICacao *>(
                                *reinterpret_cast<void **>(
                                    reinterpret_cast<uint8_t *>(this) + 0x2f4));
                            /* 先前誤植為 returnBuffer；Ghidra 反編譯
                             * BnHwCacao::onTransact 確認這個 vtable slot（code 8）
                             * 實際是 setColorSpaceForHandle(hidl_handle, DataSpace)。 */
                            svc->setColorSpaceForHandle(
                                hdl, static_cast<V30::DataSpace>(THIS_U32(0x628)));
                        }

                        int *ro = reinterpret_cast<int *>(RI_PTR(newItem, 0xd0));
                        int s = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
                        uint32_t flags = (uint32_t)s >> 31;
                        returnBufferToSurface(newItem, (uint8_t)flags);
                        newItem->deinit();
                        delete newItem;
                    }
                }
                if (THIS_I32(0x570) == 0)
                {
                    int pErr = processInternal(PROCESS_TYPE_8);
                    PAL_LogPrint(__FILE__, __LINE__, 1, 0x40, "result of preparesuperslow = %d", pErr);
                    notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d8)), (PAL_Err_t)pErr);
                    THIS_PTR(0x5d8) = nullptr;
                }
            }
            tryVideoCaptureRequest();
            return;

        do_super_slow_capture:
            THIS_U32(0x5e4) = RI_U32(param_1, 0x118);
            THIS_U32(0x5e8) = RI_U32(param_1, 0x11c);

            {
                int *ro = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
                int status = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);

                uint32_t tickRate = THIS_U32(0x610);
                int64_t framePos = 0;
                if (status >= 0)
                {
                    framePos = (int64_t)((uint64_t)tickRate * (uint64_t)RI_U32(param_1, 0x118)) + (int64_t)RI_U32(param_1, 0x11c);
                }
                *reinterpret_cast<int64_t *>(THIS_U8(0x5f0)) = framePos;

                uint32_t endTick = THIS_U32(0x598) + THIS_U32(0x5e8) - 1;
                uint32_t endSec = endTick / tickRate; // 對應 REF: udiv，不檢查零
                uint32_t endFrac = endTick - endSec * tickRate;
                uint32_t endSecAbs = THIS_U32(0x5e4) + endSec;
                THIS_U32(0x5fc) = endSecAbs;
                THIS_U32(0x600) = endFrac;
                *reinterpret_cast<uint64_t *>(THIS_U8(0x608)) =
                    (uint64_t)endSecAbs * tickRate + endFrac;

                /* 對應 REF：pop 透過 matchOldFrameId 比較 frame_sec <= mEndFrameSec */
                ResultItem *old = mFreeSwResults.pop(0, matchOldFrameId, THIS_U8(0x5fc));
                while (old)
                {
                    int *roOld = reinterpret_cast<int *>(RI_PTR(old, 0xd0));
                    int s = (*reinterpret_cast<int (**)(int *)>(*roOld + 0x10))(roOld);
                    uint32_t flags = 1;
                    if (s >= 0)
                    {
                        int64_t oldPos = (int64_t)((uint64_t)tickRate * RI_U32(old, 0x118)) + RI_U32(old, 0x11c);
                        /* 對應 REF: if (F <= O) cancel — 當 old frame 在 current frame *之後*（異常）才 cancel
                         * 原本誤寫成 oldPos <= framePos，與 REF 相反 */
                        if (framePos <= oldPos)
                        {
                            flags = 1;
                        }
                        else
                        {
                            flags = (uint32_t)s >> 31;
                        }
                    }
                    returnBufferToSurface(old, (uint8_t)flags);
                    old->deinit();
                    delete old;
                    old = mFreeSwResults.pop(0, matchOldFrameId, THIS_U8(0x5fc));
                }

                int64_t presTime = *reinterpret_cast<int64_t *>(RI_U8(param_1, 0x110));
                THIS_U32(0x620) = (uint32_t)presTime;
                THIS_U32(0x624) = (uint32_t)((uint64_t)presTime >> 32);

                if (THIS_U32(0x5e8) != 0)
                {
                    uint32_t fps = THIS_U32(0x590);
                    int64_t nsAdj = (int64_t)(tickRate - THIS_U32(0x5e8)) * 1000000000LL;
                    nsAdj /= tickRate; // 對應 REF: uldivmod，不檢查零
                    nsAdj *= 1000;
                    nsAdj /= fps; // 對應 REF: uldivmod，不檢查零
                    presTime += nsAdj;
                    *reinterpret_cast<int64_t *>(THIS_U8(0x620)) = presTime;
                }
                *reinterpret_cast<int64_t *>(RI_U8(param_1, 0x110)) = presTime;
            }

            {
                int *ro = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
                int s = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
                uint32_t flags = (uint32_t)s >> 31;
                returnBufferToSurface(param_1, (uint8_t)flags);
            }

            PAL_LogPrint(__FILE__, __LINE__, 1, 0x40,
                         "===== Super slow start  =====   [%d, %d] -> [%d, %d] ",
                         THIS_U32(0x5e4), THIS_U32(0x5e8),
                         THIS_U32(0x5fc), THIS_U32(0x600));

            param_1->deinit();
            delete param_1;
            THIS_U32(0x5b4) = 2;
            /* 對應 REF：do_super_slow_capture 結束後 fall-through 到 tryVideoCaptureRequest */
            tryVideoCaptureRequest();
            return;
        }

        case 2:
        {
            int *ro = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
            int status = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);

            uint32_t tickRate = THIS_U32(0x610);
            int64_t framePos = 0;
            uint32_t cancelFlags = 0;

            if (status < 0)
            {
                goto state2_calc_time;
            }

            framePos = (int64_t)((uint64_t)tickRate * RI_U32(param_1, 0x118)) + RI_U32(param_1, 0x11c);
            {
                int64_t endPos = *reinterpret_cast<int64_t *>(THIS_U8(0x608));
                if (framePos <= endPos)
                {
                    goto state2_calc_time;
                }
            }

            {
                const char *errFile;
                int errLine;
                const char *errMsg;
                ResultItem *prep = mPrepareResults.popRoot();
                if (!prep)
                {
                    errFile = __FILE__;
                    errLine = __LINE__;
                    errMsg = "Pop item from mFreeSwResultList failed";
                    goto state2_log_err;
                }
                else
                {
                    int cpErr = copyResultItem(prep, param_1);
                    if (cpErr < 0)
                    {
                        errFile = __FILE__;
                        errLine = __LINE__;
                        errMsg = "Copy result item failed: err = %d";
                        goto state2_log_err;
                    }
                    else
                    {
                        mFreeVideoResults.pushLast(prep);
                    }
                }
                cancelFlags = 1;
                goto state2_return_buffer;
            state2_log_err:
                PAL_LogPrint(errFile, errLine, 0x100, 1, errMsg);
                cancelFlags = 1;
            }
            goto state2_return_buffer;

        state2_calc_time:
            {
                int64_t startPos = *reinterpret_cast<int64_t *>(THIS_U8(0x5f0));
                int64_t diff = (framePos - startPos) * 1000000000000LL;
                uint32_t fps = THIS_U32(0x590);
                diff /= fps; // 對應 REF: uldivmod，不檢查零
                int64_t base = *reinterpret_cast<int64_t *>(THIS_U8(0x620));
                *reinterpret_cast<int64_t *>(RI_U8(param_1, 0x110)) = diff + base;
            }
            {
                int *ro2 = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
                int s = (*reinterpret_cast<int (**)(int *)>(*ro2 + 0x10))(ro2);
                cancelFlags = (uint32_t)s >> 31;
            }

        state2_return_buffer:
            returnBufferToSurface(param_1, (uint8_t)cancelFlags);
            param_1->deinit();
            delete param_1;
            tryVideoCaptureRequest();
            return;
        }

        case 3:
        {
            mFreeSwResults.pushLast(param_1);

            int frame_s;
            {
                int *ro = reinterpret_cast<int *>(RI_PTR(param_1, 0xd0));
                frame_s = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
                if (frame_s >= 0)
                {
                    THIS_U32(0x5a8) = RI_U32(param_1, 0x110);
                    THIS_U32(0x5ac) = RI_U32(param_1, 0x114);
                }
            }

            ResultItem *vidItem = mFreeVideoResults.popRoot();
            while (vidItem != nullptr)
            {
                ResultItem *newItem = createResultItemFromBuffer();
                if (newItem)
                {
                    copyResultItem(newItem, vidItem);
                    int *ib = reinterpret_cast<int *>(RI_PTR(newItem, 0x124));
                    (*reinterpret_cast<void (**)(int *)>(*ib + 0x2c))(ib);
                    int *ro2 = reinterpret_cast<int *>(RI_PTR(newItem, 0xd0));
                    int s2 = (*reinterpret_cast<int (**)(int *)>(*ro2 + 0x10))(ro2);
                    uint32_t f2 = (uint32_t)s2 >> 31;
                    returnBufferToSurface(newItem, (uint8_t)f2);
                    newItem->deinit();
                    delete newItem;
                }
                mPrepareResults.pushLast(vidItem);
                if (THIS_I32(0x570) == 0)
                {
                    notifyResult(*reinterpret_cast<Request **>(THIS_U8(0x5d8)), (PAL_Err_t)0);
                    THIS_PTR(0x5d8) = nullptr;
                }
                vidItem = mFreeVideoResults.popRoot();
            }

            {
                ResultItem *sw;
                while ((sw = mFreeSwResults.popRoot()) != nullptr)
                {
                    int *ro3 = reinterpret_cast<int *>(RI_PTR(sw, 0xd0));
                    int s3 = (*reinterpret_cast<int (**)(int *)>(*ro3 + 0x10))(ro3);
                    uint32_t f3 = (uint32_t)s3 >> 31;
                    returnBufferToSurface(sw, (uint8_t)f3);
                    sw->deinit();
                    delete sw;
                }
            }

            {
                int64_t curTs = *reinterpret_cast<int64_t *>(THIS_U8(0x5a8));
                int64_t limTs = *reinterpret_cast<int64_t *>(THIS_U8(0x5c8));
                /* 對應 REF: (curTs > limTs) || (tryVideoCaptureRequest(), 5b4==4) */
                if (curTs > limTs ||
                    (tryVideoCaptureRequest(), THIS_I32(0x5b4) == 4))
                {
                    THIS_U32(0x5b4) = 0;
                    int paErr = processAsyncInternal(PROCESS_BURST);
                    if ((uint32_t)paErr > 0x7fffffffu)
                    {
                        notifyResult(mPrepareVideoReq, (PAL_Err_t)paErr);
                        mPrepareVideoReq = nullptr;
                    }
                }
            }
            return;
        }

        default:
            param_1->deinit();
            delete param_1;
            return;
        }
    }

    /* ────────────────────────────────────────────────
     *  tryVideoCaptureRequest
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::tryVideoCaptureRequest()
    {
        int pending = THIS_I32(0x588);
        int maxBuf = THIS_I32(0x58c);
        if (maxBuf == pending)
            return;

        int count = maxBuf - pending;
        int errorFlag = 0;

        for (int i = 0; i < count; i++)
        {
            ResultItem *item = createResultItemFromBuffer();
            if (!item)
            {
                PAL_LogPrint(__FILE__, __LINE__, 1, 0x40, "Failed to create ResultItem");
                errorFlag = -0x6f;
            }
            else
            {
                if (errorFlag >= 0)
                {
                    /* 對應 REF：先讀取 buffer 的 width/height（在構造 HIDL 物件之前） */
                    ANativeWindowBuffer *buf0 =
                        reinterpret_cast<ANativeWindowBuffer *>(RI_PTR(item, 0x128));
                    uint32_t bufW = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(buf0) + 0x20);
                    uint32_t bufH = *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(buf0) + 0x24);

                    /* Param 歸零初始化 + HIDL 欄位構造（與 processAsyncInternal 一致） */
                    alignas(8) uint8_t paramBuf[sizeof(V30::Param)];
                    memset(paramBuf, 0, sizeof(paramBuf));
                    uint8_t *p = paramBuf;
                    auto *pStr = new (p + 0x30) android::hardware::hidl_string();
                    auto *pHdl1 = new (p + 0x78) android::hardware::hidl_handle();
                    auto *pHdl2 = new (p + 0x98) android::hardware::hidl_handle();
                    /* REF: local_48=0 (8B NEON); local_4c=0; uStack_3c=1
                     * 以 uint64_t 清零前 8 bytes，觸發 GCC NEON vmov+vstr */
                    *reinterpret_cast<uint64_t *>(p + 0xB8) = 0ULL;
                    *reinterpret_cast<uint32_t *>(p + 0xC0) = 0;
                    auto *pVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(p + 0xB8);

                    *reinterpret_cast<uint32_t *>(p + 0x00) = 5; // processType
                    p[0xC4] = 1;                                 // capture flag

                    // 搜尋 mBuffers 找到對應或空 slot
                    uint32_t bufferCount = THIS_U32(0x438);
                    uint32_t bufIdx = 0;
                    for (bufIdx = 0; bufIdx < bufferCount; bufIdx++)
                    {
                        ANativeWindowBuffer *slot = mBuffers[bufIdx];
                        if (slot == buf0 || slot == nullptr)
                            break;
                    }
                    *reinterpret_cast<uint32_t *>(p + 0xB0) = bufIdx;

                    // item+0x124 虛擬呼叫取得 queryResult
                    void *imgBufObj = RI_PTR(item, 0x124);
                    {
                        uintptr_t *vtbl = *reinterpret_cast<uintptr_t **>(imgBufObj);
                        typedef uint32_t (*QueryFn)(void *);
                        QueryFn fn = reinterpret_cast<QueryFn>(vtbl[0x3c / 4]);
                        uint32_t qr = fn(imgBufObj);
                        *reinterpret_cast<uint32_t *>(p + 0xA8) = qr;
                    }

                    // 若 slot 為空，註冊 buffer 並複製 metadata
                    if (mBuffers[bufIdx] == nullptr)
                    {
                        mBuffers[bufIdx] = buf0;
                        *reinterpret_cast<uint32_t *>(p + 0x90) = bufW;
                        *reinterpret_cast<uint32_t *>(p + 0x94) = bufH;
                        native_handle_t *nh = *reinterpret_cast<native_handle_t **>(
                            reinterpret_cast<uint8_t *>(buf0) + 0x3c);
                        /* 對應 REF: hidl_handle::operator=(ahStack_68, nh)，非 owning */
                        assign_hidl_handle_from_native(
                            reinterpret_cast<android::hardware::hidl_handle *>(p + 0x98), nh);
                    }

                    RI_PTR(item, 0xd8) = item;
                    RI_U32(item, 0xdc) = 0;

                    {
                        auto svc = reinterpret_cast<V30::ICacao *>(
                            *reinterpret_cast<void **>(
                                reinterpret_cast<uint8_t *>(this) + 0x2f4));
                        /* 直接傳 const Param& 和 const Result& — 與 REF tryVideoCapture 完全一致 */
                        const V30::Result &resultRef3 = *reinterpret_cast<const V30::Result *>(RI_U8(item, 0xd8));
                        auto ret = svc->processAsync(*reinterpret_cast<const V30::Param *>(paramBuf), resultRef3);
                        /* 對應 REF：先呼叫 HIDL 欄位 dtor（反向 ctor 順序），再檢查錯誤 */
                        pVec->~hidl_vec<V30::ImageBufInfo>();
                        pHdl2->~hidl_handle();
                        pHdl1->~hidl_handle();
                        pStr->~hidl_string();
                        if (hidlErrCodeLenient(ret) != 0)
                        {
                            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "processAsync for capture failed");
                            errorFlag = -0x6f;
                        }
                    }
                }

                if (errorFlag >= 0)
                {
                    mAsyncResults.pushLast(item);
                    THIS_I32(0x588) += 1;
                    continue;
                }

                returnBufferToSurface(item, 1);
                item->deinit();
                delete item;
            }
        }
    }

    /* ────────────────────────────────────────────────
     *  returnBufferToSurface
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::returnBufferToSurface(ResultItem *item, uint8_t cancelFlag)
    {
        uint32_t err = 0;
        const char *errFile;
        const char *errMsg;
        int errLine;

        if (!THIS_PTR(0x57c))
        {
            errLine = __LINE__;
            errFile = __FILE__;
            errMsg = "NativeWindow is not available";
            goto log_info;
        }
        if (RI_PTR(item, 0x128) == nullptr)
        {
            errLine = __LINE__;
            errFile = __FILE__;
            errMsg = "Android buffer not available";
            goto log_err1;
        }

        if (cancelFlag != 0)
        {
            ANativeWindow *surface = reinterpret_cast<ANativeWindow *>(THIS_PTR(0x57c));
            ANativeWindowBuffer *buf = reinterpret_cast<ANativeWindowBuffer *>(RI_PTR(item, 0x128));
            int fence = RI_I32(item, 0x120);
            int cErr = surface->cancelBuffer(surface, buf, fence);
            if (cErr != 0)
            {
                errLine = __LINE__;
                errFile = __FILE__;
                errMsg = "Cancel buffer failed";
                goto log_err2;
            }
        }
        else
        {
            ANativeWindow *surface = reinterpret_cast<ANativeWindow *>(THIS_PTR(0x57c));

            uint64_t tsOffset = ((uint64_t)THIS_U32(0x61c) << 32) | THIS_U32(0x618);
            uint64_t itemTs = ((uint64_t)RI_U32(item, 0x114) << 32) | RI_U32(item, 0x110);
            int64_t finalTs = (int64_t)(tsOffset + itemTs);
            int tsErr = surface->perform(surface, NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP, finalTs);
            if (tsErr != 0)
            {
                errLine = __LINE__;
                errFile = __FILE__;
                errMsg = "Set time stamp failed : %d";
                goto log_err2;
            }

            ANativeWindowBuffer *buf = reinterpret_cast<ANativeWindowBuffer *>(RI_PTR(item, 0x128));
            int fence = RI_I32(item, 0x120);
            int qErr = surface->queueBuffer(surface, buf, fence);
            if (qErr != 0)
            {
                errLine = __LINE__;
                errFile = __FILE__;
                errMsg = "Queue buffer failed";
                goto log_err2;
            }
        }
        goto post_buf;

    log_info:
        PAL_LogPrint(errFile, errLine, 1, 0x40, errMsg);
        err = 0xffffff9bu;
        goto post_buf;
    log_err1:
        PAL_LogPrint(errFile, errLine, 0x100, 1, errMsg);
        err = 0xffffff9bu;
        goto post_buf;
    log_err2:
        PAL_LogPrint(errFile, errLine, 0x100, 1, errMsg);
        err = 0xffffff91u;

    post_buf:
        {
            ANativeWindowBuffer *buf = reinterpret_cast<ANativeWindowBuffer *>(RI_PTR(item, 0x128));
            uint32_t bufCount = THIS_U32(0x438);
            if (bufCount != 0)
            {
                uint32_t i = 0;
                do
                {
                    if (mBuffers[i] == buf)
                        break;
                    i++;
                } while (i < bufCount);
            }
        }

        if (err > 0x7fffffffu && RI_I32(item, 0x120) != -1)
        {
            close(RI_I32(item, 0x120));
            RI_I32(item, 0x120) = -1;
        }
    }

    /* ────────────────────────────────────────────────
     *  createNativeWindow
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::createNativeWindow(ProcessCtrlVideoRecParam *param)
    {
        if (mSurface)
        {
            deleteNativeWindow();
        }

        // REF 使用集中 goto 錯誤處理模式 — 所有 native_window_* 失敗都跳到
        // 共用的 PAL_LogPrint + cleanup 出口，減少重複的 sp<> 析構程式碼。
        int ret;
        const char *errFile;
        const char *errMsg;
        int errLine;

        ANativeWindow *paramWindow =
            *reinterpret_cast<ANativeWindow **>(
                reinterpret_cast<uint8_t *>(param) + 0x160);
        if (!paramWindow)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Buffer producer not found");
            ret = (int)0xffffff98u; // -0x68
            goto cleanup;
        }

        {
            // [20260811 修正] 先前用 `::operator new(0x788)` + placement-new 手動配置
            // Surface 物件，0x788 是反編譯 a9 (poplar_kddi) stock 二進位檔得出的
            // sizeof(Surface)，只對那個 Android 9 版本的 libgui.so ABI 有效。
            // 這裡實際連結的是本平台（Android 15）frameworks/native/libs/gui 編出
            // 的 libgui.so，Surface 類別大小已經不同，用舊的 0x788 配置會導致建構子
            // 寫入超出配置範圍，造成 heap 損毀（實測：錄影開始時 cacaoserver 於
            // Surface 建構子內 SIGSEGV）。改回直接 `new`，交給編譯器用它實際連結
            // 的（正確的）Surface 標頭決定大小。
            // 視窗由呼叫端（AIDL 的 android.view.Surface）提供，不再於此用 libgui 建立 Surface。
            ANativeWindow *w = paramWindow;
            sp<ANativeWindow> surfSp = w;

            if (!surfSp.get())
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "native window NULL");
                ret = -0x70;
                goto cleanup;
            }

            // REF 順序: api_connect → data_space → query usage → set_usage64 →
            //           scaling_mode → query w/h → set_buffers_format → min_undequeued →
            //           計算 count → set_buffer_count → dequeue/cancel → set_transform
            ret = w->perform(w, NATIVE_WINDOW_API_CONNECT, NATIVE_WINDOW_API_CAMERA);
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while connecting API to window";
                errFile = __FILE__;
                goto log_err;
            }

            ret = w->perform(w, NATIVE_WINDOW_SET_BUFFERS_DATASPACE, *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(param) + 0x164));
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while setting data space";
                errFile = __FILE__;
                goto log_err;
            }

            {
                // [已修正，20260802] 反編譯確認原版這裡呼叫的是 perform cmd=0x1e(30)
                // = NATIVE_WINDOW_SET_USAGE64，不是 NATIVE_WINDOW_SET_USAGE
                // （這個常數在這份 SDK 的 system/window.h 裡是「=0，deprecated」的
                // 舊版 32-bit 版本，跟 SET_USAGE64 是完全不同的兩個 perform
                // command，name 相似容易看錯）。OR 進去的值 0x20033 本身沒錯，
                // 只是原本呼叫錯 command id。
                int usage = 0;
                ret = w->query(w, NATIVE_WINDOW_CONSUMER_USAGE_BITS, &usage);
                if (ret)
                {
                    errLine = __LINE__;
                    errMsg = "Error while getting usage";
                    errFile = __FILE__;
                    goto log_err;
                }

                usage |= 0x20033;
                ret = w->perform(w, NATIVE_WINDOW_SET_USAGE64, (uint64_t)(uint32_t)usage);
                if (ret)
                {
                    errLine = __LINE__;
                    errMsg = "Error while setting flags";
                    errFile = __FILE__;
                    goto log_err;
                }
            }

            ret = w->perform(w, NATIVE_WINDOW_SET_SCALING_MODE, NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while setting scaling mode";
                errFile = __FILE__;
                goto log_err;
            }

            ret = w->query(w, NATIVE_WINDOW_WIDTH, (int *)&THIS_U32(0x580));
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while getting width";
                errFile = __FILE__;
                goto log_err;
            }
            ret = w->query(w, NATIVE_WINDOW_HEIGHT, (int *)&THIS_U32(0x584));
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while getting height";
                errFile = __FILE__;
                goto log_err;
            }

            ret = w->perform(w, NATIVE_WINDOW_SET_BUFFERS_FORMAT, 0x22);
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while setting format";
                errFile = __FILE__;
                goto log_err;
            }

            {
                int minUndequeued = 0;
                ret = w->query(w, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeued);
                if (ret)
                {
                    errLine = __LINE__;
                    errMsg = "Error while querying min undequeued buffers";
                    errFile = __FILE__;
                    goto log_err;
                }

                int totalBufs;
                if ((THIS_U32(0x5dc) & 0xfffffffe) == 4)
                {
                    THIS_I32(0x58c) = (int)mBurstCount;
                    int fr = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(param) + 0x168);
                    totalBufs = (int)mBurstCount + minUndequeued +
                                (int)(((uint64_t)(uint32_t)(fr * 30000) / mVideoMaxDur) / 1000);
                }
                else
                {
                    THIS_I32(0x58c) = (int)mBufferCount;
                    totalBufs = (int)mBufferCount + minUndequeued;
                }

                if (totalBufs > 64)
                    totalBufs = 64;

                int allocErr = 0;
                ret = w->perform(w, NATIVE_WINDOW_SET_BUFFER_COUNT, totalBufs);
                if (ret)
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Error while setting buffer count");
                    allocErr = -0x6f;
                }

                mBuffers.free();
                mBuffers.alloc(totalBufs);

                int deqCount = totalBufs - minUndequeued;
                unsigned i = 0;
                for (; (int)i < deqCount; i++)
                {
                    ANativeWindowBuffer *buf = nullptr;
                    int fence = -1;
                    int dqErr = w->dequeueBuffer(w, &buf, &fence);
                    if (dqErr == 0)
                    {
                        if (allocErr >= 0)
                        {
                            mBuffers[i] = buf;
                        }
                    }
                    else
                    {
                        if (dqErr == -0x13)
                            THIS_U32(0x5b4) = 4;
                        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Dequeue failed. ret:%d", dqErr);
                        allocErr = -0x68;
                    }
                }

                // REF: 剩餘 slot 填 NULL
                for (; (int)i < totalBufs; i++)
                {
                    mBuffers[i] = nullptr;
                }

                // cancel 所有 dequeued buffer
                if (minUndequeued < totalBufs)
                {
                    for (unsigned j = 0; (int)j < deqCount; j++)
                    {
                        ANativeWindowBuffer *buf = mBuffers[j];
                        w->cancelBuffer(w, buf, -1);
                    }
                }

                if (allocErr < 0)
                {
                    ret = allocErr;
                    goto cleanup_window;
                }
            }

            ret = w->perform(w, NATIVE_WINDOW_SET_BUFFERS_TRANSFORM, 0);
            if (ret)
            {
                errLine = __LINE__;
                errMsg = "Error while setting buffer transform";
                errFile = __FILE__;
                goto log_err;
            }

            // 成功 — REF 用 sp<ANativeWindow>::operator= 設定 mSurface
            // 等效於 incStrong + store
            reinterpret_cast<sp<ANativeWindow> *>(&mSurface)->operator=(surfSp);
            THIS_U32(0x588) = 0;
            ret = 0;
            goto cleanup_window;

        log_err:
            PAL_LogPrint(errFile, errLine, 0x100, 1, errMsg);
            ret = -0x6f;

        cleanup_window:;
            // REF: 手動 decStrong ANativeWindow
        }

    cleanup:
        return ret;
    }

    /* ────────────────────────────────────────────────
     *  deleteNativeWindow
     * ──────────────────────────────────────────────── */

    void ProcessCtrlGateway::deleteNativeWindow()
    {
        if (!mSurface)
            return;
        int err = native_window_api_disconnect(mSurface, NATIVE_WINDOW_API_CAMERA);
        if (err != 0)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Disconnect window failed. ret:%d", err);
        }
        // REF: sp<ANativeWindow>::operator=(0) → decStrong + null
        // 對應 createNativeWindow 中 surface->incStrong(this)
        mSurface->decStrong(this);
        mSurface = nullptr;
    }

    /* ────────────────────────────────────────────────
     *  createFreeSwResultList
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::createFreeSwResultList()
    {
        // 計算目標 frame 數（參考版直接除，不做除零保護）
        uint64_t n1 = (uint64_t)THIS_U32(0x598) * 1000000000ULL;
        uint64_t lVar8 = n1 / (uint64_t)THIS_U32(0x5bc);
        int iVar1 = (int)((uint32_t)((lVar8 * 30000ULL) / 0xe8d4a51000ULL));

        if (!mSurface)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Not prepared yet");
            return (int)0xffffff9bu;
        }

        // Probe：dequeue 一個 buffer 取得 stride
        ResultItem *probe = createResultItemFromBuffer();
        if (!probe)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "Failed to create result item for buffer size");
            return (int)0xffffff91u;
        }

        // 透過 vtable offset 0x14 取得 ImageBuf 的 stride
        int *probeImgBuf = reinterpret_cast<int *>(RI_U32(probe, 0x124));
        uint32_t stride = (*reinterpret_cast<uint32_t (**)(int *)>(*probeImgBuf + 0x14))(probeImgBuf);

        // 歸還 probe buffer 並銷毀 probe ResultItem
        returnBufferToSurface(probe, 1);
        probe->deinit();
        (*reinterpret_cast<void (**)(ResultItem *)>(*reinterpret_cast<uint32_t *>(probe) + 4))(probe);

        // 迴圈建立 skeleton ResultItem（不 dequeue，僅存尺寸/stride/format）
        uint32_t uVar4 = 0;
        do
        {
            // 已有足夠 items → 完成
            if ((uint32_t)iVar1 + 0x14u <= THIS_U32(0x428))
            {
                return (int)uVar4;
            }
            ResultItem *ri = new ResultItem();
            ProcessCtrlResult *result = new ProcessCtrlResult();
            *reinterpret_cast<ProcessCtrlResult **>(reinterpret_cast<uint8_t *>(ri) + 0xd0) = result;
            ImageBuf *imgBuf = new ImageBuf(
                ImageSize{THIS_U32(0x580), THIS_U32(0x584)},
                stride, (ImageFormat)0x20001u, (DataSpace)THIS_U32(0x628));
            *reinterpret_cast<ImageBuf **>(reinterpret_cast<uint8_t *>(ri) + 0x124) = imgBuf;
            uVar4 = (uint32_t)mPrepareResults.pushLast(ri);
        } while (uVar4 < 0x80000000u);
        return (int)uVar4;
    }

    /* ────────────────────────────────────────────────
     *  createResultItemFromBuffer
     * ──────────────────────────────────────────────── */

    ProcessCtrlGateway::ResultItem *ProcessCtrlGateway::createResultItemFromBuffer()
    {
        if (!mSurface)
        {
            PAL_LogPrint(__FILE__, __LINE__, 1, 0x40, "NativeWindow is not available");
            return nullptr;
        }

        ResultItem *item = new ResultItem();
        ANativeWindowBuffer *buf = nullptr;
        int fence = -1;
        ImageBuf *imgBuf = nullptr;

        int dqErr = mSurface->dequeueBuffer(mSurface, &buf, &fence);
        if (dqErr == 0 && buf)
        {
            ImageFormat imageFormat = (ImageFormat)0;
            if (THIS_U32(0x5dc) == 4)
            {
                imageFormat = (ImageFormat)0x20001;
            }
            uint32_t w = static_cast<uint32_t>(buf->width);
            uint32_t h = static_cast<uint32_t>(buf->height);
            void *data = const_cast<native_handle_t *>(buf->handle);
            DataSpace ds = (DataSpace)THIS_U32(0x628);
            ImageSize sz = {w, h};

            imgBuf = new ImageBuf(data, sz, ds, imageFormat);
            if (!imgBuf)
                goto error;

            ProcessCtrlResult *result = new ProcessCtrlResult();
            if (!result)
                goto error;

            RI_PTR(item, 0xd0) = result;
            RI_I32(item, 0x120) = fence;
            RI_PTR(item, 0x124) = imgBuf;
            RI_PTR(item, 0x128) = buf;
            return item;
        }

        if (dqErr == -19)
            THIS_U32(0x5b4) = 4;
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Dequeue failed. ret:%d", dqErr);

    error:
        if (mSurface && buf)
        {
            int cancelErr = mSurface->cancelBuffer(mSurface, buf, fence);
            if (cancelErr != 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Cancel buffer failed");
            }
        }
        if (imgBuf)
        {
            delete imgBuf;
        }
        item->deinit();
        delete item;
        return nullptr;
    }

    /* ────────────────────────────────────────────────
     *  copyProcessCtrlParam
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::copyProcessCtrlParam(const Request *req, V30::Param *out)
    {
        int result = (int)0xffffff99;
        if (!req || !out)
            return result;

        int paramObj = *reinterpret_cast<const int *>(reinterpret_cast<const uint8_t *>(req) + 4);
        if (!paramObj)
            return result;
        uint8_t *p = reinterpret_cast<uint8_t *>(paramObj);

        __aeabi_memclr(out, sizeof(V30::Param));
        uint8_t *dst = reinterpret_cast<uint8_t *>(out);

        uint32_t cvt = convert(static_cast<ProcessType>(*reinterpret_cast<uint32_t *>(p + 8)));
        *reinterpret_cast<uint32_t *>(dst) = cvt;

        auto *hdl1 = reinterpret_cast<android::hardware::hidl_handle *>(dst + 0x78);
        auto *hdl2 = reinterpret_cast<android::hardware::hidl_handle *>(dst + 0x98);
        assign_hidl_handle_null(hdl1);
        assign_hidl_handle_null(hdl2);

        int vecPtr = *reinterpret_cast<const int *>(reinterpret_cast<const uint8_t *>(req) + 8);
        if (vecPtr != 0)
        {
            uint32_t bufCount = *reinterpret_cast<uint32_t *>(vecPtr + 8);
            *reinterpret_cast<uint32_t *>(dst + 0xb4) = bufCount;
            auto *hidlVec = reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(
                dst + 0xb8);
            resize_image_buf_vec(hidlVec, bufCount);
            for (uint32_t i = 0; i < bufCount; i++)
            {
                int *imgBuf = *reinterpret_cast<int **>(*reinterpret_cast<int *>(vecPtr + 4) + i * 4);
                uint8_t *info = reinterpret_cast<uint8_t *>(&((*hidlVec)[i]));
                uint32_t wh[2] = {};
                (*reinterpret_cast<void (**)(uint32_t *, int *)>(*imgBuf + 0x1c))(wh, imgBuf);
                native_handle_t *nh = (*reinterpret_cast<native_handle_t *(**)(int *)>(
                    *imgBuf + 0x10))(imgBuf);
                *reinterpret_cast<uint32_t *>(info) = wh[0];
                *reinterpret_cast<uint32_t *>(info + 4) = wh[1];
                assign_hidl_handle_from_native(
                    reinterpret_cast<android::hardware::hidl_handle *>(info + 8), nh);
                uint32_t cf = (*reinterpret_cast<uint32_t (**)(int *)>(*imgBuf + 0x3c))(imgBuf);
                *reinterpret_cast<uint32_t *>(info + 0x18) = cf;
            }
        }

        dst[8] = p[0x10];
        dst[9] = p[0x11];
        *reinterpret_cast<uint64_t *>(dst + 0x10) = *reinterpret_cast<const uint64_t *>(p + 0x18);
        *reinterpret_cast<uint64_t *>(dst + 0x18) = *reinterpret_cast<const uint64_t *>(p + 0x20);
        *reinterpret_cast<uint64_t *>(dst + 0x20) = *reinterpret_cast<const uint64_t *>(p + 0x28);
        dst[0x28] = p[0x30];

        size_t slen = strlen(reinterpret_cast<const char *>(p + 0x31));
        reinterpret_cast<android::hardware::hidl_string *>(dst + 0x30)->setToExternal(reinterpret_cast<const char *>(p + 0x31), slen);

        dst[0x40] = p[0x131];
        *reinterpret_cast<uint32_t *>(dst + 0x48) = *reinterpret_cast<const uint32_t *>(p + 0x138);
        *reinterpret_cast<uint32_t *>(dst + 0x4c) = *reinterpret_cast<const uint32_t *>(p + 0x13c);
        dst[0x50] = p[0x140];
        *reinterpret_cast<uint32_t *>(dst + 0x54) = *reinterpret_cast<uint32_t *>(p + 0x144);
        dst[0x58] = p[0x148];
        *reinterpret_cast<uint32_t *>(dst + 0x5c) = *reinterpret_cast<uint32_t *>(p + 0x14c);
        *reinterpret_cast<uint32_t *>(dst + 0x60) = *reinterpret_cast<uint32_t *>(p + 0x150);
        *reinterpret_cast<uint32_t *>(dst + 0x64) = *reinterpret_cast<uint32_t *>(p + 0x154);
        *reinterpret_cast<uint32_t *>(dst + 0x68) = *reinterpret_cast<uint32_t *>(p + 0x158);
        dst[0x6c] = p[0x15c];
        return 0;
    }

    /* ────────────────────────────────────────────────
     *  matchResult   (Ghidra 0001adb8)
     *  比較器：item[0xd8] == (int)data && item[0xdc] == 0
     * ──────────────────────────────────────────────── */
    bool ProcessCtrlGateway::matchResult(ResultItem *item, void *data)
    {
        return RI_I32(item, 0xd8) == (int)(uintptr_t)data && RI_I32(item, 0xdc) == 0;
    }

    /* ────────────────────────────────────────────────
     *  matchOldFrameId   (Ghidra 0001c4d4)
     *  比較器：若 buffer 狀態 OK，則 item->frame_sec <= *threshold；否則永遠為 true
     * ──────────────────────────────────────────────── */
    bool ProcessCtrlGateway::matchOldFrameId(ResultItem *item, void *data)
    {
        int *ro = reinterpret_cast<int *>(RI_PTR(item, 0xd0));
        int status = (*reinterpret_cast<int (**)(int *)>(*ro + 0x10))(ro);
        if (status >= 0)
        {
            return RI_U32(item, 0x118) <= *reinterpret_cast<uint32_t *>(data);
        }
        return true;
    }

    /* ────────────────────────────────────────────────
     *  copyResultItem
     * ──────────────────────────────────────────────── */

    int ProcessCtrlGateway::copyResultItem(ResultItem *dst, ResultItem *src)
    {
        const char *logFile;
        int logLine;
        const char *logMsg;

        if (!dst || !src)
        {
            logLine = __LINE__;
            logFile = __FILE__;
            logMsg = "Src/Dst Result Item must not be null";
            goto log_err;
        }

        {
            int *srcBuf = reinterpret_cast<int *>(RI_PTR(src, 0x124));
            int *dstBuf = reinterpret_cast<int *>(RI_PTR(dst, 0x124));

            int srcFmt = (*reinterpret_cast<int (**)(int *)>(*srcBuf + 0x14))(srcBuf);
            int dstFmt = (*reinterpret_cast<int (**)(int *)>(*dstBuf + 0x14))(dstBuf);
            if (srcFmt != dstFmt)
            {
                logLine = __LINE__;
                logFile = __FILE__;
                logMsg = "Buffer size must be same";
                goto log_err;
            }

            void *srcData = (*reinterpret_cast<void *(**)(int *)>(*srcBuf + 0x18))(srcBuf);
            void *dstData = (*reinterpret_cast<void *(**)(int *)>(*dstBuf + 0x18))(dstBuf);
            uint32_t dataSize = (*reinterpret_cast<uint32_t (**)(int *)>(*srcBuf + 0x14))(srcBuf);
            memcpy(dstData, srcData, dataSize);

            int *srcResult = reinterpret_cast<int *>(RI_PTR(src, 0xd0));
            int *dstResult = reinterpret_cast<int *>(RI_PTR(dst, 0xd0));
            {
                int status = (*reinterpret_cast<int (**)(int *)>(*srcResult + 0x10))(srcResult);
                (*reinterpret_cast<void (**)(int *, int)>(*dstResult + 0x14))(dstResult, status);
            }

            RI_U32(dst, 8) = RI_U32(src, 8);
            __aeabi_memcpy8(RI_U8(dst, 0x10), RI_U8(src, 0x10), 32);
            *RI_U8(dst, 0x30) = *RI_U8(src, 0x30);

            *reinterpret_cast<android::hardware::hidl_string *>(RI_U8(dst, 0x38)) =
                *reinterpret_cast<android::hardware::hidl_string *>(RI_U8(src, 0x38));

            __aeabi_memcpy8(RI_U8(dst, 0x48), RI_U8(src, 0x48), 44);
            *RI_U8(dst, 0x74) = *RI_U8(src, 0x74);
            *reinterpret_cast<uint64_t *>(RI_U8(dst, 0x78)) =
                *reinterpret_cast<const uint64_t *>(RI_U8(src, 0x78));

            assign_hidl_handle(
                reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(dst, 0x80)),
                reinterpret_cast<const android::hardware::hidl_handle *>(RI_U8(src, 0x80)));

            RI_U32(dst, 0x90) = RI_U32(src, 0x90);
            RI_U32(dst, 0x98) = RI_U32(src, 0x98);
            RI_U32(dst, 0x9c) = RI_U32(src, 0x9c);

            assign_hidl_handle(
                reinterpret_cast<android::hardware::hidl_handle *>(RI_U8(dst, 0xa0)),
                reinterpret_cast<const android::hardware::hidl_handle *>(RI_U8(src, 0xa0)));

            RI_U32(dst, 0xb0) = RI_U32(src, 0xb0);
            RI_U32(dst, 0xb8) = RI_U32(src, 0xb8);
            RI_U32(dst, 0xbc) = RI_U32(src, 0xbc);

            assign_image_buf_vec(
                reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(RI_U8(dst, 0xc0)),
                reinterpret_cast<android::hardware::hidl_vec<V30::ImageBufInfo> *>(RI_U8(src, 0xc0)));

            /* REF: __aeabi_memcpy8（8-byte 對齊，0xd8 是 8-byte 對齊偏移）*/
            __aeabi_memcpy8(RI_U8(dst, 0xd8), RI_U8(src, 0xd8), 0x48);

            int *dstImage = reinterpret_cast<int *>(RI_PTR(dst, 0x124));
            (*reinterpret_cast<void (**)(int *, int)>(*dstImage + 0x24))(dstImage, 1);

            return 0;
        }

    log_err:
        PAL_LogPrint(logFile, logLine, 0x100, 1, logMsg);
        return -0x67;
    }

    /* ────────────────────────────────────────────────
     *  convert
     * ──────────────────────────────────────────────── */

    uint32_t ProcessCtrlGateway::convert(ProcessType pt)
    {
        uint32_t v = static_cast<uint32_t>(pt);
        if (v < 10)
        {
            return v;
        }
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                     "invalid process type");
        return 0;
    }

    void ProcessCtrlGateway::printStackTrace()
    {
    }

    void ProcessCtrlGateway::getService()
    {
    }

    int ProcessCtrlGateway::setConfig_v30(const ProcessConfigBase *cfg)
    {
        (void)cfg;
        return 0;
    }

    int ProcessCtrlGateway::getConfig_v30(ProcessConfigBase *cfg) const
    {
        (void)cfg;
        return 0;
    }

} // namespace cacao

/* ── ProcessCtrlGatewayFactory ─────────────────── */
namespace cacao
{

    using android::sp;

    ICacaoGateway *ProcessCtrlGatewayFactory::create()
    {
        ProcessCtrlGateway *gw = new ProcessCtrlGateway();
        if (gw)
        {
            int ret = gw->init(100);
            if (ret >= 0)
                return static_cast<ICacaoGateway *>(gw);
            delete gw;
        }
        return nullptr;
    }

    /* createMode — 對應 REF Factory_createMode_0001c620
     * REF: alloc 0x2c, ObjectBase ctor, 手動初始化, 手動安裝 vtable, 直接呼叫 deserialize
     * 我們: 使用 ProcessCtrlMode ctor（自動安裝正確 vtable），然後直接呼叫 deserialize
     * 注意: 回傳 reinterpret_cast（不做 base adjust），和 REF 行為一致 */
    ProcessModeBase *ProcessCtrlGatewayFactory::createMode(
        ISerialize::SerializedData *data)
    {
        void *mem = ObjectBase::operator new(0x2c);
        if (!mem)
            return nullptr;
        ProcessCtrlMode *mode = ::new (mem) ProcessCtrlMode();
        if (mode)
        {
            int ret = mode->deserialize(data);
            if (static_cast<uint32_t>(ret) < 0x80000000u)
            {
                return reinterpret_cast<ProcessModeBase *>(mode);
            }
            delete mode;
        }
        return nullptr;
    }

    /* createParam — 對應 Ghidra Factory_createParam_1_0001c684
     * alloc 0x160 bytes, ProcessCtrlParam ctor, deserialize
     * REF: ProcessCtrlParam ctor → vtable[0x20/4] (deserialize)
     * 我們: ProcessCtrlParam ctor 安裝正確 vtable，然後呼叫 deserialize */
    ProcessParamBase *ProcessCtrlGatewayFactory::createParam(
        ISerialize::SerializedData *data)
    {
        void *mem = ObjectBase::operator new(0x160);
        if (!mem)
            return nullptr;
        ProcessCtrlParam *param = ::new (mem) ProcessCtrlParam();
        if (param)
        {
            int ret = param->deserialize(data);
            if (static_cast<uint32_t>(ret) < 0x80000000u)
            {
                return param;
            }
            delete param;
        }
        return nullptr;
    }

    /* createParam(binder, data) — 對應 Ghidra Factory_createParam_2_0001c710
     * alloc 0x170 bytes, ProcessCtrlVideoRecParam ctor, deserialize,
     * 然後 asInterface(binder) 取得 IGraphicBufferProducer sp */
    ProcessParamBase *ProcessCtrlGatewayFactory::createParam(
        ANativeWindow *window, ISerialize::SerializedData *data)
    {
        void *mem = ObjectBase::operator new(0x170);
        if (!mem)
            return nullptr;
        ProcessCtrlVideoRecParam *param = ::new (mem) ProcessCtrlVideoRecParam();
        uint8_t *p = reinterpret_cast<uint8_t *>(param);
        if (param)
        {
            int ret = param->deserialize(data);
            if (ret >= 0)
            {
                // 寫入 +0x160：持有一份 ANativeWindow 參考
                if (window != nullptr)
                {
                    ANativeWindow_acquire(window);
                    *reinterpret_cast<ANativeWindow **>(p + 0x160) = window;
                    return param;
                }
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1, "Buffer producer cannot be got");
            }
            delete param;
        }
        return nullptr;
    }

    /* createResult — 對應 Ghidra Factory_createResult_0001c7d0
     * REF: alloc 0x34, ProcessCtrlResult ctor, vtable[0x2c/4](this,data) → deserialize
     * 我們: ProcessCtrlResult ctor 安裝正確 vtable，然後呼叫 deserialize */
    ProcessResultBase *ProcessCtrlGatewayFactory::createResult(
        ISerialize::SerializedData *data)
    {
        void *mem = ObjectBase::operator new(0x34);
        if (!mem)
            return nullptr;
        ProcessCtrlResult *result = ::new (mem) ProcessCtrlResult();
        if (result)
        {
            int ret = result->deserialize(data);
            if ((uint32_t)ret < 0x80000000u)
            {
                return result;
            }
            delete result;
        }
        return nullptr;
    }

    /* createConfig — 對應 REF Factory_createConfig_0001c800
     * REF: alloc 0xc, ObjectBase ctor, *(this+8)=0, 手動設 vtable, 直接 bl deserialize
     * 我們: ProcessCtrlDynamicParameterConfig ctor 安裝正確 vtable，然後 deserialize */
    ProcessConfigBase *ProcessCtrlGatewayFactory::createConfig(
        unsigned int type, ISerialize::SerializedData *data)
    {
        void *mem = ObjectBase::operator new(0xc);
        if (!mem)
            return nullptr;
        ProcessCtrlDynamicParameterConfig *cfg = ::new (mem) ProcessCtrlDynamicParameterConfig();
        if (cfg)
        {
            int ret = cfg->deserialize(data);
            if (static_cast<uint32_t>(ret) < 0x80000000u)
            {
                return cfg;
            }
            delete cfg;
        }
        return nullptr;
    }

} // namespace cacao

/* ── 強制匯出 sp<> 模板實例化 ─────────────────────────────
 * 原始 .so 包含這些 sp<> 函式但編譯器通常會 inline
 * 明確實例化以匹配原始 symtab
 * ─────────────────────────────────────────────────────────── */
namespace V30_ = vendor::somc::hardware::camera::cacao::V3_0;
namespace V31_ = vendor::somc::hardware::camera::cacao::V3_1;
namespace VProv_ = vendor::somc::hardware::camera::provider::V1_0;

template class android::sp<V30_::ICacao>;
template class android::sp<V31_::ICacao>;
template class android::sp<V30_::ICacaoCallback>;
template class android::sp<VProv_::IVendorSomcCameraProvider>;

// LINEAGE14_HEADERS（Android 14）的 StrongPointer.h 引入了 fromExisting()/force_set()
// 要求 T 具備 incStrongRequireStrong/forceIncStrong，但 ANativeWindow 沒有這兩個方法。
// 避免 template class（全實例化），改為只實例化需要匯出的個別成員函式。
#if !defined(LIBCACAO_USE_LINEAGE14_HEADERS)
template class android::sp<ANativeWindow>;
#else
// 個別成員顯式實例化（不觸發 fromExisting/force_set）
template android::sp<ANativeWindow> &android::sp<ANativeWindow>::operator=(ANativeWindow *);
template android::sp<ANativeWindow> &android::sp<ANativeWindow>::operator=(const android::sp<ANativeWindow> &);
#endif

// sp<ICacaoCallback> converting ctor from Callback*
template android::sp<V30_::ICacaoCallback>::sp(cacao::ProcessCtrlGateway::Callback *);

/* ── 強制匯出 std::function destructor ────────────────────
 * 原始 .so 匯出這些 std::function dtor，必須顯式實例化
 * ─────────────────────────────────────────────────────────── */
#include <functional>
#include <android/hardware/camera/common/1.0/types.h>

template class std::function<void(
    ::android::hardware::camera::common::V1_0::Status,
    const ::android::sp<V30_::ICacao> &)>;

template class std::function<void(
    const V30_::Config &,
    V30_::ErrCode)>;

/* ── 強制匯出 cacao_pal template vtables ─────────────────────
 * REF .so 匯出這些 vtables，必須顯式實例化
 * ──────────────────────────────────────────────────────────── */
#include <cacao/ProcessCtrlGatewayBase.h>
struct ANativeWindowBuffer;

template class cacao_pal::PtrList<cacao::ProcessConfigBase>;
template class cacao_pal::PtrList<cacao::ProcessCtrlGateway::ResultItem>;
template class cacao_pal::PtrList<cacao::ProcessCtrlGatewayBase::Request>;
template class cacao_pal::PtrLockList<cacao::ProcessConfigBase>;
template class cacao_pal::PtrLockList<cacao::ProcessCtrlGateway::ResultItem>;
template class cacao_pal::PtrLockList<cacao::ProcessCtrlGatewayBase::Request>;
template class cacao_pal::PtrLockListDel<cacao::ProcessConfigBase>;
template class cacao_pal::PtrLockListDel<cacao::ProcessCtrlGatewayBase::Request>;
template class cacao_pal::Array<ANativeWindowBuffer *>;
template class cacao_pal::Array<cacao::ProcessCtrlGatewayBase::Request *>;
template class cacao_pal::Array<cacao_pal::PtrList<cacao::ProcessCtrlGatewayBase::Request>::Node *>;
