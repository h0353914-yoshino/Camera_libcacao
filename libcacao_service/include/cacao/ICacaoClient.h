// ICacaoClient.h — android::ICacaoClient 回呼介面宣告（純 C++ 抽象介面，傳輸由 AIDL 負責）

#pragma once
#include <utils/RefBase.h>
#include <binder/IMemory.h>
#include <cacao/ProcessCtrlGatewayBase.h>

namespace android
{

    // ICacaoClient — 相機客戶端回呼介面
    // BnCacaoClient::onTransact 在 libcacao_client.so 中
    // 虛函式順序從 BpCacaoClient proxy 推導
    class ICacaoClient : public virtual RefBase
    {
    public:
        virtual ~ICacaoClient() {}


        // vtable slot 4 — 通知事件
        virtual void notifyEvent(int eventType,
                                 const sp<IMemory> &mem,
                                 cacao::ISerialize::SerializedData sd) = 0;

    };


} // namespace android