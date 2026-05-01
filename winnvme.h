#pragma once

#include <string.h>
#include <ntifs.h>
#include <wdm.h>
#include <ntddscsi.h>
#include <ntdddisk.h>
#include <ntstrsafe.h>
#include <nvme.h>

EXTERN_C_START
#include <storport.h>
#include <srbhelper.h>
EXTERN_C_END

// Storport ミニポート NVMe ドライバ全体で共有する宣言を集約したヘッダです。
// プールタグ、定数、SRB 拡張、NVMe Queue、NVMe Device、SCSI/IOCTL ハンドラの
// プロトタイプがここにまとまっています。

// デバッグ用の目印値です。メモリ破壊や意図しない delete を検出する用途で使います。
#define MARKER_TAG          0x23939889
#define BSOD_DELETE_ERROR   0x28825252

// DbgPrintEx で使う共通設定です。DEBUG_PREFIX はログの発生元識別子になります。
#define MSG_BUFFER_SIZE     128
#define DEBUG_PREFIX        "SPC ==>"
#define DBG_FILTER          0x00000888

// カーネルプールに確保した領域を追跡しやすくするためのタグです。
#define CALLINOUT_TAG       (ULONG)'TUOC'
#define DBGMSG_LEVEL        0x00001000
#define KPRINTF(x)          DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBGMSG_LEVEL, x)

#define CPP_TAG             (ULONG)'MPPC'
// 概要: カーネルプールからメモリを確保します。
void* operator new(size_t size, POOL_TYPE type, ULONG tag = CPP_TAG);
// 概要: カーネルプールからメモリを確保します。
void* operator new[](size_t size, POOL_TYPE type, ULONG tag = CPP_TAG);

// 概要: デバッグログへ状態やコマンド情報を出力します。
void DebugSrbFunctionCode(ULONG code);
// 概要: デバッグログへ状態やコマンド情報を出力します。
void DebugScsiOpCode(UCHAR opcode);
// 概要: 指定条件を満たすかどうかを判定します。
bool IsSupportedOS(ULONG major, ULONG minor = 0);

class CDebugCallInOut
{
private:
    static const int BufSize = 64;
    char* Name = NULL;
public:
    // スコープに置くだけで関数入口/出口ログを出す RAII トレーサです。
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CDebugCallInOut(char* name);
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CDebugCallInOut(const char* name);
    // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
    ~CDebugCallInOut();
};

class CSpinLock
{
public:
    // KeAcquireSpinLock/KeReleaseSpinLock を RAII で扱います。
    // 取得時 IRQL は OldIrql に保存され、解放時に復元されます。
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CSpinLock(KSPIN_LOCK* lock, bool acquire = true);
    // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
    ~CSpinLock();

    // 概要: スピンロックを取得して IRQL を保存します。
    void DoAcquire();
    // 概要: スピンロックを解放して IRQL を復元します。
    void DoRelease();
protected:
    KSPIN_LOCK* Lock = NULL;
    KIRQL OldIrql = PASSIVE_LEVEL;
    bool IsAcquired = false;
};

class CStorSpinLock
{
public:
    // Storport のロック API 用 RAII ラッパです。
    // Queue や DPC など Storport 管理下の共有データを守るときに使います。
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CStorSpinLock(PVOID devext, STOR_SPINLOCK reason, PVOID ctx = NULL);
    // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
    ~CStorSpinLock();
    // 概要: Storport のスピンロックを取得します。
    inline void Acquire(STOR_SPINLOCK reason, PVOID ctx = NULL)
    {
        StorPortAcquireSpinLock(this->DevExt, reason, ctx, &this->Handle);
    }
    // 概要: Storport のスピンロックを解放します。
    inline void Release()
    {
        StorPortReleaseSpinLock(this->DevExt, &this->Handle);
    }

protected:
    PVOID DevExt = NULL;
    STOR_LOCK_HANDLE Handle = { DpcLock, 0 };
};

namespace SPC
{
    // CAutoPtr は std::unique_ptr 相当の単純な所有権管理クラスです。
    // カーネルドライバで STL を避けつつ、途中 return/goto 時のリークを減らします。
    template <typename _Ty, POOL_TYPE _PoolType, ULONG _PoolTag>
    class SpcCppDeleter
    {
    public:
        constexpr SpcCppDeleter() noexcept = default;
        // 概要: SpcCppDeleter の処理を行います。
        SpcCppDeleter(const SpcCppDeleter<_Ty, _PoolType, _PoolTag>&) noexcept {}

        // 概要: operator の処理を行います。
        void operator()(_Ty* _Ptr) const noexcept
        {
            static_assert(0 < sizeof(_Ty), "can't delete an incomplete type");
            delete _Ptr;
        }
    };

    template<typename _Ty, POOL_TYPE _PoolType, ULONG _PoolTag, class _Dx = SpcCppDeleter<_Ty, _PoolType, _PoolTag>>
    class CAutoPtr
    {
    public:
        using DataType = _Ty;
        using DeleterType = _Dx;

        // 概要: 自動解放ポインタを初期化します。
        CAutoPtr() noexcept
        {
            this->Ptr = nullptr;
        }
        // 概要: 自動解放ポインタを初期化します。
        CAutoPtr(DataType* ptr) noexcept
        {
            this->Ptr = ptr;
        }
        // 概要: 自動解放ポインタを初期化します。
        CAutoPtr(PVOID ptr) noexcept
        {
            this->Ptr = (DataType*)ptr;
        }

        // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
        virtual ~CAutoPtr() noexcept
        {
            Reset();
        }

        CAutoPtr& operator=(CAutoPtr<_Ty, _PoolType, _PoolTag, _Dx>&& _Right) noexcept
        {
            Reset(_Right.Release());
            return *this;
        }

        // 概要: 関数 の処理を行います。
        operator _Ty* () const
        {
            return this->Ptr;
        }

        // 概要: 関数 の処理を行います。
        _Ty* operator->() const noexcept
        {
            return this->Ptr;
        }

        // 概要: デバイスまたは内部設定を更新します。
        void Set(_Ty* ptr) noexcept
        {
            this->Ptr = ptr;
        }

