// ProcessCtrlGatewayBase.cpp
// 依照 libcacao_process_ctrl_gateway.so 反編譯結果 100% 重建
// 所有實作參照原始 .so 邏輯

#include <cacao/ProcessCtrlGatewayBase.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <cutils/native_handle.h>

// 偏移存取巨集 (一般物件)
#define OBJ_U8(p, off) (reinterpret_cast<uint8_t *>(p) + (off))
#define OBJ_U32(p, off) (*reinterpret_cast<uint32_t *>(OBJ_U8(p, off)))
#define OBJ_I32(p, off) (*reinterpret_cast<int32_t *>(OBJ_U8(p, off)))
#define OBJ_PTR(p, off) (*reinterpret_cast<void **>(OBJ_U8(p, off)))

namespace cacao
{

/* ════════════════════════════════════════════════════════════════
 *  Request 實作
 *  物件佈局 (from Ghidra Request_ctor_00017804):
 *   +0x00  vtable
 *   +0x04  ProcessParamBase* mParam  (cloned via vtable[0x10])
 *   +0x08  Vector<ImageBuf*>* mImageBufs
 *   +0x0c  ICBProcessResultListener* mListener
 *   +0x10  ProcessResultBase* mResult
 *   +0x14  uint8_t mFlags
 *   +0x18  PAL_Msg_t* mReplyMsg
 *   +0x1c  cacao_pal::Mutex mMutex (size 0x88)
 *   +0xa4  uint32_t mState
 * ════════════════════════════════════════════════════════════════ */

// 偏移定義
#define REQ_PARAM(r) OBJ_PTR(r, 0x04)
#define REQ_IMAGEBUFS(r) OBJ_PTR(r, 0x08)
#define REQ_LISTENER(r) OBJ_PTR(r, 0x0c)
#define REQ_RESULT(r) OBJ_PTR(r, 0x10)
#define REQ_FLAGS(r) (*OBJ_U8(r, 0x14))
#define REQ_REPLYMSG(r) OBJ_PTR(r, 0x18)
#define REQ_MUTEX(r) (*reinterpret_cast<cacao_pal::Mutex *>(OBJ_U8(r, 0x1c)))
#define REQ_STATE(r) OBJ_U32(r, 0xa4)

    ProcessCtrlGatewayBase::Request::Request(
        const ProcessParamBase *param,
        android::Vector<ImageBuf *> *imageBufs,
        ICBProcessResultListener *listener,
        ProcessResultBase *result,
        uint8_t flags,
        const PAL_Msg_t *replyMsg)
    {
        // ObjectBase ctor 已由 C++ 語義呼叫
        // 對應 Ghidra Request_ctor_00017804
        REQ_PARAM(this) = nullptr;
        REQ_IMAGEBUFS(this) = (void *)imageBufs;
        REQ_RESULT(this) = (void *)result;
        REQ_LISTENER(this) = (void *)listener;
        REQ_FLAGS(this) = flags;
        REQ_REPLYMSG(this) = (void *)replyMsg;
        // 明確以 placement new 初始化 Mutex（char _pad 不觸發 C++ 自動構造）
        new (reinterpret_cast<uint8_t *>(this) + 0x1c) cacao_pal::Mutex();
        REQ_STATE(this) = 0; // STATE_IDLE
        // param clone: vtable[0x10](param) — param->clone()
        if (param)
        {
            uintptr_t **vtbl = *reinterpret_cast<uintptr_t ***>(const_cast<ProcessParamBase *>(param));
            void *cloned = reinterpret_cast<void *(*)(const void *)>(vtbl[0x10 / 4])(param);
            REQ_PARAM(this) = cloned;
        }
    }

    ProcessCtrlGatewayBase::Request::~Request()
    {
        // 對應 Ghidra Request_dtor_00017858
        // if (mParam) { mParam->decRef() — vtable[0x04] }
        void *param = REQ_PARAM(this);
        if (param)
        {
            int **vtbl = *reinterpret_cast<int ***>(param);
            reinterpret_cast<void (*)(void *)>(vtbl[0x04 / 4])(param);
        }
        // Ghidra: 明確呼叫 ~Mutex(this+0x1c)（mMutex 在 _pad 內，C++ 無法自動析構）
        reinterpret_cast<cacao_pal::Mutex *>(reinterpret_cast<uint8_t *>(this) + 0x1c)->~Mutex();
    }

