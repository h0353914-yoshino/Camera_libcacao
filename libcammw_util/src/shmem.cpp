// cammw_util 的 shmem buffer 系列重建。對照 kong 這輪 decompiled.c：
//   cammw_util_shmem_alloc_buf               @ 0x141fc
//   cammw_util_shmem_import_buf              @ 0x142b0
//   cammw_util_shmem_free_buf                @ 0x144c0
//   cammw_util_shmem_attach_image_buf        @ 0x14698
//   cammw_util_shmem_detach_image_buf        @ 0x14970
//   cammw_util_shmem_make_image_buf_from_handle @ 0x14a0c
//   cammw_util_shmem_alloc_image_buf         @ 0x13244
//
// cammw_util_shmem_clean_buf（@0x14e68）沒有轉譯完，這個函式只在
// gralloc-backed buffer 需要重新 lock/unlock 時才會走到（不是每次
// alloc/free 都會用到的路徑），先留 TODO，之後裝置測試如果卡在
// clean_buf 相關路徑再回來補。
//
// 這裡用的 cammw_util_image_buffer_t 是 bufmgr.cpp 定義的那個 24-byte
// 描述子（不是 gralloc_alloc/ashmem_alloc/heap_alloc 用的 104-byte
// cammw_buf_t——這兩套是分開的，shmem 系列全部操作 24-byte 版）。
//
// image_buffer_t 的欄位（從這些函式的存取反推）：
//   [0] base/mapped_addr, [1] size, [2] fd, [3] fd_metadata/descriptor,
//   [4] type(0=ashmem 1=gralloc 2=unsupported 3=heap),
//   [5]byte cleanup_flag=1
// attach_image_buf 額外用到 [0x10]/[0x18]（跟 image_buffer_t 不同的
// 更大結構，attach 專用的輸出格式，這裡照原樣用 byte offset 處理）。

#define LOG_TAG "cammw_util"

#include <gr_priv_handle.h>
#include "cammw_util_internal.h"

#include <cutils/ashmem.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <hardware/gralloc1.h>

extern "C" GRALLOC1_PFN_RELEASE g_release_func;
extern "C" GRALLOC1_PFN_RETAIN g_retain_func;
extern "C" GRALLOC1_PFN_UNLOCK g_unlock_func;
extern "C" GRALLOC1_PFN_LOCK g_lock_func;
extern "C" GRALLOC1_PFN_DESTROY_DESCRIPTOR g_destroy_descriptor_func;
// C++ linkage，跟 gralloc_alloc.cpp 那份宣告同理——不能加 extern "C"。
int cammw_util_gralloc1_del_descriptor_list(int fd, unsigned long long *out_descriptor);
extern "C" int cammw_util_mmap(int fd, uint32_t size, uint8_t **out_addr);
extern "C" int cammw_util_munmap(uint8_t *addr, uint32_t size);
extern "C" int cammw_util_init(void);
extern "C" int cammw_util_ashmem_alloc(uint32_t, uint32_t, int32_t, uint32_t, uint8_t, void *);
extern "C" int cammw_util_heap_alloc(uint32_t, uint32_t, int32_t, uint32_t, uint8_t, void *);
extern "C" int cammw_util_gralloc_alloc(int, uint32_t, uint32_t, int, uint32_t, uint32_t, uint8_t, void *);
extern "C" int cammw_util_gralloc_make_buf_from_private_handle(const void *, const int *, int, int,
                                                               int, uint32_t, const void *, uint8_t,
                                                               void *);

namespace
{
  // 惰性初始化用的 gralloc1 device getter（context.cpp 提供，直接沿用
  // 內部惰性初始化，不重複實作）。
  extern "C" int cammw_util_get_gralloc1_dev(gralloc1_device_t **out_device);

  gralloc1_device_t *get_device_or_null()
  {
    gralloc1_device_t *dev = nullptr;
    cammw_util_get_gralloc1_dev(&dev);
    return dev;
  }
} // namespace

extern "C" int cammw_util_shmem_alloc_buf(int type, uint32_t param2, int size, void *out)
{
  if (size == 0 || out == nullptr)
  {
    return -0x67;
  }
  int rc = cammw_util_init(); // 內部自己會做「已初始化就跳過」的判斷
  if (rc != 0)
  {
    return rc;
  }
  switch (type)
  {
  case 0:
    return cammw_util_ashmem_alloc(size, 1, 0, 3, 0, out);
  case 1:
    return cammw_util_gralloc_alloc(1, param2, size, 1, 0, 3, 0, out);
  case 2:
    return -0x6a;
  case 3:
    return cammw_util_heap_alloc(size, 1, 0, 3, 0, out);
  default:
    return -0x67;
  }
}