        _Ty* Get() const noexcept
        {
            return this->Ptr;
        }

        // 概要: 指定条件を満たすかどうかを判定します。
        bool IsNull() const noexcept
        {
            return (NULL == this->Ptr);
        }

        // 概要: 内部状態や未完了要求を初期状態へ戻します。
        void Reset(_Ty* new_ptr = nullptr) noexcept
        {
            _Ty* old_ptr = this->Ptr;
            this->Ptr = new_ptr;

            if (old_ptr)
            {
                this->Deleter(old_ptr);
            }
        }

        _Ty* Release() noexcept
        {
            _Ty* old_ptr = this->Ptr;
            this->Ptr = nullptr;
            return old_ptr;
        }

    protected:
        DataType* Ptr = nullptr;
        DeleterType Deleter;

        friend class CAutoPtr;
    };
}

using SPC::CAutoPtr;

// プールタグ定義です。Windows カーネルのメモリ追跡ではタグ単位で使用量を確認できるため、
// SRB、PRP list、ログページ、レジスタバッファなど用途別に分けています。


#define TAG_GENBUF      (ULONG)'FUBG'
#define TAG_SRB         (ULONG)' BRS'
#define TAG_SRBEXT      (ULONG)'EBRS'
#define TAG_VPDPAGE     (ULONG)'PDPV'
#define TAG_PRP2        (ULONG)'2PRP'
#define TAG_LOG_PAGE    (ULONG)'PGOL'
#define TAG_REG_BUFFER  (ULONG)'BGER'
#define TAG_ISR_POLL_CPL (ULONG)'LPCC'

// ドライバ全体で使う固定値です。NVMe/SCSI/Storport の境界で使う ID、キュー数、
// タイムアウト、レジストリ名などをここにまとめています。


#define NVME_INVALID_ID     ((ULONG)-1)
#define NVME_INVALID_CID    ((USHORT)-1)
#define NVME_INVALID_QID    ((USHORT)-1)

#define MAX_LOGIC_UNIT      1
#define MAX_IO_QUEUE_COUNT  64


#define ACCESS_RANGE_COUNT  8


#pragma region  ======== SCSI and SRB ========
#define SRB_FUNCTION_SPC_INTERNAL   0xFF
#define INVALID_PATH_ID      ((UCHAR)~0)
#define INVALID_TARGET_ID    ((UCHAR)~0)
#define INVALID_LUN_ID       ((UCHAR)~0)
#define INVALID_SRB_QUEUETAG ((ULONG)~0)
#pragma endregion

#pragma region  ======== NVME ========
#define DEFAULT_INT_COALESCE_COUNT  10
#define DEFAULT_INT_COALESCE_TIME   2
#pragma endregion

#pragma region  ======== REGISTRY ========
#define REGNAME_ADMQ_DEPTH      (UCHAR*)"AdmQDepth"
#define REGNAME_IOQ_DEPTH       (UCHAR*)"IoQDepth"
#define REGNAME_IOQ_COUNT       (UCHAR*)"IoQCount"
#define REGNAME_COALESCE_TIME   (UCHAR*)"IntCoalescingTime"
#define REGNAME_COALESCE_COUNT  (UCHAR*)"IntCoalescingEntries"


#pragma endregion

// バイトオーダ変換、ビット操作、SRB バッファコピーなどの軽量 inline 補助関数群です。
// SCSI CDB はビッグエンディアン表現を含むため、Windows/NVMe 側の値へ変換します。


// 概要: DivRoundUp の処理を行います。
FORCEINLINE size_t DivRoundUp(size_t value, size_t align_size)
{
    return (size_t)((value + (align_size-1))/align_size);
}

// 概要: RoundUp の処理を行います。
FORCEINLINE size_t RoundUp(size_t value, size_t align_size)
{
    return (DivRoundUp(value, align_size) * align_size);
}

// 概要: 指定条件を満たすかどうかを判定します。
FORCEINLINE bool IsAddrEqual(PPHYSICAL_ADDRESS a, PPHYSICAL_ADDRESS b)
{
    if (a->QuadPart == b->QuadPart)
        return true;
    return false;
}
// 概要: 指定条件を満たすかどうかを判定します。
FORCEINLINE bool IsAddrEqual(PHYSICAL_ADDRESS& a, PHYSICAL_ADDRESS& b)
{
    return IsAddrEqual(&a, &b);
}

// NVMe 仕様に基づく固定値です。Queue Entry サイズ、Feature ID、Vendor/Product 文字列など、
// NVMe コマンド作成や INQUIRY 応答で参照されます。


namespace NVME_CONST{
    static const char* VENDOR_ID = "SPC     ";
    static const char* PRODUCT_ID = "SomkingPC NVMe  ";
    static const char* PRODUCT_REV = "0100";

    static const ULONG TX_PAGES = 512;
    static const ULONG TX_SIZE = PAGE_SIZE * TX_PAGES;
    static const UCHAR SUPPORT_NAMESPACES = 4;
    static const ULONG UNSPECIFIC_NSID = 0;
    static const ULONG DEFAULT_CTRLID = 1;
    static const UCHAR MAX_TARGETS = 1;
    static const UCHAR MAX_LU = 1;
    static const ULONG MAX_IO_PER_LU = 512;
    static const UCHAR IOSQES = 6;
    static const UCHAR IOCQES = 4;
    static const UCHAR ADMIN_QUEUE_DEPTH = 64;
    
    static const USHORT CPL_INIT_PHASETAG = 1;
    static const UCHAR IO_QUEUE_COUNT = 16;
    static const USHORT IO_QUEUE_DEPTH = MAX_IO_PER_LU / 2;
    static const ULONG DEFAULT_MAX_TXSIZE = 131072;
    static const USHORT MAX_CID = MAXUSHORT-1;
    static const ULONG MAX_NS_COUNT = 1024;
    static const ULONG CPL_ALL_ENTRY = MAXULONG;
    static const ULONG INIT_DBL_VALUE = 0;
    static const ULONG INVALID_DBL_VALUE = (ULONG)MAXUSHORT;
    static const ULONG ISR_HANDLED_CPL = 8;
    static const ULONG MAX_INT_COUNT = 64;
    static const ULONG SAFE_SUBMIT_THRESHOLD = 8;
    static const ULONG STALL_TIME_US = 100;
    static const ULONG SLEEP_TIME_US = STALL_TIME_US*5;

