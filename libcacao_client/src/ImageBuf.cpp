// ImageBuf.cpp
// cacao::ImageBuf — gralloc v2.0-backed 影像緩衝區
// 反編譯來源: src/libcacao_client.cpp 5207-5900 行（Ghidra pseudocode）
// 100% 遵循原始 .so 行為

#define LOG_TAG "libcacao_client"
#include <log/log.h>

#include <cacao/ProcessCtrlGatewayBase.h> // ImageBuf class + ObjectBase + SharedBuffer

// 確保 sizeof(ImageBuf) 符合各架構的 ABI
#if defined(__LP64__)
static_assert(sizeof(cacao::ImageBuf) == 0x38,
              "ImageBuf size mismatch — must be 0x38 bytes on 64-bit");
#else
static_assert(sizeof(cacao::ImageBuf) == 0x28,
              "ImageBuf size mismatch — must be 0x28 bytes on 32-bit");
#endif

// gralloc v2.0 HIDL
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <android/hardware/graphics/allocator/2.0/IAllocator.h>

// PAL 記憶體函式
extern "C" void *PAL_MemAllocEx(int pool, size_t size, const char *file, int line, int flags);
extern "C" void PAL_MemFreeEx(void *ptr, int flags);

using IMapper = android::hardware::graphics::mapper::V2_0::IMapper;
using IAllocator = android::hardware::graphics::allocator::V2_0::IAllocator;
using MapperError = android::hardware::graphics::mapper::V2_0::Error;

namespace cacao
{

    // ──────────────────────────────────────────────────────────
    // 靜態 gralloc singleton
    // 原始 .so 在 BSS: _ZN5cacao8ImageBuf7mMapperE / mAllocatorE
    // ──────────────────────────────────────────────────────────
    android::sp<IMapper> ImageBuf::mMapper;
    android::sp<IAllocator> ImageBuf::mAllocator;

// ──────────────────────────────────────────────────────────
// 內部 field accessor helpers（匹配各架構 ABI layout）
//
// 64-bit 佈局（來自 Ghidra libcacao_client.so 64-bit 分析）：
//   +0x00: vtable ptr (8 bytes)
//   +0x08: mWidth  (uint32_t)
//   +0x0C: mHeight (uint32_t)
//   +0x10: mHandle (void*, 8 bytes)
//   +0x18: mMappedAddr (void*, 8 bytes)
//   +0x20: mBufSize/stride (uint32_t)
//   +0x24: mDataSpace (uint32_t)
//   +0x28: mDataSize (uint32_t)
//   +0x2C: mOwnFlag (uint8_t)
//   +0x2D: mDirty (bool)
//   +0x2E: _pad[2]
//   +0x30: mSharedBuf (SharedBuffer*, 8 bytes)
//   sizeof = 0x38
//
// 32-bit 佈局（來自 Ghidra libcacao_client.so 32-bit 分析）：
//   +0x00: vtable ptr (4 bytes)
//   +0x04: mWidth  (uint32_t)
//   +0x08: mHeight (uint32_t)
//   +0x0C: mHandle (void*, 4 bytes)
//   +0x10: mMappedAddr (void*, 4 bytes)
//   +0x14: mBufSize/stride (uint32_t)
//   +0x18: mDataSpace (uint32_t)
//   +0x1C: mDataSize (uint32_t)
//   +0x20: mOwnFlag (uint8_t)
//   +0x21: mDirty (bool)
//   +0x22: _pad[2]
//   +0x24: mSharedBuf (SharedBuffer*, 4 bytes)
//   sizeof = 0x28
// ──────────────────────────────────────────────────────────
#ifdef __LP64__
#define IB_WIDTH_OFF 0x08
#define IB_HEIGHT_OFF 0x0C
#define IB_HANDLE_OFF 0x10
#define IB_MAPPED_OFF 0x18
#define IB_BSIZ_OFF 0x20
#define IB_DS_OFF 0x24
#define IB_DSIZ_OFF 0x28
#define IB_OWN_OFF 0x2C
#define IB_DIRTY_OFF 0x2D
#define IB_SB_OFF 0x30
#else
#define IB_WIDTH_OFF 0x04
#define IB_HEIGHT_OFF 0x08
#define IB_HANDLE_OFF 0x0C
#define IB_MAPPED_OFF 0x10
#define IB_BSIZ_OFF 0x14
#define IB_DS_OFF 0x18
#define IB_DSIZ_OFF 0x1C
#define IB_OWN_OFF 0x20
#define IB_DIRTY_OFF 0x21
#define IB_SB_OFF 0x24
#endif

#define IB(b, offset, T) (*reinterpret_cast<T *>(reinterpret_cast<char *>(b) + (offset)))
#define IBC(b, offset, T) (*reinterpret_cast<const T *>(reinterpret_cast<const char *>(b) + (offset)))

