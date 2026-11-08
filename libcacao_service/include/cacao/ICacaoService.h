// ICacaoService.h — android::ICacaoService 介面宣告（純 C++ 抽象介面，傳輸由 AIDL 負責）

#pragma once
#include <utils/RefBase.h>
#include <binder/IMemory.h>
#include <cacao/ProcessCtrlGatewayBase.h>

namespace cacao
{
    namespace ProcessCtrlCaps
    {
        struct CameraIndex
        {
            int index; // 0 或 1
        };
    }
} // namespace cacao

namespace android
{

    class ICacao;
    class ICacaoClient;

    // ICacaoService 純虛介面
    // BnCacaoService::onTransact 中的 4 個 transaction code:
    //   code 1 → connect (vtable+0x10)
    //   code 2 → getServicePid (vtable+0x14)
    //   code 3 → getCaps (vtable+0x18)
    //   code 4 → getJpegBufferSize (vtable+0x1c)
    class ICacaoService : public virtual RefBase
    {
    public:
        virtual ~ICacaoService() {}


        // vtable slot 4 (offset +0x10) — tx code 1
        virtual sp<ICacao> connect(const sp<ICacaoClient> &client) = 0;
        // vtable slot 5 (offset +0x14) — tx code 2
        virtual int getServicePid() = 0;
        // vtable slot 6 (offset +0x18) — tx code 3
        virtual int getCaps(const cacao::ProcessCtrlCaps::CameraIndex &camIdx,
                            const sp<IMemory> &mem,
                            cacao::ISerialize::SerializedData sd) = 0;
        // vtable slot 7 (offset +0x1c) — tx code 4
        virtual int getJpegBufferSize(cacao::ImageSize size) = 0;
    };


} // namespace android