    #pragma region ======== SetFeature default values ========
    static const UCHAR INTCOAL_TIME = 2;
    static const UCHAR INTCOAL_THRESHOLD = 8;

    static const UCHAR AB_BURST = 7;
    static const UCHAR AB_HPW = 32 - 1;
    static const UCHAR AB_MPW = 16 - 1;
    static const UCHAR AB_LPW = 8 - 1;

    #pragma endregion
};

// ドライバ内部で使う列挙型です。Queue 種別、コマンド種別、Identify CNS、
// デバイス状態など、数値だけでは意図が読み取りにくい状態を名前付きで扱います。


typedef enum class _QUEUE_TYPE
{
    UNKNOWN = 0,
    ADM_QUEUE = 1,
    IO_QUEUE = 2,
}QUEUE_TYPE;

_Enum_is_bitflag_
typedef enum class _NVME_CMD_TYPE : UINT32
{
    UNKNOWN = 0,
    ADM_CMD = 1,
    IO_CMD = 2,
    SELF_ISSUED = 0x80000000,
}NVME_CMD_TYPE;

typedef enum class _CMD_CTX_TYPE
{
    UNKNOWN = 0,
    WAIT_EVENT = 1,
    LOCAL_ADM_CMD = 2,
    SRBEXT = 3
}CMD_CTX_TYPE;

typedef enum class _IDENTIFY_CNS : UCHAR
{
    IDENT_NAMESPACE = 0,
    IDENT_CONTROLLER = 1,

}IDENTIFY_CNS;


typedef enum _NVME_STATE {
    STOP = 0,
    RUNNING = 1,
    SETUP = 2,
    INIT = 3,
    TEARDOWN = 4,
    RESETCTRL = 5,
    RESETBUS = 6,
    SHUTDOWN = 10,
}NVME_STATE;


// PCIe MSI-X テーブルのレイアウト定義です。
// メッセージアドレス、メッセージデータ、ベクタ制御を読み書きするために使います。


#pragma push(1)
#pragma warning(disable:4201)

typedef union _MSIX_MSG_ADDR
{
    struct {
        ULONG64 Aligment : 2;
        ULONG64 DestinationMode : 1;
        ULONG64 RedirHint : 1;
        ULONG64 Reserved : 8;
        ULONG64 DestinationID : 8;
        ULONG64 BaseAddr : 12;
        ULONG64 Reserved2 : 32;
    };
    // 概要: return の処理を行います。
    inline ULONG64 GetApicBaseAddr() { return (BaseAddr << 20); }
    ULONG64 AsULONG64;
}MSIX_MSG_ADDR, * PMSIX_MSG_ADDR;

typedef union _MSIX_MSG_DATA{
    struct {
        ULONG Vector : 8;
        ULONG DeliveryMode : 3;
        ULONG Reserved : 3;
        ULONG Level : 1;
        ULONG TriggerMode : 1;
        ULONG Reserved2 : 16;
    };
    ULONG AsUlong;
}MSIX_MSG_DATA, *PMSIX_MSG_DATA;

typedef union _MSIX_VECTOR_CTRL
{
    struct {
        ULONG Mask : 1;
        ULONG Reserved : 31;
    };
    ULONG AsUlong;
}MSIX_VECTOR_CTRL, *PMSIX_VECTOR_CTRL;

typedef struct _MSIX_TABLE_ENTRY
{
    MSIX_MSG_ADDR MsgAddr;
    // 概要: 内部状態やデバイス情報を取得します。
    inline ULONG64 GetApicBaseAddr() { return MsgAddr.GetApicBaseAddr(); }
    MSIX_MSG_DATA MsgData;
    MSIX_VECTOR_CTRL VectorCtrl;
}MSIX_TABLE_ENTRY, * PMSIX_TABLE_ENTRY;

typedef struct
{
    PHYSICAL_ADDRESS MsgAddress;
    ULONG MsgData;
    struct {
        ULONG Mask : 1;
        ULONG Reserved : 31;
    }DUMMYSTRUCTNAME;
}MsixVector, * PMsixVector;
#pragma pop()

// SRB ごとの作業状態を保持する拡張領域です。
// Storport の SRB、NVMe コマンド、PRP list、一時バッファ、完了コールバックを束ね、
// BuildIo から Queue 完了まで同じコンテキストとして扱います。


class CNvmeDevice;
struct _SPCNVME_SRBEXT;

typedef VOID SPC_SRBEXT_COMPLETION(struct _SPCNVME_SRBEXT *srbext);
typedef SPC_SRBEXT_COMPLETION* PSPC_SRBEXT_COMPLETION;