    static inline uint32_t &ibWidth(ImageBuf *b) { return IB(b, IB_WIDTH_OFF, uint32_t); }
    static inline uint32_t &ibHeight(ImageBuf *b) { return IB(b, IB_HEIGHT_OFF, uint32_t); }
    static inline void *&ibHandle(ImageBuf *b) { return IB(b, IB_HANDLE_OFF, void *); }
    static inline void *&ibMapped(ImageBuf *b) { return IB(b, IB_MAPPED_OFF, void *); }
    static inline uint32_t &ibBufSize(ImageBuf *b) { return IB(b, IB_BSIZ_OFF, uint32_t); }
    static inline uint32_t &ibDS(ImageBuf *b) { return IB(b, IB_DS_OFF, uint32_t); }
    static inline uint32_t &ibDataSize(ImageBuf *b) { return IB(b, IB_DSIZ_OFF, uint32_t); }
    static inline uint8_t &ibOwn(ImageBuf *b) { return IB(b, IB_OWN_OFF, uint8_t); }
    static inline bool &ibDirty(ImageBuf *b) { return IB(b, IB_DIRTY_OFF, bool); }
    static inline SharedBuffer *&ibSB(ImageBuf *b) { return IB(b, IB_SB_OFF, SharedBuffer *); }

    // const 版本（指標型別需透過 const_cast 讀取，因物件本身存的是非 const 指標）
    static inline uint32_t ibWidth_c(const ImageBuf *b) { return IBC(b, IB_WIDTH_OFF, uint32_t); }
    static inline uint32_t ibHeight_c(const ImageBuf *b) { return IBC(b, IB_HEIGHT_OFF, uint32_t); }
    static inline void *ibHandle_c(const ImageBuf *b) { return IB(const_cast<ImageBuf *>(b), IB_HANDLE_OFF, void *); }
    static inline uint32_t ibBufSize_c(const ImageBuf *b) { return IBC(b, IB_BSIZ_OFF, uint32_t); }
    static inline uint32_t ibDS_c(const ImageBuf *b) { return IBC(b, IB_DS_OFF, uint32_t); }
    static inline uint32_t ibDataSize_c(const ImageBuf *b) { return IBC(b, IB_DSIZ_OFF, uint32_t); }
    static inline bool ibDirty_c(const ImageBuf *b) { return IBC(b, IB_DIRTY_OFF, bool); }
    static inline SharedBuffer *ibSB_c(const ImageBuf *b) { return IB(const_cast<ImageBuf *>(b), IB_SB_OFF, SharedBuffer *); }

    // ──────────────────────────────────────────────────────────
    // getMapper / getAllocator
    // Ghidra 5338-5425: 檢查 singleton 是否已初始化，若無則呼叫 getService
    // 注意：原始 .so 使用靜態 guard（pthread ABI style），此處用 sp<> null check
    // ──────────────────────────────────────────────────────────

    // getMapper — 初始化 IMapper singleton
    // Ghidra 5338-5395: 若 mMapper == null 則 getService("default", false)
    // passthrough 模式（!isRemote），否則回傳錯誤
    int ImageBuf::getMapper()
    {
        if (mMapper == nullptr)
        {
            mMapper = IMapper::getService("default", false);
            if (mMapper == nullptr || mMapper->isRemote())
            {
                ALOGE("CacaoImageBuf: gralloc-mapper must be in passthrough mode");
                mMapper = nullptr;
                return -0x6f;
            }
        }
        return 0;
    }

    // getAllocator — 初始化 IAllocator singleton
    // Ghidra 5398-5462: 若 mAllocator == null 則 getService("default", false)
    int ImageBuf::getAllocator()
    {
        if (mAllocator == nullptr)
        {
            mAllocator = IAllocator::getService("default", false);
            if (mAllocator == nullptr)
            {
                ALOGE("CacaoImageBuf: gralloc-alloc is missing");
                return -0x6f;
            }
        }
        return 0;
    }

