// ICacao.h — android::ICacao 連線介面宣告（純 C++ 抽象介面，傳輸由 AIDL 負責）

#pragma once
#include <android/native_window.h>
#include <utils/RefBase.h>
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
    class ICacao : public virtual RefBase
    {
    public:
        virtual ~ICacao() {}


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


} // namespace android