typedef struct _SPCNVME_SRBEXT
{
    static _SPCNVME_SRBEXT *GetSrbExt(PSTORAGE_REQUEST_BLOCK srb);
	// 概要: 内部変数やデバイス状態を初期化します。
	static _SPCNVME_SRBEXT* InitSrbExt(PVOID devext, PSTORAGE_REQUEST_BLOCK srb);

    // Storport が渡した元の SRB と、この要求を処理する NVMe デバイスです。
    CNvmeDevice *DevExt;
    PSTORAGE_REQUEST_BLOCK Srb;
    UCHAR SrbStatus;
    BOOLEAN InitOK;

    // 要求ごとに構築される NVMe コマンド、完了エントリ、PRP list 情報です。
    BOOLEAN FreePrp2List;
    NVME_COMMAND NvmeCmd;
    NVME_COMPLETION_ENTRY NvmeCpl;
    PVOID Prp2VA;
    PHYSICAL_ADDRESS Prp2PA;
    // 非同期コマンド完了時に呼ばれる任意の後処理です。
    PSPC_SRBEXT_COMPLETION CompletionCB;

    PVOID ExtBuf;      
    
    #pragma region ======== for Debugging ========
    class CNvmeQueue *SubmittedQ;
    ULONG IoQueueIndex;
    PNVME_COMMAND SubmittedCmd;
    ULONG Tag;
    ULONG SubIndex;
    #pragma endregion

    // 概要: 内部変数やデバイス状態を初期化します。
    void Init(PVOID devext, STORAGE_REQUEST_BLOCK *srb);
    // 概要: 要求で使用した一時リソースを解放します。
    void CleanUp();
    // 概要: コマンド完了後の後処理と SRB 完了通知を行います。
    void CompleteSrb(UCHAR status);
    // 概要: コマンド完了後の後処理と SRB 完了通知を行います。
    void CompleteSrb(NVME_COMMAND_STATUS& nvme_status);
    // 概要: FuncCode の処理を行います。
    ULONG FuncCode();
    // 概要: ScsiQTag の処理を行います。
    ULONG ScsiQTag();
    // 概要: Cdb の処理を行います。
    PCDB Cdb();
    // 概要: CdbLen の処理を行います。
    UCHAR CdbLen();
    // 概要: PathID の処理を行います。
    UCHAR PathID();
    // 概要: TargetID の処理を行います。
    UCHAR TargetID();
    // 概要: Lun の処理を行います。
    UCHAR Lun();
    // 概要: DataBuf の処理を行います。
    PVOID DataBuf();
    // 概要: DataBufLen の処理を行います。
    ULONG DataBufLen();
    // 概要: デバイスまたは内部設定を更新します。
    void SetTransferLength(ULONG length);
    // 概要: 内部状態や未完了要求を初期状態へ戻します。
    void ResetExtBuf(PVOID new_buffer = NULL);
    PSRBEX_DATA_PNP SrbDataPnp();
}SPCNVME_SRBEXT, * PSPCNVME_SRBEXT;

// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeToSrbStatus(NVME_COMMAND_STATUS& status);
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeGenericToSrbStatus(NVME_COMMAND_STATUS &status);
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeCmdSpecificToSrbStatus(NVME_COMMAND_STATUS &status);
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeMediaErrorToSrbStatus(NVME_COMMAND_STATUS &status);

// 概要: デバイスまたは内部設定を更新します。
void SetScsiSenseBySrbStatus(PSTORAGE_REQUEST_BLOCK srb, UCHAR &status);

// NVMe の Submission Queue と Completion Queue を表すクラス群です。
// CCmdHistory は Command ID と SRB 拡張の対応を保持し、CNvmeQueue は
// キューメモリ、Doorbell、完了処理を管理します。


typedef struct _QUEUE_PAIR_CONFIG {
    // Queue 作成時に必要なデバイス拡張、Queue ID、深さ、Doorbell、割り込み情報をまとめます。
    PVOID DevExt = NULL;
    USHORT QID = 0;
    USHORT Depth = 0;
    USHORT HistoryDepth = 0;
    ULONG NumaNode = MM_ANY_NODE_OK;
    QUEUE_TYPE Type = QUEUE_TYPE::IO_QUEUE;
    PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL SubDbl = NULL;
    PNVME_COMPLETION_QUEUE_HEAD_DOORBELL CplDbl = NULL;
    PVOID PreAllocBuffer = NULL;
    size_t PreAllocBufSize = 0; 
}QUEUE_PAIR_CONFIG, * PQUEUE_PAIR_CONFIG;

typedef struct _COMPLETED_BLOCK {
    NVME_COMPLETION_ENTRY NvmeCpl;
    PVOID SrbExt;
}COMPLETED_BLOCK, *PCOMPLETED_BLOCK;

class CCmdHistory
{
    friend class CNvmeQueue;
public:
    // Completion Entry の Command Identifier から元の要求を引き戻すための履歴表です。
    const ULONG BufferTag = (ULONG)'HBRS';

    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CCmdHistory();
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CCmdHistory(class CNvmeQueue*parent, PVOID devext, USHORT depth, ULONG numa_node = 0);
    // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
    ~CCmdHistory();

    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS Setup(class CNvmeQueue*parent, PVOID devext, USHORT depth, ULONG numa_node = 0);
    // 概要: デバイスまたはキューの終了処理を行います。
    void Teardown();
    // 概要: 内部状態や未完了要求を初期状態へ戻します。
    void Reset();
    // 概要: コマンド履歴へ SRB 拡張を登録します。
    NTSTATUS Push(ULONG index, PSPCNVME_SRBEXT srbext);
    // 概要: コマンド履歴から SRB 拡張を取り出します。
    NTSTATUS Pop(ULONG index, PSPCNVME_SRBEXT &srbext);

private:
    PVOID DevExt = NULL;
    PVOID Buffer = NULL;
    size_t BufferSize = 0;
    PSPCNVME_SRBEXT *History = NULL;
    ULONG Depth = 0;
    USHORT QueueID = NVME_INVALID_QID;
    ULONG NumaNode = MM_ANY_NODE_OK;

    class CNvmeQueue* Parent = NULL;
};

class CNvmeQueue
{
public:
    // 1 組の Submission Queue / Completion Queue を管理します。
    // Admin Queue と I/O Queue の両方で使われます。
    const static ULONG BufferTag = (ULONG)'QMVN';
    const static MEMORY_CACHING_TYPE CacheType = MEMORY_CACHING_TYPE::MmNonCached;
    // 概要: QueueCplDpcRoutine の処理を行います。
    static VOID QueueCplDpcRoutine(
        _In_ PSTOR_DPC dpc,
        _In_ PVOID devext,
        _In_opt_ PVOID sysarg1,
        _In_opt_ PVOID sysarg2
    );
    
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CNvmeQueue();
    // 概要: オブジェクトの初期化と内部状態の準備を行います。
    CNvmeQueue(QUEUE_PAIR_CONFIG* config);
    // 概要: オブジェクトの終了処理と保持リソースの解放を行います。
    ~CNvmeQueue();

    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS Setup(QUEUE_PAIR_CONFIG* config);
    // 概要: デバイスまたはキューの終了処理を行います。
    void Teardown();

    // 概要: 指定条件を満たすかどうかを判定します。
    inline bool IsInitOK(){return this->IsReady;}
    