    // ──────────────────────────────────────────────────────────
    // 建構子 1 — default ctor（Ghidra 5207-5237）
    // 初始化所有欄位為 0/null，建立 SharedBuffer，取 mapper/allocator
    // ──────────────────────────────────────────────────────────
    ImageBuf::ImageBuf()
    {
        // 欄位清零（Ghidra: *(undefined8*)(this+4)=0 等）
        ibHandle(this) = nullptr;
        ibMapped(this) = nullptr;
        ibWidth(this) = 0;
        ibHeight(this) = 0;
        ibBufSize(this) = 0;
        ibDS(this) = 0;
        ibDataSize(this) = 0;
        ibOwn(this) = 0x01; // Ghidra: *(undefined2*)(this+0x20) = 0x101
        ibDirty(this) = false;
        ibSB(this) = nullptr;

        ibSB(this) = SharedBuffer::create();

        int r = getMapper();
        if (r >= 0)
        {
            getAllocator();
        }
    }

    // ──────────────────────────────────────────────────────────
    // 建構子 2 — void* data（Ghidra 5465-5545）
    // ImageBuf(void* data, ImageSize size, DataSpace ds, ImageFormat fmt)
    // 包裝已存在的 native_handle，呼叫 mapper 取得 importedHandle
    // ──────────────────────────────────────────────────────────
    ImageBuf::ImageBuf(void *data, ImageSize size, DataSpace ds, ImageFormat fmt)
    {
        int rc = 0;

        ibWidth(this) = size.width;
        ibHeight(this) = size.height;
        ibHandle(this) = data; // +0x0c = native handle ptr
        ibMapped(this) = nullptr;
        ibBufSize(this) = 0;
        ibDS(this) = static_cast<uint32_t>(ds);
        ibDataSize(this) = 0;
        ibOwn(this) = 0x00; // Ghidra: *(undefined2*)(this+0x20) = 0x100
        ibDirty(this) = false;
        ibSB(this) = nullptr;

        rc = getMapper();
        if (rc < 0)
        {
            return;
        }

        // 根據 ImageFormat 計算 bufSize（Ghidra 5521-5540）
        uint32_t fmt32 = static_cast<uint32_t>(fmt);
        uint32_t w = size.width, h = size.height;
        if (fmt32 == 0)
        {
            // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED-like default
            ibBufSize(this) = (((w + 0x7f) & ~0x7f) *
                               (((h / 2 + 0x1f) & ~0x1f) + ((h + 0x1f) & ~0x1f)));
        }
        else if (fmt32 == 0x1000001)
        {
            // JPEG/BLOB: w*h*2 + 0x10000
            ibBufSize(this) = w * h * 2 + 0x10000;
        }
        else if (fmt32 == 0x20001)
        {
            // NV21-like
            ibBufSize(this) = (((w + 0x7f) & ~0x7f) *
                               (((h / 2 + 0x1f) & ~0x1f) + ((h + 0x1f) & ~0x1f)));
        }
        // fmt32 != 0 的其他值不設定 bufSize（沿用 0 或 caller 會 setDataSize）

        // BH（msm8998 4.19 / lineage gralloc）的實際分配大小以 gralloc private_handle_t 的 size 為準。
        // 上面的公式（NV21 對齊 128/32）可能比實際分配大，cacao 會以它做 memcpy 而讀超出映射範圍。
        // 只在 handle 確定是 gralloc 的 private_handle_t 時才取 size（magic 'gmsm'、fds=2）。
        // 欄位位置：native_handle data[k] 位於 int index 3 + k；magic = data[2]，size = data[10]。
        {
            const int *hd = reinterpret_cast<const int *>(data);
            const int kGrallocMagic = 'gmsm';
            if (hd[1] == 2 && hd[2] >= 11 && hd[3 + 2] == kGrallocMagic && hd[3 + 10] > 0)
                ibBufSize(this) = static_cast<uint32_t>(hd[3 + 10]);
        }

        ibSB(this) = SharedBuffer::create();
    }

