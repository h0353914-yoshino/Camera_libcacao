// CacaoAidlUtil.h — IMemory/SerializedData ↔ vendor.somc.cacao.CacaoBlob 轉換（header-only）
// libcacao_client（app 端 proxy）與 libcacao_service（server 端 stub）各自編入，避免跨函式庫匯出符號。
// 對外 C++ API 仍使用 sp<IMemory> + SerializedData，只有線上傳輸改成 AIDL 的 fd + size。
#pragma once

#include <android/binder_interface_utils.h>
#include <android/binder_parcel_utils.h>
#include <aidl/vendor/somc/cacao/CacaoBlob.h>
#include <binder/IMemory.h>
#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <cacao/ProcessCtrlGatewayBase.h>
#include <string.h>
#include <unistd.h>

namespace cacao_aidl
{

    using aidl::vendor::somc::cacao::CacaoBlob;

    // SerializedData 的前 4 bytes 為 fd 個數，後面接 fd 陣列（最多 95 個），與 binder_sd_helpers.h 相同。
    static constexpr int kMaxSdFds = 95;

    // IMemory + SD → CacaoBlob：dup 共享記憶體 heap 的 fd 送出，另帶 SD 內的 fd。
    static inline CacaoBlob toBlob(const android::sp<android::IMemory> &mem,
                                   const cacao::ISerialize::SerializedData &sd)
    {
        CacaoBlob b;
        b.size = 0;
        if (mem != nullptr)
        {
            android::sp<android::IMemoryHeap> heap = mem->getMemory(nullptr, nullptr);
            int fd = heap != nullptr ? heap->getHeapID() : -1;
            if (fd >= 0)
            {
                b.mem = ndk::ScopedFileDescriptor(::dup(fd));
                b.size = (int32_t)mem->size();
            }
        }
        const int *pad = reinterpret_cast<const int *>(&sd);
        int n = pad[0];
        if (n < 0)
            n = 0;
        if (n > kMaxSdFds)
            n = kMaxSdFds;
        for (int i = 0; i < n; i++)
        {
            b.fds.emplace_back(::dup(pad[i + 1]));
        }
        return b;
    }

    // CacaoBlob → IMemory + SD：以 fd 重建 MemoryHeapBase/MemoryBase（與對方映射同一塊記憶體）。
    static inline void fromBlob(const CacaoBlob &b, android::sp<android::IMemory> *mem,
                                cacao::ISerialize::SerializedData *sd)
    {
        memset(sd, 0, sizeof(*sd));
        int *pad = reinterpret_cast<int *>(sd);
        int n = (int)b.fds.size();
        if (n > kMaxSdFds)
            n = kMaxSdFds;
        pad[0] = n;
        for (int i = 0; i < n; i++)
        {
            pad[i + 1] = ::dup(b.fds[i].get());
        }
        mem->clear();
        if (b.mem.get() >= 0 && b.size > 0)
        {
            android::sp<android::MemoryHeapBase> heap =
                new android::MemoryHeapBase(b.mem.get(), (size_t)b.size, 0, 0);
            if (heap->getBase() != MAP_FAILED)
            {
                android::sp<android::IMemory> m = new android::MemoryBase(heap, 0, (size_t)b.size);
                *mem = m;
                sd->size = (uint32_t)b.size;
                sd->data = m->unsecurePointer();
            }
        }
    }

} // namespace cacao_aidl