    // 概要: 構築済みコマンドを NVMe キューへ投入します。
    NTSTATUS SubmitCmd(SPCNVME_SRBEXT* srbext, PNVME_COMMAND src_cmd);
    // 概要: コマンド完了後の後処理と SRB 完了通知を行います。
    void CompleteCmd(ULONG max_count = 0);
    // 概要: 内部状態や未完了要求を初期状態へ戻します。
    void ResetAllCmd();
    // 概要: 内部状態やデバイス情報を取得します。
    void GetQueueAddr(PVOID* subva, PHYSICAL_ADDRESS* subpa, PVOID* cplva, PHYSICAL_ADDRESS* cplpa);
    // 概要: 内部状態やデバイス情報を取得します。
    void GetQueueAddr(PVOID *subq, PVOID* cplq);
    // 概要: 内部状態やデバイス情報を取得します。
    void GetQueueAddr(PHYSICAL_ADDRESS* subq, PHYSICAL_ADDRESS* cplq);
    // 概要: 内部状態やデバイス情報を取得します。
    void GetSubQAddr(PHYSICAL_ADDRESS* subq);
    // 概要: 内部状態やデバイス情報を取得します。
    void GetCplQAddr(PHYSICAL_ADDRESS* cplq);

    STOR_DPC QueueCplDpc;
    PVOID DevExt = NULL;
    USHORT QueueID = NVME_INVALID_QID;
    USHORT Depth = 0;
    ULONG NumaNode = MM_ANY_NODE_OK;
    bool IsReady = false;
    bool UseExtBuffer = false;
    PVOID Buffer = NULL;
    PHYSICAL_ADDRESS BufferPA = {0};
    size_t BufferSize = 0;
    volatile LONG InflightCmds = 0;
    PNVME_COMMAND SubQ_VA = NULL;
    PHYSICAL_ADDRESS SubQ_PA = { 0 }; 
    size_t SubQ_Size = 0;

    PNVME_COMPLETION_ENTRY CplQ_VA = NULL;
    PHYSICAL_ADDRESS CplQ_PA = { 0 }; 
    size_t CplQ_Size = 0;

    QUEUE_TYPE Type = QUEUE_TYPE::IO_QUEUE;
    USHORT HistoryDepth = 0;
    CCmdHistory History;

    ULONG SubTail = NVME_CONST::INIT_DBL_VALUE;
    ULONG SubHead = NVME_CONST::INIT_DBL_VALUE;
    ULONG CplHead = NVME_CONST::INIT_DBL_VALUE;
    USHORT PhaseTag = NVME_CONST::CPL_INIT_PHASETAG;
    PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL SubDbl = NULL;
    PNVME_COMPLETION_QUEUE_HEAD_DOORBELL CplDbl = NULL;

    KSPIN_LOCK SubLock;

    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsSafeForSubmit();
    // 概要: レジスタまたはデバイス情報を読み取ります。
    ULONG ReadSubTail();
    // 概要: レジスタまたはデバイス状態へ値を書き込みます。
    void WriteSubTail(ULONG value);
    // 概要: レジスタまたはデバイス情報を読み取ります。
    ULONG ReadCplHead();
    // 概要: レジスタまたはデバイス状態へ値を書き込みます。
    void WriteCplHead(ULONG value);
    // 概要: 内部変数やデバイス状態を初期化します。
    bool InitQueueBuffer();
    // 概要: キュー処理に必要なメモリを確保します。
    bool AllocQueueBuffer();
    // 概要: キュー処理で確保したメモリを解放します。
    void DeallocQueueBuffer();
};

// NVMe コントローラ単位の状態と操作をまとめる中心クラスです。
// PCI リソース、NVMe レジスタ、Admin/IO Queue、Identify 情報、レジストリ設定、
// PnP/電源状態を保持し、ミニポートコールバックから呼び出されます。


typedef struct _DOORBELL_PAIR{
    NVME_SUBMISSION_QUEUE_TAIL_DOORBELL SubTail;
    NVME_COMPLETION_QUEUE_HEAD_DOORBELL CplHead;
}DOORBELL_PAIR, *PDOORBELL_PAIR;


class CNvmeDevice {
public:
    // Storport の DeviceExtension として配置されるため、C++ コンストラクタではなく
    // Setup/Teardown でライフサイクルを管理します。
    static const ULONG BUGCHECK_BASE = 0x23939889;
    static const ULONG BUGCHECK_ADAPTER = BUGCHECK_BASE + 1;
    static const ULONG BUGCHECK_INVALID_STATE = BUGCHECK_BASE + 2;
    static const ULONG BUGCHECK_NOT_IMPLEMENTED = BUGCHECK_BASE + 10;
    static const ULONG DEV_POOL_TAG = (ULONG) 'veDN';
    // 概要: NVMe の状態や完了コードを変換します。
    static BOOLEAN NvmeMsixISR(IN PVOID devext, IN ULONG msgid);
    // 概要: RestartAdapterDpc の処理を行います。
    static void RestartAdapterDpc(
            IN PSTOR_DPC  Dpc,
            IN PVOID  DevExt,
            IN PVOID  Arg1,
            IN PVOID  Arg2);
    // 概要: RestartAdapterWorker の処理を行います。
    static void RestartAdapterWorker(
        _In_ PVOID DevExt,
        _In_ PVOID Context,
        _In_ PVOID Worker);
public:
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS Setup(PPORT_CONFIGURATION_INFORMATION pci);
    // 概要: デバイスまたはキューの終了処理を行います。
    void Teardown();

    // NVMe Controller Configuration/Status レジスタを操作する基本制御です。
    // 概要: EnableController の処理を行います。
    NTSTATUS EnableController();
    // 概要: DisableController の処理を行います。
    NTSTATUS DisableController();
    // 概要: ShutdownController の処理を行います。
    NTSTATUS ShutdownController();

    // 概要: 内部変数やデバイス状態を初期化します。
    NTSTATUS InitController();
    // 概要: 内部変数やデバイス状態を初期化します。
    NTSTATUS InitNvmeStage1();
    // 概要: 内部変数やデバイス状態を初期化します。
    NTSTATUS InitNvmeStage2();
    // 概要: RestartController の処理を行います。
    NTSTATUS RestartController();