    // ──────────────────────────────────────────────────────────
    // 建構子 3 — ImageSize+stride（Ghidra 5464..5556）
    // ImageBuf(ImageSize size, uint stride, ImageFormat fmt, DataSpace ds)
    // 分配新 gralloc buffer
    // ──────────────────────────────────────────────────────────
    ImageBuf::ImageBuf(ImageSize size, unsigned int stride, ImageFormat fmt, DataSpace ds)
    {
        int rc = 0;
        uint32_t fmt32 = static_cast<uint32_t>(fmt);
        uint32_t w = size.width, h = size.height;

        ibWidth(this) = w;
        ibHeight(this) = h;
        ibHandle(this) = nullptr;
        ibMapped(this) = nullptr;
        ibBufSize(this) = stride; // stride 存在 +0x14 (Ghidra: *(uint*)(this+0x14)=param_4)
        ibDS(this) = static_cast<uint32_t>(ds);
        ibDataSize(this) = 0;
        ibOwn(this) = 0x01; // Ghidra: *(undefined2*)(this+0x20) = 0x101
        ibDirty(this) = false;
        ibSB(this) = nullptr;

        rc = getMapper();
        if (rc < 0)
            goto done;
        rc = getAllocator();
        if (rc < 0)
            goto done;

        {
            // 計算分配參數（Ghidra 5480-5505）
            uint32_t alloc_w = w, alloc_h = h, alloc_fmt = 0;
            uint32_t alloc_size = stride; // if stride != 0, use as size

            if (stride == 0)
            {
                if (fmt32 == 0x1000001)
                {
                    alloc_size = w * h * 2 + 0x10000;
                    alloc_fmt = 0x21; // HAL_PIXEL_FORMAT_YCbCr_420_888
                }
                else if (fmt32 == 0x20001)
                {
                    alloc_w = (h + 1) & ~1;
                    alloc_size = (w + 1) & ~1;
                    alloc_fmt = 0x11; // HAL_PIXEL_FORMAT_Y16
                }
                else
                {
                    alloc_w = (h + 1) & ~1;
                    alloc_size = (w + 1) & ~1;
                    alloc_fmt = 0x22; // HAL_PIXEL_FORMAT_YV12
                }
            }
            else
            {
                alloc_fmt = 0x21;
                alloc_w = 1;
            }

            // 呼叫 IAllocator::allocate（Ghidra 5506-5535）
            if (mAllocator != nullptr)
            {
                IMapper::BufferDescriptorInfo desc;
                desc.width = alloc_size;
                desc.height = alloc_w;
                desc.layerCount = 1;
                desc.format = static_cast<android::hardware::graphics::common::V1_0::PixelFormat>(alloc_fmt);
                desc.usage = 0x33; // GRALLOC_USAGE_SW_READ_OFTEN | SW_WRITE_OFTEN

                android::hardware::hidl_vec<uint32_t> descriptors;
                int alloc_rc = -1;
                android::hardware::hidl_vec<android::hardware::hidl_handle> handles;
                uint32_t stride_out = 0;

                mMapper->createDescriptor(desc,
                                          [&](MapperError err, const android::hardware::hidl_vec<uint32_t> &d)
                                          {
                                              if (err == MapperError::NONE)
                                              {
                                                  descriptors = d;
                                                  alloc_rc = 0;
                                              }
                                              else
                                              {
                                                  ALOGE("ImageBuf: createDescriptor failed");
                                                  alloc_rc = -0x6f;
                                              }
                                          });

                if (alloc_rc < 0)
                {
                    rc = alloc_rc;
                    goto done;
                }

                mAllocator->allocate(descriptors, 1,
                                     [&](android::hardware::graphics::mapper::V2_0::Error err,
                                         uint32_t s,
                                         const android::hardware::hidl_vec<android::hardware::hidl_handle> &hs)
                                     {
                                         if (err == android::hardware::graphics::mapper::V2_0::Error::NONE)
                                         {
                                             handles = hs;
                                             stride_out = s;
                                             alloc_rc = 0;
                                         }
                                         else
                                         {
                                             ALOGE("ImageBuf: allocate failed");
                                             alloc_rc = -0x6f;
                                         }
                                     });

                if (alloc_rc < 0)
                {
                    rc = alloc_rc;
                    goto done;
                }

                if (handles.size() > 0)
                {
                    // 匯入 handle 取得 process-local handle
                    void *imported_handle = nullptr;
                    mMapper->importBuffer(handles[0],
                                          [&](MapperError err, void *h)
                                          {
                                              if (err == MapperError::NONE)
                                              {
                                                  imported_handle = h;
                                                  alloc_rc = 0;
                                              }
                                              else
                                              {
                                                  ALOGE("ImageBuf: importBuffer failed");
                                                  alloc_rc = -0x6f;
                                              }
                                          });
                    if (alloc_rc < 0)
                    {
                        rc = alloc_rc;
                        goto done;
                    }
                    ibHandle(this) = imported_handle;
                }

                ibSB(this) = SharedBuffer::create();
            }
        }

    done:
        (void)rc;
    }

