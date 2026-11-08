// ICacao.cpp — android::ICacao Binder 介面實作
// 包含: ICacao, BpCacao, BnCacao::onTransact
// 依照 libcacao_client.so (ARM 32-bit) Ghidra 反編譯結果 100% 重建
// Interface descriptor: "com.sonymobile.cacao.ICacao"
//
// BnCacao::onTransact transaction codes:
//   code 1 → disconnect (no reply)
//   code 2 → start(sp<IMemory>, SerializedData) → int
//   code 3 → stop() → int
//   code 4 → setConfig(int configId, sp<IMemory>, SerializedData) → int
//   code 5 → getConfig(int configId, sp<IMemory>, SerializedData) → int
//   code 6 → process(sp<IMemory>, SD, Vector<ImageBuf*>, sp<IMemory>, SD, int, cb) → int
//   code 7 → process(sp<IMemory>, SD, sp<IBinder>, sp<IMemory>, SD, int, cb) → int
//   code 8 → cancel(int cookie) → int

#include <binder/IInterface.h>
#include <binder/IBinder.h>
#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <utils/Vector.h>
#include <log/log.h>
#include <unistd.h>
#include <string.h>

#include <cacao/ICacao.h>
#include <cacao/ICacaoProcessCallback.h>
#include <cacao/binder_sd_helpers.h>

#define LOG_TAG "ICacao"

namespace android
{

    namespace
    {

        static inline void write_reply_intptr(Parcel *reply, intptr_t value)
        {
#if INTPTR_MAX == INT64_MAX
            reply->writeInt64((int64_t)value);
#else
            reply->writeInt32((int32_t)value);
#endif
        }

        static inline intptr_t read_reply_intptr(Parcel &reply)
        {
#if INTPTR_MAX == INT64_MAX
            return (intptr_t)reply.readInt64();
#else
            return (intptr_t)reply.readInt32();
#endif
        }

    } // namespace

    // ══════════════════════════════════════════════════════════════
    // ICacao 基礎介面實作
    // ══════════════════════════════════════════════════════════════

    const String16 ICacao::descriptor("com.sonymobile.cacao.ICacao");

    ICacao::ICacao() {}
    ICacao::~ICacao() {}

    const String16 &ICacao::getInterfaceDescriptor() const
    {
        return descriptor;
    }

    // ══════════════════════════════════════════════════════════════
    // BpCacao — Binder Proxy
    // ══════════════════════════════════════════════════════════════

    class BpCacao : public BpInterface<ICacao>
    {
    public:
        explicit BpCacao(const sp<IBinder> &impl)
            : BpInterface<ICacao>(impl)
        {
        }

        virtual ~BpCacao() {}

        // code 1: disconnect() — 無 reply
        virtual void disconnect() override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            remote()->transact(1, data, &reply, 0);
        }