// param_1: 0=gralloc 匯入(?) / 1=ashmem 匯入 —— 照原版分支，param_1==1
// 走 gralloc 路徑其實是誤導名，這裡忠實照原本的數值分支，不強行取名。
extern "C" int cammw_util_shmem_import_buf(int type, uint32_t size, int src_fd,
                                           cammw_util_image_buffer_t *out_buf)
{
  int32_t *out = out_buf != nullptr ? out_buf->w : nullptr;
  if (out == nullptr)
  {
    return -0x67;
  }
  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }
  if (type - 2u < 2)
  {
    return -0x6a;
  }

  if (type == 1)
  {
    if (src_fd < 0)
    {
      ALOGE("E: %s: Invalid argument, hmem=%d", __FUNCTION__, src_fd);
      return -0x67;
    }
    int fd = dup(src_fd);
    out[0] = 0;
    out[1] = 0;
    out[2] = -1;
    out[3] = 0;
    out[4] = 4;
    reinterpret_cast<uint8_t *>(&out[5])[0] = 1;
    if (fd < 0)
    {
      return -0x67;
    }
    uint8_t *mapped = nullptr;
    if (cammw_util_mmap(fd, size, &mapped) != 0)
    {
      close(fd);
      return -0x67;
    }
    out[0] = reinterpret_cast<intptr_t>(mapped);
    out[1] = static_cast<int32_t>(size);
    out[2] = fd;
    out[3] = 0;
    out[4] = 1;
    return 0;
  }

  if (type != 0)
  {
    return -0x67;
  }
  if (src_fd < 0)
  {
    ALOGE("E: %s: Invalid argument, hmem=%d", __FUNCTION__, src_fd);
    return -0x67;
  }
  out[0] = 0;
  out[1] = 0;
  out[2] = -1;
  out[3] = 0;
  out[4] = 4;
  reinterpret_cast<uint8_t *>(&out[5])[0] = 1;
  int fd = dup(src_fd);
  if (fd < 0)
  {
    ALOGE("E: %s: Failed, dup fd=%d", __FUNCTION__, fd);
    return -0x67;
  }
  if (ashmem_set_prot_region(fd, PROT_READ | PROT_WRITE) < 0)
  {
    ALOGE("E: %s: Failed ashmem_set_prot_region", __FUNCTION__);
    close(fd);
    return -0x69;
  }
  const size_t page = getpagesize();
  const uint32_t region_size = static_cast<uint32_t>((page + size - 1) & ~(page - 1));
  uint8_t *mapped = nullptr;
  if (cammw_util_mmap(fd, region_size, &mapped) != 0)
  {
    close(fd);
    return -0x67;
  }
  out[0] = reinterpret_cast<intptr_t>(mapped);
  out[1] = static_cast<int32_t>(region_size);
  out[2] = fd;
  out[3] = 0;
  out[4] = 0;
  return 0;
}

