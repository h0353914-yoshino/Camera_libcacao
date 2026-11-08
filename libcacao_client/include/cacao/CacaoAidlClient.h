// CacaoAidlClient.h — app 端 AIDL 橋接（實作在 src/CacaoAidlClient.cpp）
#pragma once

#include <utils/StrongPointer.h>
#include <cacao/ICacao.h>
#include <cacao/ICacaoService.h>

namespace android
{

    // 以 AIDL 連到 vendor.somc.cacao.ICacaoService/default，重試規則與原本 getService 相同。
    // onDied：服務死亡時在 binder 執行緒呼叫（取代 IBinder::DeathRecipient）。
    sp<ICacaoService> cacaoAidlGetService(void (*onDied)());

    // 判斷 sp<ICacao> 是否為 AIDL proxy，並掛／取消死亡通知（取代 asBinder()->linkToDeath）。
    bool cacaoAidlIsCacao(const sp<ICacao> &cacao);
    void cacaoAidlLinkCacaoDeath(const sp<ICacao> &cacao);
    void cacaoAidlUnlinkCacaoDeath(const sp<ICacao> &cacao);

} // namespace android