    // ──────────────────────────────────────────────────────────
    // Copy 建構子（Ghidra 5556-5600）
    // ──────────────────────────────────────────────────────────
    ImageBuf::ImageBuf(const ImageBuf &other)
    {
        ibWidth(this) = ibWidth_c(&other);
        ibHeight(this) = ibHeight_c(&other);
        ibHandle(this) = ibHandle_c(&other);
        ibMapped(this) = nullptr; // copy 的 mappedAddr 不繼承 (Ghidra 清零)
        ibBufSize(this) = ibBufSize_c(&other);
        ibDS(this) = ibDS_c(&other);
        ibDataSize(this) = ibDataSize_c(&other);
        ibOwn(this) = 0x00; // copy 不擁有 handle（Ghidra 清零後僅設 dirty）
        ibDirty(this) = ibDirty_c(&other);
        SharedBuffer *sb = ibSB_c(&other);
        if (sb)
            sb->incRefCnt();
        ibSB(this) = sb;
    }

    // ──────────────────────────────────────────────────────────
    // 解構子（Ghidra 5605-5650）
    // SharedBuffer::release → 若降至0：flush/清除 handle
    // ──────────────────────────────────────────────────────────
    ImageBuf::~ImageBuf()
    {
        int released = SharedBuffer::release(ibSB(this));
        if (released && mMapper != nullptr)
        {
            // 若 mappedAddr 不為 null → flushCache（虛擬呼叫 slot 11）
            if (ibMapped(this) != nullptr)
            {
                this->flushCache();
            }
            // 若 mOwnFlag 非0（此 .so 有 importBuffer）→ freeBuffer
            if (ibOwn(this) != 0 && ibHandle(this) != nullptr)
            {
                mMapper->freeBuffer(ibHandle(this));
                ibHandle(this) = nullptr;
                ibMapped(this) = nullptr;
            }
        }
    }

    // ──────────────────────────────────────────────────────────
    // Virtual 方法實作
    // ──────────────────────────────────────────────────────────

    // getClassName — vtable slot 3
    const char *ImageBuf::getClassName() const
    {
        return "ImageBuf";
    }

    // getNative — vtable slot 4，回傳 native handle ptr (this+0x0c)
    void *ImageBuf::getNative() const
    {
        return ibHandle_c(this);
    }

    // getBufSize — vtable slot 5
    // Ghidra 5655-5668: if(!isValid) log error; else return *(this+0x14)
    uint32_t ImageBuf::getBufSize() const
    {
        if (!isValid())
        {
            ALOGE("ImageBuf::getBufSize: invalid (handle=NULL)");
            return 0;
        }
        uint32_t sz = ibBufSize_c(this);
        if (sz == 0)
        {
        }
        return sz;
    }

    // getBufAddress — vtable slot 6
    // Ghidra 5670-5685: if(!isValid) log; if(*(+0x10)!=0) return; else clearCache()
    void *ImageBuf::getBufAddress()
    {
        if (!isValid())
        {
            ALOGE("ImageBuf::getBufAddress: invalid");
            return nullptr;
        }
        if (ibMapped(this) != nullptr)
        {
            return ibMapped(this);
        }
        clearCache(); // 虛擬呼叫 → slot 10
        return ibMapped(this);
    }

    // getImageSize — vtable slot 7
    // Ghidra 5688-5697: return { *(+4), *(+8) }
    ImageSize ImageBuf::getImageSize() const
    {
        return {ibWidth_c(this), ibHeight_c(this)};
    }

    // getDirtyFlag — vtable slot 8
    // Ghidra 5700-5710: if(!isValid) return false; return (this[0x21] != 0)
    bool ImageBuf::getDirtyFlag() const
    {
        if (!isValid())
            return false;
        return ibDirty_c(this);
    }