extern "C" int cammw_util_shmem_free_buf(cammw_util_image_buffer_t *buf_desc)
{
  int32_t *buf = buf_desc != nullptr ? buf_desc->w : nullptr;
  if (buf == nullptr)
  {
    return -0x67;
  }
  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }

  switch (buf[4])
  {
  case 0:
  {
    if (buf[0] == 0)
    {
      ALOGE("E: %s: Invalid Arg", __FUNCTION__);
      return -0x67;
    }
    cammw_util_munmap(reinterpret_cast<uint8_t *>(static_cast<intptr_t>(buf[0])),
                      static_cast<uint32_t>(buf[1]));
    close(buf[2]);
    return 0;
  }
  case 1:
  {
    if (buf[0] == 0)
    {
      ALOGE("E: %s: Invalid Arg", __FUNCTION__);
      return -0x67;
    }
    gralloc1_device_t *device = get_device_or_null();
    if (device == nullptr)
    {
      ALOGE("E: %s: failed to get gralloc1 device", __FUNCTION__);
      return -0x6f;
    }
    if (buf[3] == 0)
    {
      cammw_util_munmap(reinterpret_cast<uint8_t *>(static_cast<intptr_t>(buf[0])),
                        static_cast<uint32_t>(buf[1]));
      close(buf[2]);
      buf[2] = -1;
    }
    else
    {
      unsigned long long descriptor = 0xffffffffffffffffULL;
      int rc2 = cammw_util_gralloc1_del_descriptor_list(buf[2], &descriptor);
      if (rc2 != 0)
      {
        return rc2;
      }
      int32_t release_fence = -1;
      g_unlock_func(device, reinterpret_cast<buffer_handle_t>(static_cast<intptr_t>(buf[3])),
                    &release_fence);
      uint32_t destroy_rc = g_destroy_descriptor_func(device, descriptor);
      if (destroy_rc > 0x7fffffffu)
      {
        ALOGE("E: %s: descriptor destroy is failed", __FUNCTION__);
      }
      uint32_t release_rc = g_release_func(device, reinterpret_cast<buffer_handle_t>(static_cast<intptr_t>(buf[3])));
      if (release_rc > 0x7fffffffu)
      {
        ALOGE("E: %s: Buffer free failed", __FUNCTION__);
        return -0x67;
      }
    }
    return 0;
  }
  case 2:
    return 0;
  case 3:
    if (buf[0] == 0)
    {
      ALOGE("E: %s: Invalid Arg", __FUNCTION__);
      return -0x67;
    }
    free(reinterpret_cast<void *>(static_cast<intptr_t>(buf[0])));
    return 0;
  default:
    return -0x67;
  }
}

// dispatch 到 ashmem/gralloc/heap 三種 backend 之一（type 2 不支援）。
extern "C" int cammw_util_shmem_alloc_image_buf(int type, uint32_t param2, uint32_t width,
                                                uint32_t height, int32_t format,
                                                uint32_t color_space, void *out)
{
  if (out == nullptr)
  {
    return -0x67;
  }
  memset(out, 0, 0x68);
  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }
  switch (type)
  {
  case 0:
    return cammw_util_ashmem_alloc(width, height, format, color_space, 1, out);
  case 1:
    return cammw_util_gralloc_alloc(1, param2, width, static_cast<int>(height), format,
                                    color_space, 1, out);
  case 2:
    return -0x6a;
  case 3:
    return cammw_util_heap_alloc(width, height, format, color_space, 1, out);
  default:
    return -0x67;
  }
}

// attach：把一個已存在的 image buffer（另一個 process 傳過來的描述）
// 複製一份、dup 兩個 fd，包成一個新的 private_handle 交給 gralloc1_retain。
// 這裡建的是要交給 gralloc 的 private_handle_t（不是解析既有 handle），
// 直接用本機 gralloc 的建構子，不自己排佈局。
extern "C" int cammw_util_shmem_attach_image_buf(const int32_t *src, uint32_t param2,
                                                 uint32_t param3, uint32_t format, const int32_t *src5,
                                                 int width, int height, int32_t *out)
{
  if (src == nullptr || out == nullptr)
  {
    return -0x67;
  }
  memset(out, 0, 0x68);

  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }

  const int src_type = src[4];
  if (src_type - 2u < 2 || src_type == 0)
  {
    return -0x6a;
  }
  if (src_type != 1)
  {
    return -0x67;
  }

  // -------- 格式對照表（跟 gralloc_alloc/ashmem_alloc 是同一份格式碼空間）--------
  int32_t gralloc_format;
  int32_t plane_w = width;
  int32_t plane_h = height;
  if (format < 0x800000)
  {
    if (format < 0x40001)
    {
      if (format < 0x20000)
      {
        if (format == 0 || format == 0x10000)
          return -0x6a;
        if (format != 0x10001)
          return -0x67;
        gralloc_format = 0x32315659;
      }
      else if (format == 0x20000)
      {
        gralloc_format = 0x109;
      }
      else if (format == 0x20001)
      {
        gralloc_format = 0x11;
      }
      else if (format == 0x40000)
      {
        gralloc_format = 0x10;
      }
      else
      {
        return -0x67;
      }
    }
    else if (format - 0x100000u < 4)
    {
      return -0x6a;
    }
    else if (format == 0x40001)
    {
      gralloc_format = 0x10b;
    }
    else if (format == 0x80000)
    {
      plane_w = height * width;
      plane_h = 1;
      gralloc_format = 0x21;
      goto build;
    }
    else
    {
      return -0x67;
    }
  }
  else if (format < 0x4000000)
  {
    if (format - 0x1000000u > 2)
    {
      if (format == 0x800000)
        return -0x6a;
      if (format != 0x2000000)
        return -0x67;
      return -0x6a;
    }
    gralloc_format = 0x21;
  }
  else
  {
    switch (format)
    {
    case 0x4000000:
      gralloc_format = 3;
      break;
    case 0x4000001:
      gralloc_format = 1;
      break;
    case 0x4000002:
      gralloc_format = 0x10d;
      break;
    case 0x4000003:
      gralloc_format = 0x10e;
      break;
    default:
      return -0x67;
    }
  }
  plane_h = height;
  plane_w = width;