    // 概要: 作成済みキューを NVMe コントローラへ登録します。
    NTSTATUS RegisterIoQueues(PSPCNVME_SRBEXT srbext);
    // 概要: NVMe コントローラからキュー登録を解除します。
    NTSTATUS UnregisterIoQueues(PSPCNVME_SRBEXT srbext);

    // 概要: IdentifyAllNamespaces の処理を行います。
    NTSTATUS IdentifyAllNamespaces();
    // 概要: IdentifyFirstNamespace の処理を行います。
    NTSTATUS IdentifyFirstNamespace();
    // 概要: NVMe キューまたは関連リソースを作成します。
    NTSTATUS CreateIoQueues(bool force = false);

    // Identify と Feature 設定などの Admin Command 群です。
    // 概要: IdentifyController の処理を行います。
    NTSTATUS IdentifyController(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA ident, bool poll = false);
    // 概要: IdentifyNamespace の処理を行います。
    NTSTATUS IdentifyNamespace(PSPCNVME_SRBEXT srbext, ULONG nsid, PNVME_IDENTIFY_NAMESPACE_DATA data);
    // 概要: IdentifyActiveNamespaceIdList の処理を行います。
    NTSTATUS IdentifyActiveNamespaceIdList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, ULONG &ret_count);

    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetNumberOfIoQueue(USHORT count);
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetInterruptCoalescing();
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetAsyncEvent();
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetArbitration();
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetSyncHostTime();
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetPowerManagement();
    // 概要: 内部状態やデバイス情報を取得します。
    NTSTATUS GetLbaFormat(ULONG nsid, NVME_LBA_FORMAT &format);
    // 概要: 内部状態やデバイス情報を取得します。
    NTSTATUS GetNamespaceBlockSize(ULONG nsid, ULONG& size);
    // 概要: 内部状態やデバイス情報を取得します。
    NTSTATUS GetNamespaceTotalBlocks(ULONG nsid, ULONG64& blocks);
    // I/O パスで使うコマンド投入入口です。Admin Queue と I/O Queue を明確に分けます。
    // 概要: 構築済みコマンドを NVMe キューへ投入します。
    NTSTATUS SubmitAdmCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd);
    // 概要: 構築済みコマンドを NVMe キューへ投入します。
    NTSTATUS SubmitIoCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd);
    // 概要: 内部状態や未完了要求を初期状態へ戻します。
    void ResetOutstandingCmds();
    // 概要: デバイスまたは内部設定を更新します。
    NTSTATUS SetPerfOpts();
    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsInValidIoRange(ULONG nsid, ULONG64 offset, ULONG len);

    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsWorking();
    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsSetup();
    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsTeardown();
    // 概要: 指定条件を満たすかどうかを判定します。
    bool IsStop();

    bool ReadCacheEnabled;
    bool WriteCacheEnabled;
    ULONG MinPageSize;
    ULONG MaxPageSize;
    ULONG MaxTxSize;
    ULONG MaxTxPages;
    NVME_STATE State;
    ULONG RegisteredIoQ = 0;
    ULONG AllocatedIoQ = 0;
    ULONG DesiredIoQ = 0;

    ULONG DeviceTimeout;
    ULONG StallDelay;

    ACCESS_RANGE AccessRanges[ACCESS_RANGE_COUNT];
    ULONG AccessRangeCount;
    ULONG Bar0Size;
    UCHAR MaxNamespaces;
    USHORT IoDepth;
    USHORT AdmDepth;
    ULONG TotalNumaNodes;
    ULONG NamespaceCount;
    
    UCHAR CoalescingThreshold;
    UCHAR CoalescingTime;

    USHORT  VendorID;
    USHORT  DeviceID;
    ULONG   CpuCount;

    NVME_VERSION                        NvmeVer;
    NVME_CONTROLLER_CAPABILITIES        CtrlCap;
    NVME_IDENTIFY_CONTROLLER_DATA       CtrlIdent;
    NVME_IDENTIFY_NAMESPACE_DATA        NsData[NVME_CONST::SUPPORT_NAMESPACES];
    
    PVOID                               RestartWorker;
    STOR_DPC                            RestartDpc;
    GROUP_AFFINITY                      MsgGroupAffinity[NVME_CONST::MAX_INT_COUNT];
private:
    PNVME_CONTROLLER_REGISTERS          CtrlReg;
    PPORT_CONFIGURATION_INFORMATION     PortCfg;
    ULONG *Doorbells;
    PMSIX_TABLE_ENTRY MsixTable;
    CNvmeQueue* AdmQueue;
    CNvmeQueue* IoQueue[MAX_IO_QUEUE_COUNT];

    PCI_COMMON_CONFIG                   PciCfg;

    // 概要: 内部変数やデバイス状態を初期化します。
    void InitVars();
    // 概要: レジストリからドライバ設定を読み込みます。
    void LoadRegistry();

    // 概要: NVMe キューまたは関連リソースを作成します。
    NTSTATUS CreateAdmQ();
    // 概要: 作成済みキューを NVMe コントローラへ登録します。
    NTSTATUS RegisterAdmQ();
    // 概要: NVMe コントローラからキュー登録を解除します。
    NTSTATUS UnregisterAdmQ();
    // 概要: NVMe キューまたは関連リソースを削除します。
    NTSTATUS DeleteAdmQ();

    // 概要: NVMe キューまたは関連リソースを作成します。
    NTSTATUS CreateIoQ();
    // 概要: NVMe キューまたは関連リソースを削除します。
    NTSTATUS DeleteIoQ();

    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadCtrlCap();
    // 概要: PCI リソースを仮想アドレスへマップします。
    bool MapCtrlRegisters();
    // 概要: 内部状態やデバイス情報を取得します。
    bool GetPciBusData(INTERFACE_TYPE type, ULONG bus, ULONG slot);

    // 概要: コントローラ状態が期待値になるまで待機します。
    bool WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy);
    // 概要: コントローラ状態が期待値になるまで待機します。
    bool WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy, BOOLEAN cc_en);
    // 概要: コントローラ状態が期待値になるまで待機します。
    bool WaitForCtrlerShst(ULONG time_us);

    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier = true);
    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier = true);
    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadNvmeRegister(NVME_VERSION &ver, bool barrier = true);
    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadNvmeRegister(NVME_CONTROLLER_CAPABILITIES &cap, bool barrier = true);
    // 概要: レジスタまたはデバイス情報を読み取ります。
    void ReadNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES& aqa,
                        NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS& asq,
                        NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS& acq,
                        bool barrier = true);

    // 概要: レジスタまたはデバイス状態へ値を書き込みます。
    void WriteNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier = true);
    // 概要: レジスタまたはデバイス状態へ値を書き込みます。
    void WriteNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier = true);
    // 概要: レジスタまたはデバイス状態へ値を書き込みます。
    void WriteNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES &aqa, 
                        NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS &asq, 
                        NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS &acq, 
                        bool barrier = true);
    
    // 概要: 内部状態やデバイス情報を取得します。
    void GetAdmQueueDbl(PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL &sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL &cpl);
    // 概要: 内部状態やデバイス情報を取得します。
    void GetQueueDbl(ULONG qid, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL& sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL& cpl);

    // 概要: 指定条件を満たすかどうかを判定します。
    BOOLEAN IsControllerEnabled(bool barrier = true);
    // 概要: 指定条件を満たすかどうかを判定します。
    BOOLEAN IsControllerReady(bool barrier = true);
    // 概要: デバイス能力から内部キャッシュ値を更新します。
    void UpdateMaxTxSize();
};