    // setDirtyFlag — vtable slot 9
    // Ghidra 5712-5723: if(!isValid) log; else this[0x21]=param
    void ImageBuf::setDirtyFlag(bool dirty)
    {
        if (!isValid())
        {
            ALOGE("ImageBuf::setDirtyFlag: invalid");
            return;
        }
        ibDirty(this) = dirty;
    }

    // clearCache — vtable slot 10
    // Ghidra 5725-5800: IMapper::lock → 取得 mappedAddr，清除 dirty
    void ImageBuf::clearCache()
    {
        if (!isValid() || mMapper == nullptr)
        {
            ALOGE("ImageBuf::clearCache: invalid or no mapper");
            return;
        }

        int rc = 0;
        void *mapped = nullptr;
        android::hardware::hidl_handle fence;
        IMapper::Rect region{0, 0,
                             static_cast<int32_t>(ibWidth(this)),
                             static_cast<int32_t>(ibHeight(this))};

        mMapper->lock(ibHandle(this),
                      0x66, // GRALLOC_USAGE_SW_READ_OFTEN | SW_WRITE_OFTEN
                      region, fence,
                      [&](MapperError err, void *data)
                      {
                          if (err == MapperError::NONE)
                          {
                              mapped = data;
                          }
                          else
                          {
                              ALOGE("ImageBuf::clearCache: IMapper::lock failed err=%d hdl=%p",
                                    (int)err, ibHandle(this));
                              rc = -0x6f;
                          }
                      });

        if (rc < 0)
            return;

        ibDirty(this) = false;
        ibMapped(this) = mapped;
    }

    // flushCache — vtable slot 11
    // Ghidra 5804-5840: IMapper::unlock → 釋放 mappedAddr，清除 dirty
    void ImageBuf::flushCache()
    {
        if (!isValid() || mMapper == nullptr)
        {
            ALOGE("ImageBuf::flushCache: invalid or no mapper");
            return;
        }

        int fd = -1;
        mMapper->unlock(ibHandle(this),
                        [&](MapperError err, const android::hardware::hidl_handle &release_fence)
                        {
                            if (err == MapperError::NONE)
                            {
                                // 取得 fence fd
                                if (release_fence.getNativeHandle() &&
                                    release_fence.getNativeHandle()->numFds > 0)
                                {
                                    fd = release_fence.getNativeHandle()->data[0];
                                }
                            }
                            else
                            {
                                ALOGE("ImageBuf::flushCache: IMapper::unlock failed");
                            }
                        });

        ibDirty(this) = false;
        if (fd >= 0)
        {
            close(fd);
        }
    }

    // flushAndClearCache — vtable slot 12
    // Ghidra 5841-5857: if(dirty && mapped) flushCache(); clearCache()
    void ImageBuf::flushAndClearCache()
    {
        if (ibDirty_c(this) && ibMapped(this) != nullptr)
        {
            int rc = 0;
            // 呼叫 flushCache via vtable（但直接呼叫亦可，保持與 Ghidra 一致）
            this->flushCache();
            if (!isValid())
                return; // guard
        }
        this->clearCache();
    }

    // getDataSize — vtable slot 13
    // Ghidra 5862-5866: return *(this+0x1c)
    uint32_t ImageBuf::getDataSize() const
    {
        return ibDataSize_c(this);
    }

    // setDataSize — vtable slot 14
    // Ghidra 5869-5880: if(!isValid) log; else *(this+0x1c)=size
    void ImageBuf::setDataSize(uint32_t size)
    {
        if (!isValid())
        {
            ALOGE("ImageBuf::setDataSize: invalid");
            return;
        }
        ibDataSize(this) = size;
    }

    // getDataSpace — vtable slot 15
    // Ghidra 5888-5892: return *(this+0x18)
    DataSpace ImageBuf::getDataSpace() const
    {
        return static_cast<DataSpace>(ibDS_c(this));
    }

    // ──────────────────────────────────────────────────────────
    // isValid — 非虛擬
    // Ghidra 5693-5700: if(*(+0x0c) != 0) return 1; else log+return 0
    // ──────────────────────────────────────────────────────────
    bool ImageBuf::isValid() const
    {
        if (ibHandle_c(this) != nullptr)
        {
            return true;
        }
        ALOGE("ImageBuf::isValid: no native handle");
        return false;
    }

} // namespace cacao