build:
  {
    // 從來源的 image_buffer_t 拿 fd/fd_metadata dup 出來。
    auto *src_priv = reinterpret_cast<private_handle_t *const *>(
        reinterpret_cast<const uint8_t *>(src) + 0xc);
    int fd = dup(src[2]);
    if (fd < 0)
    {
      ALOGE("E: %s: dup fd is failed", __FUNCTION__);
      return -0x6f;
    }
    int fd_metadata = dup((*src_priv)->fd_metadata);
    if (fd_metadata < 0)
    {
      ALOGE("E: %s: dup fd_metadata is failed", __FUNCTION__);
      close(fd);
      return -0x6f;
    }

    // 建一個 handle 出來交給 gralloc1_retain。
    //
    // 直接用本機 gralloc 的 private_handle_t 建構子（gr_priv_handle.h），
    // 不再自己手寫結構與偏移：version / numFds / numInts / magic /
    // layer_count / base / gpuaddr / id 全部由建構子按本機佈局填好，
    // 我們只要提供真正屬於這個 buffer 的欄位。這樣哪天 CAF 改了
    // private_handle_t 的佈局，這裡會自動跟著對，不需要人工同步。
    //
    // width/height 的對應（之前這裡左右顛倒，是「拍出來整張全綠」的元凶）：
    //   原版 local_2c = param_7(height)、local_28 = param_6(width)；
    //   0x481e 把 sp+0xc(=local_2c) 寫進 +0x20(height)、
    //   0x486c 把 sp+0x10(=local_28) 寫進 +0x1c(width)。
    // gralloc 的 GetYUVPlaneInfo() 直接把 hnd->width 當 stride 用
    // （cstride = ALIGN(width/2,16)），一旦交換，chroma 指標就會指到
    // 沒被寫過的記憶體 —— U=V=0、B 通道恆為 0 的全綠畫面。
    //
    // 原版 0x482c：bic.w r1, r3, r5, lsr #26（r3=1、r5=format）決定 buffer_type；
    // 0x484a 從 literal pool 搬進 +0x60 的是 producer_usage=0x3b、
    // consumer_usage=0；0x4848 寫 layer_count=1。
    auto *handle = new private_handle_t(
        fd, fd_metadata, private_handle_t::PRIV_FLAGS_CLIENT_ALLOCATED,
        plane_w, plane_h, plane_w, plane_h, gralloc_format,
        ((static_cast<uint32_t>(format) >> 26) & 1u) ? 0 : 1,
        static_cast<unsigned int>(src[1]),
        // commonsys-intf 的 private_handle_t 只有單一 usage 欄位，
        // 取原版 producer_usage=0x3b；consumer_usage=0 不另外保存。
        static_cast<uint64_t>(0x3b));

    // 原版 puVar5[0xe] = *param_5，也就是 offset 要填 src5[0]（建構子預設 0）。
    // 先前這裡漏掉，offset 永遠是 0——但實測 GOLD 值可以是 0x6000
    // （fmt=0x4000003 那組）。offset 是隨 handle 攜帶、由 consumer 自己加的
    // 資料起點，錯了之後 chroma plane 就落在沒被寫過的記憶體上，
    // 拍出來整張全綠（U=V=0、B 通道恆為 0）。
    handle->offset = src5 != nullptr ? static_cast<uint32_t>(src5[0]) : 0u;

    gralloc1_device_t *device = get_device_or_null();
    if (device == nullptr)
    {
      close(fd);
      close(fd_metadata);
      ::operator delete(handle);
      return -0x6f;
    }

    // numInts 不用自己填：private_handle_t 的建構子已經寫成本機的
    // NumInts()。原版在 A9 上是 25（Sony A9 的 gr_priv_handle.h 沒有
    // #pragma pack(push,4)，sizeof=120），LineageOS 的 CAF 2019 版有 pack(4)、
    // sizeof=116 → 24。語意本來就是「本機 gralloc 的 NumInts」，交給建構子
    // 處理才不會有第二份需要人工同步的常數。

    // Android 15 相容措施：CAF 2019 的 BufferManager::ImportHandleLocked() 多了
    //
    //     hnd->size = lseek(hnd->fd, 0, SEEK_END);
    //     hnd->offset = 0;
    //     hnd->offset_metadata = 0;
    //
    // 註解寫「這些欄位沒有被傳輸」，但它們其實都在 private_handle_t 的 int
    // payload 裡。我們建的是「大 dmabuf 裡的一段子區域」（size=0xa000、
    // offset=0x6000）。
    //
    // 注意 offset 不是給 mmap 用的：IonAlloc::MapBuffer() 永遠是
    // mmap(0, size, ..., fd, 0)，offset 只進 debug log，所以 base 一直是整塊
    // dmabuf 的開頭。offset 是隨 handle 攜帶的資訊——「資料在 base + offset」
    // ——由讀 buffer 的一方自己加（gralloc 內部只有 CleanBuffer 的 cache
    // 維護範圍與 FreeBuffer 會用到它）。所以 offset 被歸零之後，consumer
    // 算出來的資料起點就少了 0x6000，chroma plane 落在從未寫入的記憶體上
    // → 拍出來整張全綠（U=V=0、B 通道恆為 0）。
    //
    // A9 原廠沒有這段覆寫：實機反組譯 SOV36 47.2.C.1.126 的
    // /vendor/lib/hw/gralloc.msm8998.so，BufferManager::ImportHandleLocked
    // 整個函式只有 140 bytes，匯入兩個 ion fd 之後只有
    //     vstr d16, [r4, #0x40]   ; base = 0
    //     vstr d16, [r4, #0x48]   ; base_metadata = 0
    // size(+0x34)/offset(+0x38)/offset_metadata(+0x3c) 完全不動，整個 library
    // 連 lseek 都沒有匯入。也就是說 A9 的 25 與 A15 的 24 本來就該等價，
    // 差別全在這三行。
    //
    // 實機量測（poplardcm，原廠 gralloc + numInts=24）：把 provider 記憶體裡
    // 60 個 flags=0x20000000 的 handle 撈出來比對，import 前 offset=0x6000、
    // import 後全部變成 0；size 因為 lseek 剛好回傳 0xa000 所以沒變。也就是
    // 真正致命的是 offset 被歸零，照片量出來 R=0、B=0（maxB=0）、G=0.535，
    // 就是 U=V=0 的純綠。
    //
    // 因為不能改 LineageOS 的 display HAL，就在呼叫端把值補回去：retain
    // 之後立刻把三個欄位寫回原值。ImportHandleLocked 只在「第一次 retain
    // 且 handle 尚未註冊」時執行（RetainBuffer 先查 handles_map_，命中就只
    // IncRef），而 handle 指標由我們自己持有且不變，所以補一次就夠。
    const uint32_t saved_size = handle->size;
    const uint32_t saved_offset = handle->offset;
    const uint32_t saved_offset_metadata = handle->offset_metadata;

    // 保持原版的寬鬆判斷（gralloc1_error_t 全是非負值，所以拒收其實會被當成
    // 成功往下走）。這是刻意的：retain 失敗不影響後續使用，真正要補的是
    // detach 沒關 fd。
    int retain_rc = g_retain_func(device, reinterpret_cast<buffer_handle_t>(handle));
    if (retain_rc < 0)
    {
      ALOGE("E: %s: g_retain_func failed %d", __FUNCTION__, retain_rc);
      close(fd);
      close(fd_metadata);
      ::operator delete(handle);
      return -0x6f;
    }

    handle->size = saved_size;
    handle->offset = saved_offset;
    handle->offset_metadata = saved_offset_metadata;

    out[2] = fd;
    reinterpret_cast<private_handle_t **>(out)[3] = handle; // word index 3 = 0xc bytes
    out[4] = src_type;
    reinterpret_cast<uint32_t *>(out)[6] = format;
    out[7] = static_cast<int32_t>(param2);
    out[8] = static_cast<int32_t>(param3);
    out[1] = src[1];
    out[9] = src5 != nullptr ? src5[0] : 0;
    out[10] = src5 != nullptr ? src5[1] : 0;
    out[0xb] = src5 != nullptr ? src5[2] : 0;
    out[0xc] = width;
    out[0xd] = height;
    out[0x11] = static_cast<int32_t>(param2);
    out[0x12] = static_cast<int32_t>(param3);
    out[0xb + 8] = src[4]; // = out[0x13]，沿用原本 param_1+4（來源 type）—— 待裝置驗證
    return 0;
  }
}