    /* cancel — 對應 Ghidra Request_cancel_0001791c */
    __attribute__((stack_protect)) void ProcessCtrlGatewayBase::Request::cancel()
    {
        cacao_pal::AutoLock lock(REQ_MUTEX(this));
        switch (REQ_STATE(this))
        {
        case 0: // IDLE → CANCELED
            REQ_STATE(this) = 1;
            break;
        case 1: // already CANCELED
        case 3: // STOPPING
        case 4: // DONE
            break;
        case 2: // ACTIVE → STOPPING
            REQ_STATE(this) = 3;
            onRequestCancel();
            break;
        default:
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "%s: invalid command on state %lu", "cancel", (unsigned long)REQ_STATE(this));
            break;
        }
    }

    /* start — 對應 Ghidra Request_start_000179a8 */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::Request::start()
    {
        cacao_pal::AutoLock lock(REQ_MUTEX(this));
        if (REQ_STATE(this) == 0)
        {
            REQ_STATE(this) = 2; // ACTIVE
            return 0;
        }
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                     "%s: invalid command on state %lu", "start", (unsigned long)REQ_STATE(this));
        return (int)0xffffff91u;
    }

    /* finish — 對應 Ghidra Request_finish_00017a20 */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::Request::finish()
    {
        cacao_pal::AutoLock lock(REQ_MUTEX(this));
        // state 1,2,3 → DONE(4)
        if (REQ_STATE(this) - 1U < 3U)
        {
            REQ_STATE(this) = 4;
            return 0;
        }
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                     "%s: invalid command on state %lu", "finish", (unsigned long)REQ_STATE(this));
        return (int)0xffffff91u;
    }

    /* notifyResult — 對應 Ghidra Request_notifyResult_00017ac0
     * REF assembly:
     *   ldr r0,[r5,#0x10] → result; if(r0) vtable[0x14/4](result,err) → setResult
     *   ldr r0,[r5,#0x0c] → listener; if(r0 && result) vtable[0x0c/4](listener) → onResult
     *   if(flags && replyMsg) PAL_MsgReply(replyMsg, err)
     */
    void ProcessCtrlGatewayBase::Request::notifyResult(PAL_Err_t err)
    {
        // result->setResult(err) — vtable[0x14/4]
        if (REQ_RESULT(this))
        {
            int **vtbl = *reinterpret_cast<int ***>(REQ_RESULT(this));
            reinterpret_cast<void (*)(void *, PAL_Err_t)>(vtbl[0x14 / 4])(REQ_RESULT(this), err);
        }
        // listener->onHandleResult(result) — vtable[0x0c/4]
        // REF asm (17adc): blx r2 with r0=listener, r1=result
        if (REQ_LISTENER(this) && REQ_RESULT(this))
        {
            int **vtbl = *reinterpret_cast<int ***>(REQ_LISTENER(this));
            reinterpret_cast<void (*)(void *, void *)>(vtbl[0x0c / 4])(REQ_LISTENER(this), REQ_RESULT(this));
        }
        // if (mFlags && mReplyMsg) PAL_MsgReply(replyMsg, err)
        if (REQ_FLAGS(this) != 0 && REQ_REPLYMSG(this) != nullptr)
        {
            PAL_MsgReply(reinterpret_cast<PAL_Msg_t *>(REQ_REPLYMSG(this)), (int)err);
        }
    }

    /* notifyProgress — 對應 Ghidra Request_notifyProgress_00017a9e
     * REF:
     *   result(+0x10) → vtable[0x1c/4] → setProgress
     *   listener(+0x0c), if(listener && result) → vtable[0x08/4] → onProgress
     */
    void ProcessCtrlGatewayBase::Request::notifyProgress(int progress)
    {
        // result->setProgress(progress) — vtable[0x1c/4]
        void *result = REQ_RESULT(this);
        if (result)
        {
            int **vtbl = *reinterpret_cast<int ***>(result);
            reinterpret_cast<void (*)(void *, int)>(vtbl[0x1c / 4])(result, progress);
        }
        // listener->onHandleProgress(result) — vtable[0x08/4]
        // REF asm (17ab6): bx r2 with r0=listener, r1=result (tail call)
        void *listener = REQ_LISTENER(this);
        if (listener && result)
        {
            int **vtbl = *reinterpret_cast<int ***>(listener);
            reinterpret_cast<void (*)(void *, void *)>(vtbl[0x08 / 4])(listener, result);
        }
    }

    /* checkState — 對應 Ghidra Request_checkState_00017898 */
    __attribute__((stack_protect)) bool ProcessCtrlGatewayBase::Request::checkState(unsigned int state)
    {
        cacao_pal::AutoLock lock(REQ_MUTEX(this));
        return REQ_STATE(this) == state;
    }

    /* checkCancel — 對應 Ghidra Request_checkCancel_000178dc */
    __attribute__((stack_protect)) bool ProcessCtrlGatewayBase::Request::checkCancel()
    {
        cacao_pal::AutoLock lock(REQ_MUTEX(this));
        return REQ_STATE(this) == 3; // STATE_STOPPING
    }

    void ProcessCtrlGatewayBase::Request::onRequestCancel()
    {
        // 預設無操作 — 子類可覆寫
    }

/* ════════════════════════════════════════════════════════════════
 *  RequestList 實作
 *  物件佈局 (from Ghidra RequestList_ctor_00017af4):
 *   +0x00  vtable (PtrLockList<Request>)
 *   +0x04  Mutex (size 0x88)
 *   +0x8c  vtable (embedded PtrList)
 *   +0x90  uint32_t count
 *   +0x94  Node* head
 *   +0x98  Node* tail
 *   +0x9c  Condition (linked to Mutex at +0x04)
 *  總大小: 0x128
 * ════════════════════════════════════════════════════════════════ */

// 偏移
#define RL_MUTEX(r) (*reinterpret_cast<cacao_pal::Mutex *>(OBJ_U8(r, 0x04)))
#define RL_COND(r) (*reinterpret_cast<cacao_pal::Condition *>(OBJ_U8(r, 0x9c)))
#define RL_COUNT(r) OBJ_U32(r, 0x90)
#define RL_HEAD(r) OBJ_PTR(r, 0x94)
#define RL_TAIL(r) OBJ_PTR(r, 0x98)

