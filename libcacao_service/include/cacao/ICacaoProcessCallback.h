// ICacaoProcessCallback.h — android::ICacaoProcessCallback 回呼介面宣告（純 C++ 抽象介面，傳輸由 AIDL 負責）

#pragma once
#include <utils/RefBase.h>
#include <binder/IMemory.h>
#include <utils/Vector.h>
#include <cacao/ProcessCtrlGatewayBase.h>

namespace android
{

    class ICacaoProcessCallback : public virtual RefBase
    {
    public:
        virtual ~ICacaoProcessCallback() {}


        // vtable slot 4 — transaction 1
        virtual void notifyProgress(const sp<IMemory> &mem,
                                    cacao::ISerialize::SerializedData sd) = 0;
        // vtable slot 5 — transaction 2
        virtual void notifyResult(const sp<IMemory> &mem,
                                  cacao::ISerialize::SerializedData sd,
                                  android::Vector<cacao::ImageBuf *> *bufs) = 0;

    };


} // namespace android