// 修過 fd 洩漏的 detach（跟 camera/cacao/libcammw_util_leakfix 的邏輯
// 一致，直接把驗證過的判斷邏輯搬進來，而不是照抄原版「完全不 close()」
// 的 bug）。
extern "C" int cammw_util_shmem_detach_image_buf(int32_t *buf)
{
  if (buf == nullptr)
  {
    return -0x67;
  }
  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }

  const int type = buf[4];
  if (type - 2u < 2 || type == 0)
  {
    return -0x6a;
  }
  if (type != 1)
  {
    return -0x67;
  }

  gralloc1_device_t *device = get_device_or_null();
  auto *handle = reinterpret_cast<private_handle_t *>(
      *reinterpret_cast<intptr_t *>(reinterpret_cast<uint8_t *>(buf) + 0xc));

  // fd 的生命週期交給 gralloc——這也是原版的作法。
  //
  // 曾經有一版在這裡自己 close()：那時 attach 把 numInts 寫成 A9 的 25，
  // 本機 gralloc（NumInts()=24）在 retain 時就拒收，handle 從來沒被登記，
  // ReleaseBuffer 自然也不會關 fd，開關相機十幾次就累積到 2600+ 個 fd。
  // 現在 attach 用 private_handle_t 的建構子填本機 NumInts()，retain 會成功、
  // handle 進到 gralloc 的 handles_map_，release 就會把兩個 ion fd 收乾淨，
  // 不需要（也不可以）再自己關，否則是 double close。
  if (device != nullptr)
  {
    uint32_t release_rc = g_release_func(device, reinterpret_cast<buffer_handle_t>(handle));
    if (release_rc > 0x7fffffffu)
    {
      ALOGE("E: %s: g_release_func failed %u", __FUNCTION__, release_rc);
    }
  }
  if (handle != nullptr)
  {
    delete handle;
  }
  return 0;
}