        // code 2: start(sp<IMemory>, SerializedData) → int
        // writeToken → writeStrongBinder(mem) → write_sd → transact(2) → readInt32
        virtual int start(const sp<IMemory> &mem,
                          cacao::ISerialize::SerializedData sd) override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            data.writeStrongBinder(IInterface::asBinder(mem));
            write_sd(data, sd);
            remote()->transact(2, data, &reply, 0);
            return reply.readInt32();
        }

        // code 3: stop() → int
        virtual int stop() override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            remote()->transact(3, data, &reply, 0);
            return reply.readInt32();
        }

        // code 4: setConfig(configId, sp<IMemory>, SerializedData) → int
        // writeToken → writeInt32(configId) → writeStrongBinder(mem) →
        // write_sd → transact(4) → readInt32
        virtual int setConfig(int configId, const sp<IMemory> &mem,
                              cacao::ISerialize::SerializedData sd) override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            data.writeInt32(configId);
            data.writeStrongBinder(IInterface::asBinder(mem));
            write_sd(data, sd);
            remote()->transact(4, data, &reply, 0);
            return reply.readInt32();
        }

        // code 5: getConfig(configId, sp<IMemory>, SerializedData) → int
        // 協議同 setConfig 但 code 5
        virtual int getConfig(int configId, const sp<IMemory> &mem,
                              cacao::ISerialize::SerializedData sd) override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            data.writeInt32(configId);
            data.writeStrongBinder(IInterface::asBinder(mem));
            write_sd(data, sd);
            remote()->transact(5, data, &reply, 0);
            return reply.readInt32();
        }

        // code 6: process(paramMem, paramSd, bufs, resultMem, resultSd, flags, cb) → int
        // 原始 .so BpCacao::process 實作:
        //   writeToken → writeStrongBinder(paramMem) → write_sd(paramSd)
        //   → writeInt32(bufsCount) → for each buf: writeUint32(w), writeUint32(h),
        //                              writeInt32(stride?), writeNativeHandle
        //   → writeStrongBinder(resultMem) → write_sd(resultSd)
        //   → writeInt32(flags) → writeStrongBinder(cb) → transact(6) → readAligned<intptr_t>()
        virtual int process(const sp<IMemory> &paramMem,
                            cacao::ISerialize::SerializedData paramSd,
                            Vector<cacao::ImageBuf *> bufs,
                            const sp<IMemory> &resultMem,
                            cacao::ISerialize::SerializedData resultSd,
                            int flags,
                            const sp<ICacaoProcessCallback> &callback) override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            data.writeStrongBinder(IInterface::asBinder(paramMem));
            write_sd(data, paramSd);
            uint32_t n = (uint32_t)bufs.size();
            data.writeInt32((int32_t)n);
            for (uint32_t i = 0; i < n; i++)
            {
                cacao::ImageBuf *buf = bufs[i];
                if (buf != nullptr)
                {
                    // 原始 .so 64-bit 反組譯:
                    //   vtable slot 7 (offset 0x38) = getImageSize() → 返回 ImageSize{w,h}
                    //     AArch64 ABI: 8-byte struct 在 x0 中返回, 低32=width 高32=height
                    //   vtable slot 15 (offset 0x78) = getDataSpace()
                    //   vtable slot 4 (offset 0x20) = getNative()
                    uint32_t w = 0, h = 0;
                    int32_t s = 0;
                    const native_handle_t *nh = nullptr;
                    typedef uint64_t (*GetImageSizeFn)(cacao::ImageBuf *);
                    typedef int32_t (*GetSFn)(cacao::ImageBuf *);
                    typedef const native_handle_t *(*GetNHFn)(cacao::ImageBuf *);
                    void **vt = *reinterpret_cast<void ***>(buf);
                    uint64_t imgSize = reinterpret_cast<GetImageSizeFn>(vt[7])(buf);
                    w = (uint32_t)(imgSize);
                    h = (uint32_t)(imgSize >> 32);
                    s = reinterpret_cast<GetSFn>(vt[15])(buf);
                    nh = reinterpret_cast<GetNHFn>(vt[4])(buf);
                    data.writeUint32(w);
                    data.writeUint32(h);
                    data.writeInt32(s);
                    data.writeNativeHandle(nh);
                }
                else
                {
                    data.writeUint32(0);
                    data.writeUint32(0);
                    data.writeInt32(0);
                    data.writeNativeHandle(nullptr);
                }
            }
            data.writeStrongBinder(IInterface::asBinder(resultMem));
            write_sd(data, resultSd);
            data.writeInt32(flags);
            data.writeStrongBinder(IInterface::asBinder(callback));
            remote()->transact(6, data, &reply, 0);
            return (int)read_reply_intptr(reply);
        }

        // code 7：原本傳 IGraphicBufferProducer 的 IBinder；已改為 AIDL 的 android.view.Surface，
        // 舊 libbinder 傳輸不再支援此呼叫。
        virtual int process(const sp<IMemory> &, cacao::ISerialize::SerializedData,
                            ANativeWindow *, const sp<IMemory> &,
                            cacao::ISerialize::SerializedData, int,
                            const sp<ICacaoProcessCallback> &) override
        {
            return -1;
        }

        // code 8: cancel(cookie) → int
        // 原始 .so BpCacao::cancel:
        //   writeToken → writeIntPtr(cookie) [32-bit 等同 writeInt32] → transact(8) → readInt32
        virtual int cancel(int cookie) override
        {
            Parcel data, reply;
            data.writeInterfaceToken(ICacao::getInterfaceDescriptor());
            data.writeInt32((int32_t)cookie);
            remote()->transact(8, data, &reply, 0);
            return reply.readInt32();
        }
    };

    // asInterface 手動實作
    sp<ICacao> ICacao::asInterface(const sp<IBinder> &obj)
    {
        sp<ICacao> intr;
        if (obj != nullptr)
        {
            intr = static_cast<ICacao *>(
                obj->queryLocalInterface(ICacao::descriptor).get());
            if (intr == nullptr)
            {
                intr = new BpCacao(obj);
            }
        }
        return intr;
    }

    // ══════════════════════════════════════════════════════════════
    // BnCacao::onTransact — Binder Stub 分派器
    // Server process 中，每個 BnCacao 實作（CacaoService::Client）的虛擬呼叫入口
    // ══════════════════════════════════════════════════════════════

    status_t BnCacao::onTransact(uint32_t code,
                                 const Parcel &data,
                                 Parcel *reply,
                                 uint32_t flags)
    {
        switch (code)
        {
        case 1:
        {
            // disconnect() — 無 reply
            if (!data.checkInterface(this))
                return -1;
            disconnect();
            return NO_ERROR;
        }
        case 2:
        {
            // start(sp<IMemory>, SerializedData) → int
            // 原始 .so: checkInterface → readStrongBinder(IMemory) →
            //           readInt32(fdCount) → readFDs → reconstruct SD → start → writeInt32
            if (!data.checkInterface(this))
                return -1;
            sp<IMemory> mem = interface_cast<IMemory>(data.readStrongBinder());
            cacao::ISerialize::SerializedData sd;
            reconstruct_sd(data, mem, sd);
            int ret = start(mem, sd);
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        case 3:
        {
            // stop() → int
            if (!data.checkInterface(this))
                return -1;
            int ret = stop();
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        case 4:
        {
            // setConfig(configId, sp<IMemory>, SerializedData) → int
            if (!data.checkInterface(this))
                return -1;
            int configId = data.readInt32();
            sp<IMemory> mem = interface_cast<IMemory>(data.readStrongBinder());
            cacao::ISerialize::SerializedData sd;
            reconstruct_sd(data, mem, sd);
            int ret = setConfig(configId, mem, sd);
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        case 5:
        {
            // getConfig(configId, sp<IMemory>, SerializedData) → int
            if (!data.checkInterface(this))
                return -1;
            int configId = data.readInt32();
            sp<IMemory> mem = interface_cast<IMemory>(data.readStrongBinder());
            cacao::ISerialize::SerializedData sd;
            reconstruct_sd(data, mem, sd);
            int ret = getConfig(configId, mem, sd);
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        case 6:
        {
            // process(paramMem, paramSd, Vector<ImageBuf*>, resultMem, resultSd, flags, cb) → int
            // 原始 .so 反編譯順序:
            //   readStrongBinder(paramMem) → readFDs(paramSd)
            //   readInt32(bufCount) → for each: readUint32, readUint32, readInt32, readNativeHandle
            //       → new ImageBuf(handle, w, h, ...) → insertAt(vector)
            //   readStrongBinder(resultMem) → readFDs(resultSd)
            //   readInt32(flags) → readStrongBinder(cb)
            //   → process(...) → write(result)
            if (!data.checkInterface(this))
                return -1;
            sp<IMemory> paramMem = interface_cast<IMemory>(data.readStrongBinder());
            cacao::ISerialize::SerializedData paramSd;
            reconstruct_sd(data, paramMem, paramSd);

            uint32_t bufCount = (uint32_t)data.readInt32();
            Vector<cacao::ImageBuf *> bufs;
            for (uint32_t i = 0; i < bufCount; i++)
            {
                uint32_t w = data.readUint32();
                uint32_t h = data.readUint32();
                int32_t s = data.readInt32();
                const native_handle_t *nh = data.readNativeHandle();
                // 原始 .so: ObjectBase::operator new(0x28) 分配 ImageBuf
                // 使用帶有 native_handle 的建構子
                void *raw = cacao::ObjectBase::operator new(0x28);
                cacao::ImageBuf *buf = nullptr;
                if (raw != nullptr)
                {
                    buf = ::new (raw) cacao::ImageBuf(
                        const_cast<void *>(static_cast<const void *>(nh)),
                        cacao::ImageSize{w, h},
                        static_cast<cacao::DataSpace>(s),
                        (cacao::ImageFormat)0);
                    bufs.push_back(buf);
                }
                // native_handle 已傳給 ImageBuf，不需要另外 close
            }

            sp<IMemory> resultMem = interface_cast<IMemory>(data.readStrongBinder());
            cacao::ISerialize::SerializedData resultSd;
            reconstruct_sd(data, resultMem, resultSd);

            int processFlags = data.readInt32();
            sp<ICacaoProcessCallback> cb =
                ICacaoProcessCallback::asInterface(data.readStrongBinder());

            int ret = process(paramMem, paramSd, bufs, resultMem, resultSd, processFlags, cb);

            write_reply_intptr(reply, (intptr_t)ret);
            return NO_ERROR;
        }
        case 7:
            // 舊 libbinder 傳輸不再支援（影片錄製改走 AIDL 的 processToSurface）
            return UNKNOWN_TRANSACTION;
        case 8:
        {
            // cancel(cookie) → int
            // 原始 .so: checkInterface → readIntPtr(cookie) [32-bit = readInt32] → cancel(cookie) → writeInt32(ret)
            if (!data.checkInterface(this))
                return -1;
            int cookie = (int)data.readInt32();
            int ret = cancel(cookie);
            reply->writeInt32(ret);
            return NO_ERROR;
        }
        default:
            return BBinder::onTransact(code, data, reply, flags);
        }
    }

} // namespace android
