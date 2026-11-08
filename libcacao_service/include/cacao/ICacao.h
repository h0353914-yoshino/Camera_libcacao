// ICacao.h — android::ICacao Binder 介面宣告
// ICacao 的 ctor/dtor/descriptor/BnCacao::onTransact 由 libcacao_client_real.so 提供
// 本檔案僅提供 CacaoService::Client 編譯所需的類型宣告
//
// BnCacao::onTransact 的 8 個 transaction code:
//   code 1 → disconnect (vtable+0x10)
//   code 2 → start (vtable+0x14)
//   code 3 → stop (vtable+0x18)
//   code 4 → setConfig (vtable+0x1c)
//   code 5 → getConfig (vtable+0x20)
//   code 6 → process with ImageBuf (vtable+0x24)
//   code 7 → process with ANativeWindow（原為 IGraphicBufferProducer 的 IBinder）
//   code 8 → cancel (vtable+0x2c)

#pragma once
#include <android/native_window.h>
#include <binder/IInterface.h>
#include <binder/IMemory.h>
#include <utils/Vector.h>
#include <cacao/ProcessCtrlGatewayBase.h>

namespace cacao
{
    class ImageBuf;
} // namespace cacao

namespace android
{

    class ICacaoProcessCallback;

    // ICacao — 單一相機連線的 Binder 介面
    class ICacao : public IInterface
    {
    public:
        ICacao();
        virtual ~ICacao();

        static sp<ICacao> asInterface(const sp<IBinder> &obj);
        static const String16 descriptor;
        virtual const String16 &getInterfaceDescriptor() const;

        // vtable slot 4 — tx code 1
        virtual void disconnect() = 0;
        // vtable slot 5 — tx code 2
        virtual int start(const sp<IMemory> &mem,
                          cacao::ISerialize::SerializedData sd) = 0;
        // vtable slot 6 — tx code 3
        virtual int stop() = 0;
        // vtable slot 7 — tx code 4
        virtual int setConfig(int configId, const sp<IMemory> &mem,
                              cacao::ISerialize::SerializedData sd) = 0;
        // vtable slot 8 — tx code 5
        virtual int getConfig(int configId, const sp<IMemory> &mem,
                              cacao::ISerialize::SerializedData sd) = 0;
        // vtable slot 9 — tx code 6
        virtual int process(const sp<IMemory> &paramMem,
                            cacao::ISerialize::SerializedData paramSd,
                            Vector<cacao::ImageBuf *> bufs,
                            const sp<IMemory> &resultMem,
                            cacao::ISerialize::SerializedData resultSd,
                            int flags,
                            const sp<ICacaoProcessCallback> &callback) = 0;
        // vtable slot 10 — tx code 7
        virtual int process(const sp<IMemory> &paramMem,
                            cacao::ISerialize::SerializedData paramSd,
                            ANativeWindow *window,
                            const sp<IMemory> &resultMem,
                            cacao::ISerialize::SerializedData resultSd,
                            int flags,
                            const sp<ICacaoProcessCallback> &callback) = 0;
        // vtable slot 11 — tx code 8
        virtual int cancel(int cookie) = 0;
    };

    class BnCacao : public BnInterface<ICacao>
    {
    public:
        virtual status_t onTransact(uint32_t code, const Parcel &data,
                                    Parcel *reply, uint32_t flags = 0);
    };

} // namespace android