// 對照 decompiled.c @ 0x14e68 完整讀完了（先前只讀了前半段）。
//
// mode 語意（跟先前猜測的「mode<3 就 unlock」不一樣，訂正）：
//   mode 0 -> 直接 (re)lock，不先 unlock
//   mode 1 -> 只 unlock
//   mode 2 -> 先 unlock 再 lock（刷新 CPU 端指標）
//   mode >=3 -> Invalid Arg
// 不管哪條路徑，只要最後動作成功，就把 buf[5] 的最低位元組清成 0
// （這個旗標比較像「待清理」，不是「目前是否鎖著」——lock 跟 unlock
// 成功後都會清它）。
// type 只有 1（gralloc）會真的做事；0/2/3 一律回 -0x6a（UNSUPPORTED）。
extern "C" int cammw_util_shmem_clean_buf(cammw_util_image_buffer_t *buf_desc, uint32_t mode)
{
  int32_t *buf = buf_desc != nullptr ? buf_desc->w : nullptr;
  if (buf == nullptr)
  {
    return -0x67;
  }
  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }
  const int type = buf[4];
  if (type - 2u < 2 || type == 0)
  {
    return -0x6a;
  }
  // type == 1（gralloc）以外的值理論上不會走到這裡（上面已經濾掉
  // 0/2/3），但保留這個防呆，跟原版一樣只信任 type==1。
  if (type != 1)
  {
    return -0x6a;
  }
  if (buf[0] == 0)
  {
    ALOGE("E: %s: Invalid Arg", __FUNCTION__);
    return -0x67;
  }
  if (mode >= 3)
  {
    ALOGE("E: %s: Invalid Arg", __FUNCTION__);
    return -0x67;
  }
  gralloc1_device_t *device = get_device_or_null();
  if (device == nullptr)
  {
    ALOGE("E: %s: failed to get gralloc1 device", __FUNCTION__);
    return -0x6f;
  }
  auto handle = reinterpret_cast<buffer_handle_t>(static_cast<intptr_t>(buf[3]));
  int32_t fence = -1;

  if (mode != 0)
  {
    // mode 1 或 2：先 unlock。lock_flag(buf[5]) 已經是 0（沒鎖著）就跳過
    // unlock 直接當作成功。
    const bool already_unlocked = (reinterpret_cast<uint8_t *>(&buf[5])[0]) == 0;
    uint32_t unlock_rc = 0;
    if (!already_unlocked)
    {
      unlock_rc = g_unlock_func(device, handle, &fence);
    }
    if (unlock_rc != 0)
    {
      ALOGE("E: %s: gralloc1_unlock_failed, e %u", __FUNCTION__, unlock_rc);
      return -0x6f;
    }
    if (mode == 1)
    {
      reinterpret_cast<uint8_t *>(&buf[5])[0] = 0;
      return 0;
    }
    // mode == 2：繼續往下走到 (re)lock。
  }

  // mode 0，或 mode 2 unlock 成功後的 relock：原版這裡傳的 rect 是全 0
  // （不是像 gralloc_make_buf_from_private_handle 那樣傳真正的
  // width/height），語意上像是「整塊 buffer、不指定區域」的慣例用法。
  {
    const gralloc1_rect_t rect = {0, 0, 0, 0};
    void *mapped = nullptr;
    uint32_t lock_rc =
        g_lock_func(device, handle, /*producerUsage=*/0x66, /*consumerUsage=*/0, &rect, &mapped, fence);
    if (lock_rc != 0)
    {
      ALOGE("E: %s: gralloc1_lock_failed, err %u", __FUNCTION__, lock_rc);
      return -0x6f;
    }
    reinterpret_cast<uint8_t *>(&buf[5])[0] = 0;
    return 0;
  }
}

