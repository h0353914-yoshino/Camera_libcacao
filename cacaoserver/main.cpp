// cacaoserver — cacao 相機服務的 vendor 常駐程式
//
// 取代 Sony 預編譯的 /system/bin/cacaoserver。原版 main 只做執行緒池設定與
// libcacao_service 的 instantiate()，這裡改用 libbinder_ndk（stable AIDL）：
//   - HIDL（vendor.somc.hardware.camera.*）的回呼仍走 hwbinder 執行緒池
//   - AIDL 服務 vendor.somc.cacao.ICacaoService/default 由 CacaoService::instantiate() 發佈

#define LOG_TAG "cacaoserver"

#include <android/binder_process.h>
#include <hidl/HidlTransportSupport.h>

// libcacao_service 匯出的 C 連結進入點（原版 cacaoserver 也是呼叫它）
extern "C" void instantiate();

int main()
{
    android::hardware::configureRpcThreadpool(1, false /* callerWillJoin */);
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    instantiate();
    ABinderProcess_joinThreadPool();
    return 0;
}