// Node 佈局: +0x00 ??, +0x04 data(Request*), +0x08 next
#define NODE_DATA(n) (*reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(n) + 4))
#define NODE_NEXT(n) (*reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(n) + 8))

    ProcessCtrlGatewayBase::RequestList::RequestList()
        : mCond(mMutex) /* 初始化 Condition，連結到 PtrLockList 的 mMutex */
    {
    }

    ProcessCtrlGatewayBase::RequestList::~RequestList()
    {
        /* mCond 由 C++ 自動析構；PtrLockListDel dtor 清理列表及 Mutex */
    }

    /* cancelAll — 對應 Ghidra RequestList_cancelAll_00017b90 */
    void ProcessCtrlGatewayBase::RequestList::cancelAll()
    {
        cacao_pal::AutoLock lock(RL_MUTEX(this));
        uint32_t count = RL_COUNT(this);
        if (count != 0)
        {
            void *node = RL_HEAD(this);
            uint32_t i = 0;
            do
            {
                Request *req = reinterpret_cast<Request *>(NODE_DATA(node));
                req->cancel();
                count = RL_COUNT(this);
                node = NODE_NEXT(node);
                i++;
            } while (i < count);
            // 等所有請求完成
            while (RL_COUNT(this) != 0)
            {
                // Condition::wait — 使用 Mutex 內的 Condition
                // 原始碼等待直到 count 歸零
                // 此處因 Condition 在 offset +0x9c，直接 wait
                reinterpret_cast<cacao_pal::Condition *>(OBJ_U8(this, 0x9c))->wait();
            }
        }
    }

    /* cancel(req) — 對應 Ghidra RequestList_cancel_00017c08 */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::RequestList::cancel(Request *req)
    {
        cacao_pal::AutoLock lock(RL_MUTEX(this));
        uint32_t count = RL_COUNT(this);
        if (count != 0)
        {
            void *node = RL_HEAD(this);
            uint32_t i = 0;
            do
            {
                if (reinterpret_cast<Request *>(NODE_DATA(node)) == req)
                {
                    if ((int)i >= 0)
                    {
                        req->cancel();
                        return 0;
                    }
                    break;
                }
                node = NODE_NEXT(node);
                i++;
            } while (i < count);
        }
        return (int)0xffffff98u;
    }

    /* start(req) — 對應 Ghidra RequestList_start_00017c94 */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::RequestList::start(Request *req)
    {
        cacao_pal::AutoLock lock(RL_MUTEX(this));
        int err = (int)0xffffff98u;
        uint32_t count = RL_COUNT(this);
        if (count != 0)
        {
            void *node = RL_HEAD(this);
            uint32_t i = 0;
            do
            {
                if (reinterpret_cast<Request *>(NODE_DATA(node)) == req)
                {
                    if ((int)i >= 0)
                    {
                        err = req->start();
                    }
                    break;
                }
                node = NODE_NEXT(node);
                i++;
            } while (i < count);
        }
        return err;
    }

    /* finish(req) — 對應 Ghidra RequestList_finish_00017cfc */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::RequestList::finish(Request *req)
    {
        cacao_pal::AutoLock lock(RL_MUTEX(this));
        int err = (int)0xffffff98u;
        uint32_t count = RL_COUNT(this);
        if (count != 0)
        {
            void *node = RL_HEAD(this);
            uint32_t i = 0;
            do
            {
                if (reinterpret_cast<Request *>(NODE_DATA(node)) == req)
                {
                    if ((int)i >= 0)
                    {
                        err = req->finish();
                    }
                    break;
                }
                node = NODE_NEXT(node);
                i++;
            } while (i < count);
        }
        return err;
    }

    /* remove(req) — 對應 Ghidra RequestList_remove_00017d64 */
    __attribute__((stack_protect)) int ProcessCtrlGatewayBase::RequestList::remove(Request *req)
    {
        cacao_pal::AutoLock lock(RL_MUTEX(this));
        void *foundNode = nullptr;
        uint32_t count = RL_COUNT(this);
        if (count != 0)
        {
            void *node = RL_HEAD(this);
            uint32_t i = 0;
            do
            {
                if (NODE_DATA(node) == (void *)req)
                {
                    foundNode = node;
                    goto found;
                }
                node = NODE_NEXT(node);
                i++;
            } while (i < count);
        }
        foundNode = nullptr;
    found:
        // removeNode via vtable[8]
        int **vtbl = *reinterpret_cast<int ***>(this);
        reinterpret_cast<void (*)(void *, void *)>(vtbl[0x08 / 4])(this, foundNode);
        // Condition::signal
        reinterpret_cast<cacao_pal::Condition *>(OBJ_U8(this, 0x9c))->signal();
        return 0;
    }

    bool ProcessCtrlGatewayBase::RequestList::compareRequestState(Request *item, void *state)
    {
        uint32_t *pState = reinterpret_cast<uint32_t *>(state);
        return item->checkState(*pState);
    }

    /* ════════════════════════════════════════════════════════════════
     *  ProcessCtrlGatewayBase 實作
     * ════════════════════════════════════════════════════════════════ */

    ProcessCtrlGatewayBase::ProcessCtrlGatewayBase()
        : cacao::ObjectBase(), mThread(nullptr), mQueId(0), mPriority(0x78), _pad0(0), mListener(nullptr), mListenerId(0), mInitFlag(0)
    {
        PAL_Create();
    }

    ProcessCtrlGatewayBase::~ProcessCtrlGatewayBase()
    {
        // Ghidra @ A9 原廠 D2Ev 0x7f0c，同樣把每個 bl 的 veneer/PLT 解到符號：
        //   7f20 -> PAL_Delete
        //   7f28 -> cacao_pal::Mutex::~Mutex
        //   7f3e / 7f44 -> (內部成員解構)
        //   7f4c -> ProcessCtrlGatewayBase::RequestList::~RequestList
        //   7f54 -> cacao_pal::Mutex::~Mutex
        //   7f5e -> cacao::ObjectBase::~ObjectBase
        // 沒有 deinit()、也沒有讀 mInitFlag——原版在這一層就是不做這件事。
        //
        // deinit() 由衍生類別 ~ProcessCtrlGateway()（見該檔案）在解構最一開始
        // 呼叫一次；原版的順序就是「先 deinit() 收掉 worker thread，再
        // PAL_Delete()」，這裡不需要、也不該再呼叫一次——曾經在這裡加過
        // `if (mInitFlag != 0) { deinit(); }` 防護（理由是擔心 client
        // binderDied 時直接 delete、deinit() 沒機會跑到），結果變成一次解構
        // 呼叫兩次 deinit()：第二次 queue 已經不在，PAL_MsgSendRecv 回
        // InvalidArg，接著對已經 join 過的 handle 再 PAL_ThreadClose，
        // pthread_join 拿到失效的 pthread_t，SIGABRT。
        PAL_Delete();
    }

    /* init(short) — 對應 Ghidra GatewayBase_init_00017f74 */
    int ProcessCtrlGatewayBase::init(short priority)
    {
        cacao_pal::AutoLock lock(mStateMutex);

        int err;
        if (mInitFlag != 0)
        {
            err = PAL_ERR_ALREADY;
        }
        else
        {
            // 與原版同：threadName buffer 和 msg struct 共用同一塊 stack 空間
            union
            {
                char threadName[0x100];
                struct
                {
                    uint32_t msg_id;
                    uint32_t r0, r1;
                    PAL_QueId_t dest;
                    uint32_t r2;
                    uint32_t size;
                } msg;
            } buf;

            PAL_MemSet(buf.threadName, 0, 0x100);
            const char *name = getClassName();
            PAL_StrNPrintf(buf.threadName, 0xff, "%s", name);

            err = PAL_ThreadCreateWithMsgQue(buf.threadName, (int)priority,
                                             reinterpret_cast<PAL_ThreadCallback_t>(threadDispatchMsg),
                                             this, &mThread);
            if (err >= 0)
            {
                mQueId = PAL_ThreadGetQueId(mThread);
                mPriority = priority;

                buf.msg.r0 = 0;
                buf.msg.r1 = 0;
                buf.msg.r2 = 0;
                buf.msg.msg_id = PAL_MSG_INIT;
                buf.msg.dest = mQueId;
                buf.msg.size = 0x18;

                err = PAL_MsgSendRecv(reinterpret_cast<PAL_Msg_t *>(&buf.msg));
                if (err < 0)
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "MsgSendRecv error <%s>",
                                 PAL_ErrToString(err));
                }
                else
                {
                    mInitFlag = 1;
                }
            }
        }
        return err;
    }

    /* deinit — 對應 Ghidra GatewayBase_deinit_00018094 */
    int ProcessCtrlGatewayBase::deinit()
    {
        stop();

        unsigned int ret = (unsigned int)PAL_ERR_NO_INIT;
        {
            cacao_pal::AutoLock lock(mStateMutex);

            if (mInitFlag == 1)
            {
                struct
                {
                    uint32_t msg_id;
                    uint32_t r0, r1;
                    PAL_QueId_t dest;
                    uint32_t r2;
                    uint32_t size;
                } msg;
                msg.r0 = 0;
                msg.r1 = 0;
                msg.r2 = 0;
                msg.msg_id = PAL_MSG_DEINIT;
                msg.dest = mQueId;
                msg.size = 0x18;

                ret = (unsigned int)PAL_MsgSendRecv(reinterpret_cast<PAL_Msg_t *>(&msg));
                if ((int)ret < 0)
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "MsgSendRecv error <%s>",
                                 PAL_ErrToString((PAL_Err_t)ret));
                }
            }
        } // lock 在這裡釋放，跟原版一樣在 PAL_ThreadClose 前就放掉

        // Ghidra @ A9 原廠 0x8094 逐指令核對到底：0x810c AutoLock::~AutoLock()
        // 先跑，然後 0x8110/0x8112 無條件 PAL_ThreadClose(mThread)，沒有
        // null 檢查，函式結束前也不寫回 mThread/mQueId/mInitFlag 任何一個
        // 欄位。原版能這樣寫，是因為 deinit() 在原版架構上保證一輩子只會被
        // 呼叫一次——唯一呼叫處是 ~ProcessCtrlGateway()，呼叫完物件緊接著就
        // 解構掉了（grep 全 codebase 只有 ProcessCtrlGateway.cpp 那一行呼叫
        // 這個 ProcessCtrlGatewayBase::deinit()，其餘 deinit() 呼叫都是別的
        // class 自己的同名方法）。之前這裡加過 if(mThread!=nullptr) 之類的
        // 冪等防護，是在 ~ProcessCtrlGatewayBase() 也誤加一次 deinit() 呼叫
        // 的年代（efa7945/bada170 修的雙重呼叫 SIGABRT）留下的；那個誤加的
        // 呼叫點已經拿掉，防護對現在的呼叫關係是死碼，跟著拿掉才符合原版。
        PAL_ThreadClose(mThread);
        return (int)ret;
    }

    /* subscribeEvents — 對應 Ghidra GatewayBase_subscribeEvents_00018138 */
    int ProcessCtrlGatewayBase::subscribeEvents(ICBProcessEventListener *listener, void *cookie)
    {
        cacao_pal::AutoLock lock(mMutex);
        mListener = listener;
        mListenerId = (int)(uintptr_t)cookie;
        return PAL_ERR_OK;
    }

    /* start — 對應 Ghidra GatewayBase_start_1_000181b8 */
    int ProcessCtrlGatewayBase::start(const ProcessModeBase *mode)
    {
        if (!mode)
            return PAL_ERR_INVAL;

        cacao_pal::AutoLock lock(mStateMutex);

        int err;
        if (mInitFlag == 1)
        {
            struct
            {
                uint32_t msg_id;
                uint32_t r0, r1;
                PAL_QueId_t dest;
                uint32_t r2;
                uint32_t size;
                const void *payload;
            } msg;
            msg.r0 = 0;
            msg.r1 = 0;
            msg.r2 = 0;
            msg.msg_id = PAL_MSG_START;
            msg.dest = mQueId;
            msg.size = 0x1c;
            msg.payload = mode;

            err = PAL_MsgSendRecv(reinterpret_cast<PAL_Msg_t *>(&msg));
            if (err < 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "MsgSendRecv error <%s>",
                             PAL_ErrToString(err));
            }
            else
            {
                mInitFlag = 2;
            }
        }
        else
        {
            err = (mInitFlag == 2) ? PAL_ERR_ALREADY : PAL_ERR_NO_INIT;
        }
        return err;
    }

    /* 檔案層級全域物件 — 對應 _GLOBAL__sub_I_cacao_process_ctrl_base.cpp：
     *   DAT_00024004 gGatewayLock、DAT_0002408c gStopMutex、
     *   DAT_00024114 gStopCond（綁 gStopMutex）。原版只有 stop() 使用。 */
    static cacao_pal::Mutex gGatewayLock;
    static cacao_pal::Mutex gStopMutex;
    static cacao_pal::Condition gStopCond(gStopMutex);

    /* stop — 對應 Ghidra GatewayBase_stop_1_00018274 */
    int ProcessCtrlGatewayBase::stop()
    {
        cacao_pal::Mutex &rlMutex = gStopMutex;
        cacao_pal::Condition &rlCond = gStopCond;
        cacao_pal::Mutex &mMutex = gGatewayLock;

        int err;
        rlMutex.lock();
        unsigned int retryCount = 0xFFFFFFFF;
        int lockResult = mMutex.tryLock();
        if (lockResult == 0)
        {
            do
            {
                lockResult = rlCond.timedWait(1, 0);
                retryCount++;
                if (5 < retryCount)
                {
                    rlMutex.unlock();
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "gGatewayLock stop failure <%s>",
                                 PAL_ErrToString(lockResult));
                    struct
                    {
                        uint32_t msg_id;
                        uint32_t r0, r1;
                        PAL_QueId_t dest;
                        uint32_t r2;
                        uint32_t size;
                    } cln = {};
                    cln.msg_id = PAL_MSG_CLEANUP;
                    cln.dest = mQueId;
                    cln.size = 0x18;
                    PAL_MsgSend(reinterpret_cast<PAL_Msg_t *>(&cln));
                    err = lockResult;
                    goto done_stop;
                }
                lockResult = mMutex.tryLock();
            } while (lockResult == 0);
        }
        rlMutex.unlock();

        {
            cacao_pal::AutoLock lock(mStateMutex);
            unsigned int initFlag = (unsigned int)mInitFlag;
            err = (int)0xffffff9bu;

            if ((initFlag & ~1u) == 2u)
            {
                {
                    struct
                    {
                        uint32_t msg_id;
                        uint32_t r0, r1;
                        PAL_QueId_t dest;
                        uint32_t r2;
                        uint32_t size;
                    } msg = {};
                    msg.msg_id = PAL_MSG_STOP;
                    msg.dest = mQueId;
                    msg.size = 0x18;
                    unsigned int r = (unsigned int)PAL_MsgSend(reinterpret_cast<PAL_Msg_t *>(&msg));
                    if (r > 0x7fffffffu)
                    {
                        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                     "MsgSend error <%s>",
                                     PAL_ErrToString((PAL_Err_t)r));
                    }
                }

                if (initFlag == 3)
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "Cancel all requests because the camera provider is died.");
                    cancelAllRequest();
                }
                else
                {
                    mRequests.cancelAll();
                }

                bool isCancelAll = (initFlag == 3);
                struct
                {
                    uint32_t msg_id;
                    uint32_t r0, r1;
                    PAL_QueId_t dest;
                    uint32_t r2;
                    uint32_t size;
                    uint8_t cancel_all;
                    uint8_t _pad[3];
                } req = {};
                req.msg_id = PAL_MSG_STOP_REQ;
                req.dest = mQueId;
                req.size = 0x1c;
                req.cancel_all = (uint8_t)isCancelAll;

                err = PAL_MsgSendRecv(reinterpret_cast<PAL_Msg_t *>(&req));
                if (err < 0)
                {
                    PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                                 "MsgSendRecv error <%s>",
                                 PAL_ErrToString(err));
                }
                else
                {
                    mInitFlag = 1;
                }
            }
        }

        mMutex.unlock();
        rlMutex.lock();
        rlCond.signal();
        rlMutex.unlock();

    done_stop:
        return err;
    }

    /* setConfig — 對應 Ghidra GatewayBase_setConfig_1_00018532 */
    int ProcessCtrlGatewayBase::setConfig(const ProcessConfigBase *cfg)
    {
        if (!cfg)
            return (int)0xffffff99u;

        int valid = onValidateConfig(cfg);
        if (valid == 0)
            return (int)0xffffff99u;

        // clone: (*(*cfg + 0x14))(cfg)
        int **vtbl = *reinterpret_cast<int ***>(const_cast<ProcessConfigBase *>(cfg));
        void *cloned = reinterpret_cast<void *(*)(const void *)>(vtbl[0x14 / 4])(cfg);
        // 推入 ConfigList (PtrLockList at mConfigs)
        // mConfigs 內嵌 PtrLockList at offset 0x1cc from this
        cacao_pal::PtrLockList<ProcessConfigBase> *cfgList =
            reinterpret_cast<cacao_pal::PtrLockList<ProcessConfigBase> *>(
                reinterpret_cast<uint8_t *>(this) + 0x1cc);
        cfgList->pushLast(reinterpret_cast<ProcessConfigBase *>(cloned));
        return 0;
    }

    /* getConfig — 對應 Ghidra GatewayBase_getConfig_1_000185b4 */
    int ProcessCtrlGatewayBase::getConfig(ProcessConfigBase *cfg)
    {
        if (!cfg)
            return (int)0xffffff99u;

        // getConfigId: (*(*cfg + 0x10))(cfg)
        int **vtbl = *reinterpret_cast<int ***>(cfg);
        uint32_t configId = reinterpret_cast<uint32_t (*)(void *)>(vtbl[0x10 / 4])(cfg);

        // 在 ConfigList 中搜尋相同 configId（匹配 REF: get 3-param 返回 T*）
        cacao_pal::PtrLockList<ProcessConfigBase> *cfgList =
            reinterpret_cast<cacao_pal::PtrLockList<ProcessConfigBase> *>(
                reinterpret_cast<uint8_t *>(this) + 0x1cc);
        ProcessConfigBase *found = cfgList->findOne(1, ConfigList::compareConfigId, &configId);
        if (!found)
        {
            // 未在佇列中找到，呼叫 onGetConfig (vtable+0x64)
            return onGetConfig(cfg);
        }
        // 找到了，呼叫 cfg->copyFrom(found) — vtable[0x18](cfg, found)
        reinterpret_cast<void (*)(void *, void *)>(vtbl[0x18 / 4])(cfg, found);
        return 0;
    }

    /* process — 對應 Ghidra GatewayBase_process_1_00018684 */
    int ProcessCtrlGatewayBase::process(const ProcessParamBase *param,
                                        ICBProcessResultListener *listener,
                                        ProcessResultBase *result)
    {
        if (!result)
            return (int)0xffffff99u;

        struct
        {
            uint32_t msg_id;
            uint32_t r0, r1;
            PAL_QueId_t dest;
            uint32_t r2;
            uint32_t size;
            void *payload;
        } msg;
        msg.r0 = 0;
        msg.r1 = 0;
        msg.r2 = 0;

        unsigned int ret;
        {
            cacao_pal::AutoLock lock(mStateMutex);
            if (mInitFlag != 2)
            {
                ret = (unsigned int)PAL_ERR_NO_INIT;
            }
            else
            {
                msg.msg_id = PAL_MSG_PROCESS;
                msg.dest = mQueId;
                msg.size = 0x1c;

                Request *req = onCreateRequest(param, nullptr, listener, result, 1,
                                               reinterpret_cast<PAL_Msg_t *>(&msg));
                if (!req)
                {
                    ret = (unsigned int)0xffffff9au;
                }
                else
                {
                    msg.payload = req;
                    ret = (unsigned int)reinterpret_cast<cacao_pal::PtrLockList<Request> *>(
                              reinterpret_cast<uint8_t *>(this) + 0xa4)
                              ->pushLast(req);
                }
            }
        }

        if ((int)ret >= 0)
        {
            ret = (unsigned int)PAL_MsgSendRecv(reinterpret_cast<PAL_Msg_t *>(&msg));
            if ((int)ret < 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "MsgSendRecv error <%s>",
                             PAL_ErrToString((PAL_Err_t)ret));
            }
        }
        return (int)ret;
    }

    /* processAsync — 對應 Ghidra GatewayBase_processAsync_1_000187b8 */
    void ProcessCtrlGatewayBase::processAsync(const ProcessParamBase *param,
                                              android::Vector<ImageBuf *> *imageBufs,
                                              ICBProcessResultListener *listener,
                                              ProcessResultBase *result)
    {
        if (!listener || !result)
            return;

        struct
        {
            uint32_t msg_id;
            uint32_t r0, r1;
            PAL_QueId_t dest;
            uint32_t r2;
            uint32_t size;
            void *payload;
        } msg;
        msg.r0 = 0;
        msg.r1 = 0;
        msg.r2 = 0;

        Request *req = nullptr;
        {
            cacao_pal::AutoLock lock(mStateMutex);
            if (mInitFlag == 2)
            {
                msg.msg_id = PAL_MSG_PROCESS;
                msg.dest = mQueId;
                msg.size = 0x1c;

                req = onCreateRequest(param, imageBufs, listener, result, 0,
                                      reinterpret_cast<PAL_Msg_t *>(&msg));
                /* Ghidra: local_24 = req = msg+0x18 — 棧上 req 就是 msg 的 payload 欄位 */
                msg.payload = req;
                if (req)
                {
                    int pushRet = reinterpret_cast<cacao_pal::PtrLockList<Request> *>(
                                      reinterpret_cast<uint8_t *>(this) + 0xa4)
                                      ->pushLast(req);
                    if (pushRet < 0)
                        req = nullptr;
                }
            }
            else
            {
            }
        }

        if (req)
        {
            int err = PAL_MsgSend(reinterpret_cast<PAL_Msg_t *>(&msg));
            if (err < 0)
            {
                PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                             "MsgSend error <%s>",
                             PAL_ErrToString(err));
                mRequests.cancel(req);
            }
        }
    }

    /* cancel — 對應 Ghidra GatewayBase_cancel_1_000188aa */
    int ProcessCtrlGatewayBase::cancel(void *cookie)
    {
        if (!cookie)
            return (int)0xffffff99u;
        return mRequests.cancel(reinterpret_cast<Request *>(cookie));
    }

    /* serviceDied — 對應 Ghidra GatewayBase_serviceDied_1_000188c8 */
    __attribute__((stack_protect)) void ProcessCtrlGatewayBase::serviceDied()
    {
        cacao_pal::AutoLock lock(mStateMutex);
        if (mInitFlag == 2)
        {
            mInitFlag = 3;
        }
    }

    /* cancelAllRequest — 對應 Ghidra GatewayBase_cancelAllRequest_0001846c */
    void ProcessCtrlGatewayBase::cancelAllRequest()
    {
        cacao_pal::PtrLockList<Request> *reqList =
            reinterpret_cast<cacao_pal::PtrLockList<Request> *>(
                reinterpret_cast<uint8_t *>(this) + 0xa4);

        if (OBJ_U32(reqList, 0x90) != 0)
        {
            const char *logFile = __FILE__;
            const char *logMsg2 = "delete native_handle of canceled request.";
            do
            {
                Request *req = reqList->popRoot();
                if (req)
                {
                    PAL_LogPrint(logFile, __LINE__, 0x100, 2,
                                 "remaining request:%p", req);
                    notifyResult(req, (PAL_Err_t)0xffffff94u);
                    // 若 req 帶 ImageBufs，釋放它們
                    void *imagebufs = OBJ_PTR(req, 0x08);
                    if (imagebufs != nullptr)
                    {
                        uint32_t cnt = OBJ_U32(imagebufs, 0x08);
                        if (cnt != 0)
                        {
                            uint32_t i = 0;
                            do
                            {
                                PAL_LogPrint(logFile, __LINE__, 0x100, 2,
                                             logMsg2, req);
                                void *bufPtr = *reinterpret_cast<void **>(
                                    OBJ_U32(imagebufs, 0x04) + i * 4);
                                // getNativeHandle: vtable[0x10]
                                int **vtbl = *reinterpret_cast<int ***>(bufPtr);
                                void *handle = reinterpret_cast<void *(*)(void *)>(vtbl[0x10 / 4])(bufPtr);
                                native_handle_close(reinterpret_cast<const native_handle_t *>(handle));
                                native_handle_delete(const_cast<native_handle_t *>(
                                    reinterpret_cast<const native_handle_t *>(handle)));
                                // buf->decRef: vtable[0x04]
                                if (bufPtr)
                                {
                                    reinterpret_cast<void (*)(void *)>(vtbl[0x04 / 4])(bufPtr);
                                }
                                i++;
                            } while (i < cnt);
                        }
                    }
                    // req->destroy: vtable[0x04]
                    int **vtbl = *reinterpret_cast<int ***>(req);
                    reinterpret_cast<void (*)(void *)>(vtbl[0x04 / 4])(req);
                }
            } while (OBJ_U32(reqList, 0x90) != 0);
        }
    }

    /* notifyProgress — 對應 Ghidra GatewayBase_notifyProgress_00018946 */
    void ProcessCtrlGatewayBase::notifyProgress(Request *req, int progress)
    {
        if (!req)
            return;
        req->notifyProgress(progress);
    }

    /* notifyResult — 對應 Ghidra GatewayBase_notifyResult_00018952 */
    int ProcessCtrlGatewayBase::notifyResult(Request *req, PAL_Err_t err)
    {
        int ret = mRequests.finish(req);
        if (ret < 0)
            return ret;
        req->notifyResult(err);
        mRequests.remove(req);
        return PAL_ERR_OK;
    }

    /* notifyEvent — 對應 Ghidra GatewayBase_notifyEvent_0001897c */
    __attribute__((stack_protect)) void ProcessCtrlGatewayBase::notifyEvent(ProcessEventBase *ev)
    {
        if (!ev)
            return;

        ICBProcessEventListener *listener;
        {
            cacao_pal::AutoLock lock(mMutex);
            listener = reinterpret_cast<ICBProcessEventListener *>(mListener);
            if (listener)
            {
                // setCookie in lock — 對應 REF 在 AutoLock 內呼叫 ev->vtable[0x18]
                int **evVtbl = *reinterpret_cast<int ***>(ev);
                reinterpret_cast<void (*)(void *, int)>(evVtbl[0x18 / 4])(ev, mListenerId);
            }
        }
        if (listener)
        {
            // listener->onEvent(ev) — vtable[0x08]，在 lock 外
            int **lVtbl = *reinterpret_cast<int ***>(listener);
            reinterpret_cast<void (*)(void *, void *)>(lVtbl[0x08 / 4])(listener, ev);
        }
    }

    /* notifyMsg — 對應 Ghidra GatewayBase_notifyMsg_000189dc */
    int ProcessCtrlGatewayBase::notifyMsg(PAL_Msg_t *msg)
    {
        if (!msg)
            return (int)0xffffff99u;
        msg->dest = mQueId;
        unsigned int ret = (unsigned int)PAL_MsgSend(msg);
        if (ret > 0x7fffffffu)
        {
            PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                         "MsgSend error <%s>",
                         PAL_ErrToString((PAL_Err_t)ret));
        }
        return (int)ret;
    }

    /* threadDispatchMsg — 對應 Ghidra GatewayBase_threadDispatchMsg_00018070
     * REF 回傳 undefined4/int 0（符合 PAL_ThreadCallback_t = int(*)(void*,void*)）*/
    int ProcessCtrlGatewayBase::threadDispatchMsg(const PAL_Msg_t *msg, void *ctx)
    {
        if (msg && ctx)
        {
            ProcessCtrlGatewayBase *self = reinterpret_cast<ProcessCtrlGatewayBase *>(ctx);
            self->applyConfigs();
            self->onDispatchMsg(const_cast<PAL_Msg_t *>(msg));
            self->cleanCanceledRequests();
        }
        return 0;
    }

    /* onDispatchMsg — 對應 Ghidra GatewayBase_onDispatchMsg_00018b50 */
    void ProcessCtrlGatewayBase::onDispatchMsg(PAL_Msg_t *msg)
    {
        uint32_t msg_id = msg->msg_id;
        switch (msg_id)
        {
        case PAL_MSG_INIT:
        {
            // case 0: vtable+0x3c(onInit) → PAL_MsgReply → tail-fall-through onInitDelayed()
            int err = onInit();
            PAL_MsgReply(msg, err);
            onInitDelayed();
            return;
        }
        case PAL_MSG_DEINIT:
        {
            // case 1: vtable+0x44(onDeinit) → PAL_MsgReply
            int err = onDeinit();
            PAL_MsgReply(msg, err);
            return;
        }
        case PAL_MSG_START:
            // case 2: tail-call onHandleStartMsg via PLT
            onHandleStartMsg(msg);
            return;
        case PAL_MSG_STOP_REQ:
        {
            // case 3: vtable+0x50(onStop) with msg[0x18] as bool arg → PAL_MsgReply
            bool isCancelAll = *reinterpret_cast<const bool *>(
                reinterpret_cast<const uint8_t *>(msg) + 0x18);
            int err = onStop(isCancelAll);
            PAL_MsgReply(msg, err);
            return;
        }
        case PAL_MSG_STOP:
            // case 4: tail-call vtable+0x54(onStopRequest)
            onStopRequest();
            return;
        case PAL_MSG_PROCESS:
            // case 5: tail-call onHandleProcessMsg via PLT
            onHandleProcessMsg(msg);
            return;
        case PAL_MSG_CLEANUP:
            // case 6: abort (onAbortMsg via PLT → abort())
            onAbortMsg();
            return;
        default:
            // default: if (msg_id - 1) >= 2 → tail-call onHandleMsg
            if ((msg_id - 1u) >= 2u)
            {
                onHandleMsg(msg);
            }
            return;
        }
    }

    void ProcessCtrlGatewayBase::onHandleInitMsg(PAL_Msg_t *msg)
    {
        int err = onInit();
        PAL_MsgReply(msg, err);
        onInitDelayed();
    }

    void ProcessCtrlGatewayBase::onHandleDeinitMsg(PAL_Msg_t *msg)
    {
        int err = onDeinit();
        PAL_MsgReply(msg, err);
    }

    void ProcessCtrlGatewayBase::onHandleStartMsg(PAL_Msg_t *msg)
    {
        const ProcessModeBase *mode = *reinterpret_cast<const ProcessModeBase **>(
            reinterpret_cast<uint8_t *>(msg) + 0x18);
        int err = onStart(mode);
        PAL_MsgReply(msg, err);
        onStartDelayed();
    }

    void ProcessCtrlGatewayBase::onHandleStopMsg(PAL_Msg_t *msg)
    {
        bool isCancelAll = *reinterpret_cast<bool *>(
            reinterpret_cast<uint8_t *>(msg) + 0x18);
        int err = onStop(isCancelAll);
        PAL_MsgReply(msg, err);
    }

    void ProcessCtrlGatewayBase::onHandleStopRequestMsg(PAL_Msg_t *msg)
    {
        onStopRequest();
    }

    void ProcessCtrlGatewayBase::onHandleProcessMsg(PAL_Msg_t *msg)
    {
        Request *req = *reinterpret_cast<Request **>(
            reinterpret_cast<uint8_t *>(msg) + 0x18);
        int err = mRequests.start(req);
        if (err >= 0)
        {
            onProcess(req);
        }
    }

    /* onAbortMsg — 對應 Ghidra GatewayBase_onAbortMsg_00018b14 */
    void ProcessCtrlGatewayBase::onAbortMsg()
    {
        PAL_LogPrint(__FILE__, __LINE__, 0x100, 1,
                     "abort %d ms later by myself.", 300);
        usleep(300000);
        abort();
    }

    /* applyConfigs — 對應 Ghidra GatewayBase_applyConfigs_00018be0 */
    void ProcessCtrlGatewayBase::applyConfigs()
    {
        cacao_pal::PtrLockList<ProcessConfigBase> *cfgList =
            reinterpret_cast<cacao_pal::PtrLockList<ProcessConfigBase> *>(
                reinterpret_cast<uint8_t *>(this) + 0x1cc);
        ProcessConfigBase *cfg;
        goto test;
    body:
        onSetConfig(cfg);
        cfgList->removeRoot();
    test:
        cfg = cfgList->getRoot();
        if (cfg)
            goto body;
    }

    /* cleanCanceledRequests — 對應 Ghidra GatewayBase_cleanCanceledRequests_00018c98 */
    void ProcessCtrlGatewayBase::cleanCanceledRequests()
    {
        cacao_pal::PtrLockList<Request> *reqList =
            reinterpret_cast<cacao_pal::PtrLockList<Request> *>(
                reinterpret_cast<uint8_t *>(this) + 0xa4);

        uint32_t cancelState = 1; // STATE_CANCELED
        cacao_pal::Array<void *> results;

        int count = reqList->get(0, (const unsigned char *)RequestList::compareRequestState,
                                 (unsigned int)(uintptr_t)&cancelState, 0, nullptr, &results);
        if (count > 0)
        {
            for (uint32_t i = 0; i < (uint32_t)count; i++)
            {
                Request *req = reinterpret_cast<Request *>(results[i]);
                notifyResult(req, (PAL_Err_t)0xffffff94u);
            }
        }
    }

    /* ════════════════════════════════════════════════════════════════
     *  ConfigList 實作
     * ════════════════════════════════════════════════════════════════ */

    /* ConfigList 構造函數：對應 Ghidra ConfigListC1Ev_00017dd4（68 bytes）
     * 非 inline — 需要 GCC 匯出 C1Ev/C2Ev 符號，包含 vtable 設定 + Mutex ctor + PtrList 初始化 */
    ProcessCtrlGatewayBase::ConfigList::ConfigList() {}
    ProcessCtrlGatewayBase::ConfigList::~ConfigList() {}

    bool ProcessCtrlGatewayBase::ConfigList::compareConfigId(
        ProcessConfigBase *item, void *key)
    {
        // getConfigId: vtable[0x10]
        int **vtbl = *reinterpret_cast<int ***>(item);
        uint32_t id = reinterpret_cast<uint32_t (*)(void *)>(vtbl[0x10 / 4])(item);
        return id == *reinterpret_cast<uint32_t *>(key);
    }

    /* ════════════════════════════════════════════════════════════════
     *  虛擬鉤子預設實作
     * ════════════════════════════════════════════════════════════════ */

    int ProcessCtrlGatewayBase::onInit() { return PAL_ERR_OK; }
    void ProcessCtrlGatewayBase::onInitDelayed() {}
    int ProcessCtrlGatewayBase::onDeinit() { return PAL_ERR_OK; }
    int ProcessCtrlGatewayBase::onStart(const ProcessModeBase *) { return PAL_ERR_OK; }
    void ProcessCtrlGatewayBase::onStartDelayed() {}
    int ProcessCtrlGatewayBase::onStop(bool) { return PAL_ERR_OK; }
    int ProcessCtrlGatewayBase::onStopRequest() { return PAL_ERR_OK; }
    void ProcessCtrlGatewayBase::onProcess(Request *) {}
    void ProcessCtrlGatewayBase::onHandleMsg(PAL_Msg_t *) {}
    int ProcessCtrlGatewayBase::onSetConfig(const ProcessConfigBase *) { return PAL_ERR_NOT_SUPPORTED; }
    int ProcessCtrlGatewayBase::onGetConfig(ProcessConfigBase *) { return PAL_ERR_NOT_SUPPORTED; }
    int ProcessCtrlGatewayBase::onValidateConfig(const ProcessConfigBase *) { return PAL_ERR_OK; }

    ProcessCtrlGatewayBase::Request *ProcessCtrlGatewayBase::onCreateRequest(
        const ProcessParamBase *param, android::Vector<ImageBuf *> *imageBufs,
        ICBProcessResultListener *listener, ProcessResultBase *result,
        uint8_t flags, const PAL_Msg_t *replyMsg)
    {
        void *mem = ObjectBase::operator new(0xa8);
        return ::new (mem) Request(param, imageBufs, listener, result, flags, replyMsg);
    }

} // namespace cacao