extern "C" int cammw_util_shmem_make_image_buf_from_handle(const void *handle, int width, int height,
                                                           uint32_t color_space, void *out)
{
  if (width == 0 || color_space >= 4 || handle == nullptr || height == 0 || out == nullptr)
  {
    ALOGE("E: %s: Invalid Arg", __FUNCTION__);
    return -0x67;
  }
  memset(out, 0, 0x68);

  int rc = cammw_util_init();
  if (rc != 0)
  {
    return rc;
  }
  gralloc1_device_t *device = get_device_or_null();
  if (device == nullptr)
  {
    ALOGE("E: %s: failed to get gralloc1 device", __FUNCTION__);
    return -0x6f;
  }

  auto *h = static_cast<const private_handle_t *>(handle);
  int32_t fmt = h->format;
  uint32_t pixel_format;
  if (fmt < 0x102)
  {
    if (fmt == 0x11)
    {
      pixel_format = 0x20001;
    }
    else if (fmt == 0x21)
    {
      pixel_format = 0x1000001;
    }
    else if (fmt == 0x23)
    {
      pixel_format = 0x20001; // 跟 0x11 同一條路徑，原版就是共用
    }
    else
    {
      ALOGE("E: %s: Unknown Format", __FUNCTION__);
      return -0x67;
    }
  }
  else if (fmt == 0x7fa30c04 || fmt == 0x102)
  {
    pixel_format = 0x20000;
  }
  else if (fmt == 0x113)
  {
    // NV21_ZSL(0x113) 是 NV21（CrCb / V-first），跟 0x11 同族，不是 0x20000。
    // 原版 0x4a74 先把 r1 預設成 0x20001，只有 0x7fa30c04(0x4ade) 與
    // 0x102(0x4aea) 會落到 0x4af0 改寫成 0x20000；0x113(0x4ae6) 是直接跳
    // 0x4afa 保留預設值。先前誤併進 0x20000 那組，等於把 V-first 的 ZSL
    // 快照 buffer 宣告成 U-first，消費端 chroma 讀反 → 橘色拍出來變藍色。
    pixel_format = 0x20001;
  }
  else
  {
    ALOGE("E: %s: Unknown Format", __FUNCTION__);
    return -0x67;
  }

  return cammw_util_gralloc_make_buf_from_private_handle(device, reinterpret_cast<const int *>(1),
                                                         width, height,
                                                         static_cast<int>(pixel_format), color_space,
                                                         handle, 1, out);
}

// 這兩個原版是 `bx lr` 的空函式（先前那輪、還有這輪 kong 都沒分析到，
// 但既有記錄講得很明確：純粹的 no-op stub，符號存在只是為了保留匯出
// 介面）。
extern "C" int cammw_util_shmem_register_buf(void *) { return 0; }
extern "C" int cammw_util_shmem_unregister_buf(void *) { return 0; }