// Storport に登録するミニポートコールバックの宣言です。
// DriverEntry の HW_INITIALIZATION_DATA に設定され、OS のストレージスタックから呼ばれます。


HW_INITIALIZE HwInitialize;
HW_PASSIVE_INITIALIZE_ROUTINE HwPassiveInitialize;
HW_STARTIO HwStartIo;
HW_BUILDIO HwBuildIo;
HW_FIND_ADAPTER HwFindAdapter;
HW_RESET_BUS HwResetBus;
HW_ADAPTER_CONTROL HwAdapterControl;
HW_PROCESS_SERVICE_REQUEST HwProcessServiceRequest;
HW_COMPLETE_SERVICE_IRP HwCompleteServiceIrp;

HW_UNIT_CONTROL HwUnitControl;
HW_TRACING_ENABLED HwTracingEnabled;
HW_CLEANUP_TRACING HwCleanupTracing;


// 概要: Storport または SCSI のイベントを処理します。
SCSI_ADAPTER_CONTROL_STATUS Handle_QuerySupportedControlTypes(
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list);
// 概要: Storport または SCSI のイベントを処理します。
SCSI_ADAPTER_CONTROL_STATUS Handle_RestartAdapter(CNvmeDevice* devext);

// NVMe PRP 作成処理の宣言です。SCSI/IOCTL のデータバッファを
// NVMe コマンドが参照できる物理ページリストへ変換します。


// 概要: データ転送用の PRP 情報を構築します。
bool BuildPrp(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd, PVOID buffer, size_t buf_size);

// NVMe コマンドビルダの宣言です。Read/Write、Identify、Queue 作成/削除、
// Feature 設定、Security Send/Receive などの Command Dword をここで統一します。


