// CacaoAidlServer.h — server 端 AIDL 橋接（實作在 src/CacaoAidlServer.cpp）
#pragma once

#include <utils/StrongPointer.h>
#include <cacao/ICacaoClient.h>

namespace android
{

    class CacaoService;

    // 以 AIDL 發佈 vendor.somc.cacao.ICacaoService/default，轉呼叫給 svc。
    bool cacaoAidlPublish(const sp<CacaoService> &svc);

    // app 端 ICacaoClient 是否為 AIDL proxy；以及對應的死亡通知掛／取消
    // （取代 IInterface::asBinder(client)->linkToDeath(service)）。
    bool cacaoAidlIsClient(const sp<ICacaoClient> &client);
    void cacaoAidlLinkClientDeath(const sp<ICacaoClient> &client, const sp<CacaoService> &svc);
    void cacaoAidlUnlinkClientDeath(const sp<ICacaoClient> &client);

} // namespace android