// 概要: 対応する NVMe コマンドを構築します。
void BuiildCmd_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG blocks, bool is_write);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentCtrler(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA data);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentActiveNsidList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, size_t list_size);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentSpecifiedNS(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_NAMESPACE_DATA data, ULONG nsid);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentAllNSList(PSPCNVME_SRBEXT srbext, PVOID ns_buf, size_t buf_size);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_SetIoQueueCount(PSPCNVME_SRBEXT srbext, USHORT count);

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_RegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_RegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_UnRegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_UnRegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_InterruptCoalescing(PSPCNVME_SRBEXT srbext, UCHAR threshold, UCHAR interval);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_SetArbitration(PSPCNVME_SRBEXT srbext);

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_GetFirmwareSlotsInfo(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_GetFirmwareSlotsInfoV1(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info);

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_AdminSecuritySend(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb);
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_AdminSecurityRecv(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb);


// HwBuildIo で使う軽量ディスパッチャです。PnP/電源など StartIo に回さず
// その場で処理したい要求と、StartIo 前の検証を担当します。
// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_DefaultHandler(PSPCNVME_SRBEXT srbext);
// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_IoctlHandler(PSPCNVME_SRBEXT srbext);
// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_ScsiHandler(PSPCNVME_SRBEXT srbext);
// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_SrbPowerHandler(PSPCNVME_SRBEXT srbext);
// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_SrbPnpHandler(PSPCNVME_SRBEXT srbext);


// HwStartIo で使う実処理ディスパッチャです。SCSI CDB と IOCTL を解釈し、
// 必要に応じて NVMe コマンドをキューへ投入します。


// 概要: StartIo_DefaultHandler の処理を行います。
UCHAR StartIo_DefaultHandler(PSPCNVME_SRBEXT srbext);
// 概要: StartIo_ScsiHandler の処理を行います。
UCHAR StartIo_ScsiHandler(PSPCNVME_SRBEXT srbext);
// 概要: StartIo_IoctlHandler の処理を行います。
UCHAR StartIo_IoctlHandler(PSPCNVME_SRBEXT srbext);

// SCSI コマンド処理の共通宣言です。各 CDB 長ごとのハンドラと、
// 読み書き共通の NVMe I/O 変換入口をまとめています。


// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG len, bool is_write);
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeStatus2SrbStatus(NVME_COMMAND_STATUS* status);

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_RequestSense6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Inquiry6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSelect6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSense6(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_TestUnitReady(PSPCNVME_SRBEXT srbext);

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read10(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write10(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadCapacity10(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify10(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSelect10(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSense10(PSPCNVME_SRBEXT srbext);

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReportLuns12(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read12(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write12(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify12(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_SecurityProtocolIn(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_SecurityProtocolOut(PSPCNVME_SRBEXT srbext);

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read16(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write16(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify16(PSPCNVME_SRBEXT srbext);
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadCapacity16(PSPCNVME_SRBEXT srbext);

// SCSI 応答バッファ生成と CDB 解析の inline 補助関数です。
// MODE SENSE/INQUIRY/READ WRITE の長さ・オフセット変換で使います。


// 概要: 応答データを SRB のデータバッファへコピーします。
inline ULONG CopyToCdbBuffer(PUCHAR& buffer, ULONG& buf_size, PVOID page, ULONG page_size, ULONG &ret_size)
{
    ULONG copied_size = 0;
    copied_size = page_size;
    if (copied_size > buf_size)
        copied_size = buf_size;

    RtlCopyMemory(buffer, page, copied_size);
    buf_size -= copied_size;
    buffer += copied_size;
    ret_size += copied_size;
    return copied_size;
}
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillParamHeader(PMODE_PARAMETER_HEADER header)
{
    header->ModeDataLength = sizeof(MODE_PARAMETER_HEADER) - sizeof(header->ModeDataLength);
    header->MediumType = DIRECT_ACCESS_DEVICE;
    header->DeviceSpecificParameter = 0;
    header->BlockDescriptorLength = 0;
}
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillParamHeader10(PMODE_PARAMETER_HEADER10 header)
{
    USHORT data_size = sizeof(MODE_PARAMETER_HEADER10);
    USHORT mode_data_size = data_size - sizeof(header->ModeDataLength);
    REVERSE_BYTES_2(header->ModeDataLength, &mode_data_size);
    header->MediumType = DIRECT_ACCESS_DEVICE;
    header->DeviceSpecificParameter = 0;
}
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillModePage_Caching(CNvmeDevice* devext, PMODE_CACHING_PAGE page)
{
    page->PageCode = MODE_PAGE_CACHING;
    page->PageLength = (UCHAR)(sizeof(MODE_CACHING_PAGE) - 2);
    page->ReadDisableCache = !devext->ReadCacheEnabled;
    page->WriteCacheEnable = devext->WriteCacheEnabled;
    page->PageSavable = TRUE;
}
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillModePage_InfoException(PMODE_INFO_EXCEPTIONS page)
{
    page->PageCode = MODE_PAGE_FAULT_REPORTING;
    page->PageLength = (UCHAR)(sizeof(MODE_INFO_EXCEPTIONS) - 2);
    page->ReportMethod = 5;
}
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillModePage_Control(PMODE_CONTROL_PAGE page)
{
    page->PageCode = MODE_PAGE_CONTROL;
    page->PageLength = (UCHAR)(sizeof(MODE_CONTROL_PAGE) - 2);
    page->QueueAlgorithmModifier = 0;
}
// 概要: SCSI 要求に対する応答バッファを作成します。
inline ULONG ReplyModePageCaching(CNvmeDevice* devext, PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_CACHING_PAGE page = { 0 };
    ULONG page_size = sizeof(MODE_CACHING_PAGE);
    FillModePage_Caching(devext, &page);

    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}
// 概要: SCSI 要求に対する応答バッファを作成します。
inline ULONG ReplyModePageControl(PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_CONTROL_PAGE page = { 0 };
    ULONG page_size = sizeof(MODE_CONTROL_PAGE);
    FillModePage_Control(&page);
    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}
// 概要: SCSI 要求に対する応答バッファを作成します。
inline ULONG ReplyModePageInfoExceptionCtrl(PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_INFO_EXCEPTIONS page = { 0 };
    ULONG page_size = sizeof(MODE_INFO_EXCEPTIONS);
    FillModePage_InfoException(&page);
    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}

#pragma region ======== Parse SCSI ReadWrite length and offset ======== 
// 概要: CDB から転送位置や長さを解析します。
inline void ParseReadWriteOffsetAndLen(CDB::_CDB6READWRITE &rw, ULONG64 &offset, ULONG &len)
{
    offset = (rw.LogicalBlockMsb1 << 16) | (rw.LogicalBlockMsb0 << 8) | rw.LogicalBlockLsb;
    len = rw.TransferBlocks;
    if(0 == len)
        len = 256;
}
// 概要: CDB から転送位置や長さを解析します。
inline void ParseReadWriteOffsetAndLen(CDB::_CDB10& rw, ULONG64& offset, ULONG &len)
{
    offset = 0;
    len = 0;
    REVERSE_BYTES_4(&offset, &rw.LogicalBlockByte0);
    REVERSE_BYTES_2(&len, &rw.TransferBlocksMsb);
}
// 概要: CDB から転送位置や長さを解析します。
inline void ParseReadWriteOffsetAndLen(CDB::_CDB12& rw, ULONG64& offset, ULONG &len)
{
    offset = 0;
    len = 0;
    REVERSE_BYTES_4(&offset, &rw.LogicalBlock);
    REVERSE_BYTES_4(&len, &rw.TransferLength);
}
// 概要: CDB から転送位置や長さを解析します。
inline void ParseReadWriteOffsetAndLen(CDB::_CDB16& rw, ULONG64& offset, ULONG &len)
{
    REVERSE_BYTES_8(&offset, &rw.LogicalBlock);
    REVERSE_BYTES_4(&len, &rw.TransferLength);
}
// 概要: CDB から転送位置や長さを解析します。
inline bool ParseReadWriteOffsetAndLen(CDB& cdb, ULONG64& offset, ULONG& len)
{
    switch (cdb.CDB6GENERIC.OperationCode)
    {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        ParseReadWriteOffsetAndLen(cdb.CDB6READWRITE, offset, len);
        return true;
    case SCSIOP_READ:
    case SCSIOP_WRITE:
        ParseReadWriteOffsetAndLen(cdb.CDB10, offset, len);
        return true;
    case SCSIOP_READ12:
    case SCSIOP_WRITE12:
        ParseReadWriteOffsetAndLen(cdb.CDB12, offset, len);
        return true;
    case SCSIOP_READ16:
    case SCSIOP_WRITE16:
        ParseReadWriteOffsetAndLen(cdb.CDB16, offset, len);
        return true;
    }

    return false;
}
#pragma endregion 

// IOCTL_SCSI_MINIPORT 系要求のハンドラ宣言です。


// 概要: IOCTL 要求を解釈して処理へ振り分けます。
UCHAR IoctlScsiMiniport_Firmware(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl);

// Windows 標準のファームウェア情報取得/更新要求を扱う関数宣言です。


// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_GetInfo(PSPCNVME_SRBEXT srbext);
// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_DownloadToAdapter(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request);
// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_ActivateSlot(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request);


