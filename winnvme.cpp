#include "winnvme.h"

// Windows Storport ミニポートとして動作する NVMe ドライバの実装本体です。
// このファイルは複数の実装ファイルを結合した形になっており、DriverEntry で
// Storport へコールバックを登録し、HwBuildIo/HwStartIo で SRB を受け取り、
// SCSI CDB または IOCTL を NVMe Admin/I/O コマンドへ変換します。
// CNvmeDevice はコントローラ状態を、CNvmeQueue は Submission/Completion Queue を、
// SPCNVME_SRBEXT は要求ごとの一時状態と完了コールバックを管理します。

// カーネル C++ 用の共通ユーティリティです。通常の CRT ヒープは使えないため、
// new/delete を ExAllocatePoolWithTag/ExFreePool に結び付けています。
// CDebugCallInOut とスピンロックラッパは、例外を使わないカーネルコードでも
// スコープ終了時に後始末できるようにするための RAII 補助です。
// 概要: カーネルプールからメモリを確保します。
void* __cdecl operator new(size_t size)
{
    return ExAllocatePoolWithTag(PagedPool, size, CPP_TAG);
}

// 概要: カーネルプールからメモリを確保します。
void* operator new(size_t size, POOL_TYPE type, ULONG tag)
{
    return ExAllocatePoolWithTag(type, size, tag);
}

// 概要: カーネルプールからメモリを確保します。
void* __cdecl operator new[](size_t size)
{
    return ExAllocatePoolWithTag(PagedPool, size, CPP_TAG);
}

// 概要: カーネルプールからメモリを確保します。
void* operator new[](size_t size, POOL_TYPE type, ULONG tag)
{
    return ExAllocatePoolWithTag(type, size, tag);
}

// 概要: カーネルプールへメモリを解放します。
void __cdecl operator delete(void* ptr, size_t size)
{
    UNREFERENCED_PARAMETER(size);
    ExFreePool(ptr);
}

// 概要: カーネルプールへメモリを解放します。
void __cdecl operator delete[](void* ptr)
{
    ExFreePool(ptr);
}

// 概要: カーネルプールへメモリを解放します。
void __cdecl operator delete[](void* ptr, size_t size)
{
    UNREFERENCED_PARAMETER(size);
    ExFreePool(ptr);
}

// 概要: デバッグログへ状態やコマンド情報を出力します。
static __inline void DebugCallIn(const char* func_name, const char* prefix)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s [%s] IN =>\n", prefix, func_name);
}

// 概要: デバッグログへ状態やコマンド情報を出力します。
static __inline void DebugCallOut(const char* func_name, const char* prefix)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s [%s] OUT <=\n", prefix, func_name);
}

// 概要: オブジェクトの初期化と内部状態の準備を行います。
CDebugCallInOut::CDebugCallInOut(const char* name)
    : CDebugCallInOut((char*)name)
{
}

// 概要: オブジェクトの初期化と内部状態の準備を行います。
CDebugCallInOut::CDebugCallInOut(char* name)
{
    this->Name = (char*)new(NonPagedPoolNx, CALLINOUT_TAG) char[this->BufSize];
    if (NULL != this->Name)
    {
        RtlStringCbCopyNA(this->Name, this->BufSize, name, this->BufSize - 1);
        DebugCallIn(this->Name, DEBUG_PREFIX);
    }
}

// 概要: オブジェクトの終了処理と保持リソースの解放を行います。
CDebugCallInOut::~CDebugCallInOut()
{
    if (NULL != this->Name)
    {
        DebugCallOut(this->Name, DEBUG_PREFIX);
        delete[] Name;
    }
}

// 概要: オブジェクトの初期化と内部状態の準備を行います。
CSpinLock::CSpinLock(KSPIN_LOCK* lock, bool acquire)
{
    this->Lock = lock;
    this->IsAcquired = false;
    if (acquire)
        DoAcquire();
}

// 概要: オブジェクトの終了処理と保持リソースの解放を行います。
CSpinLock::~CSpinLock()
{
    DoRelease();
    this->Lock = NULL;
    this->IsAcquired = false;
}

// 概要: スピンロックを取得して IRQL を保存します。
void CSpinLock::DoAcquire()
{
    if (!IsAcquired)
    {
        KeAcquireSpinLock(this->Lock, &this->OldIrql);
        IsAcquired = true;
    }
}

// 概要: スピンロックを解放して IRQL を復元します。
void CSpinLock::DoRelease()
{
    if (IsAcquired)
    {
        KeReleaseSpinLock(this->Lock, this->OldIrql);
        IsAcquired = false;
    }
}

// 概要: オブジェクトの初期化と内部状態の準備を行います。
CStorSpinLock::CStorSpinLock(PVOID devext, STOR_SPINLOCK reason, PVOID ctx)
{
    this->DevExt = devext;
    Acquire(reason, ctx);
}

// 概要: オブジェクトの終了処理と保持リソースの解放を行います。
CStorSpinLock::~CStorSpinLock()
{
    Release();
}

// 概要: 指定条件を満たすかどうかを判定します。
bool IsSupportedOS(ULONG major, ULONG minor)
{
    OSVERSIONINFOW info = { 0 };
    info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
    NTSTATUS status = RtlGetVersion(&info);
    if (NT_SUCCESS(status))
    {
        if (info.dwMajorVersion > major)
            return true;
        if (info.dwMajorVersion == major && info.dwMinorVersion >= minor)
            return true;
    }
    return false;
}

// 概要: デバッグログへ状態やコマンド情報を出力します。
void DebugSrbFunctionCode(ULONG code)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s Got SRB cmd, code (0x%08X)\n", DEBUG_PREFIX, code);
}

// 概要: デバッグログへ状態やコマンド情報を出力します。
void DebugScsiOpCode(UCHAR opcode)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s Got SCSI Cmd(0x%02X)\n", DEBUG_PREFIX, opcode);
}


// SRB_FUNCTION_IO_CONTROL のうち IOCTL_SCSI_MINIPORT_* 系を処理します。
// Windows のストレージスタックは SRB_IO_CONTROL を先頭にしたバッファを渡すため、
// Signature と ControlCode/Function を確認して、対応する処理へ振り分けます。
// 概要: IOCTL 要求を解釈して処理へ振り分けます。
UCHAR IoctlScsiMiniport_Firmware(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl)
{
    ULONG data_len = srbext->DataBufLen();
    PFIRMWARE_REQUEST_BLOCK request = (PFIRMWARE_REQUEST_BLOCK)(ioctl + 1);
    UCHAR srb_status = SRB_STATUS_INVALID_REQUEST;
    if (data_len < (sizeof(SRB_IO_CONTROL) + sizeof(FIRMWARE_REQUEST_BLOCK)))
    {
        ioctl->ReturnCode = FIRMWARE_STATUS_OUTPUT_BUFFER_TOO_SMALL;
        return SRB_STATUS_INVALID_PARAMETER;
    }

    switch (request->Function)
    {
    case FIRMWARE_FUNCTION_GET_INFO:
        srb_status = Firmware_GetInfo(srbext);
        break;
    case FIRMWARE_FUNCTION_DOWNLOAD:
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    case FIRMWARE_FUNCTION_ACTIVATE:
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    return srb_status;
}

// Storport から通知されるアダプタ制御イベントの補助処理です。
// サポートする制御種別の申告、再起動、サプライズリムーブ、電源遷移などは
// HwAdapterControl からここへ分岐します。
// 概要: Storport または SCSI のイベントを処理します。
SCSI_ADAPTER_CONTROL_STATUS Handle_QuerySupportedControlTypes(
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list)
{
    ULONG max_support = list->MaxControlType;

    if (ScsiStopAdapter <= max_support)
        list->SupportedTypeList[ScsiStopAdapter] = TRUE;
    if (ScsiRestartAdapter <= max_support)
        list->SupportedTypeList[ScsiRestartAdapter] = TRUE;
    if (ScsiAdapterSurpriseRemoval <= max_support)
        list->SupportedTypeList[ScsiAdapterSurpriseRemoval] = TRUE;
    if (ScsiAdapterPower <= max_support)
        list->SupportedTypeList[ScsiAdapterPower] = TRUE;
    if (ScsiPowerSettingNotification <= max_support)
        list->SupportedTypeList[ScsiPowerSettingNotification] = TRUE;
    return ScsiAdapterControlSuccess;
}

// 概要: Storport または SCSI のイベントを処理します。
SCSI_ADAPTER_CONTROL_STATUS Handle_RestartAdapter(CNvmeDevice* devext)
{
    devext->RestartController();
    return ScsiAdapterControlSuccess;
}

// Windows 標準のファームウェア情報取得要求を NVMe Admin Command へ変換します。
// 現状は GET_INFO を中心に実装しており、DOWNLOAD/ACTIVATE は安全側で無効化されています。

// 概要: NVMe の状態や完了コードを変換します。
static ULONG NvmeStatus2FirmwareStatus(NVME_COMMAND_STATUS *status)
{
    if(NVME_STATUS_TYPE_GENERIC_COMMAND == status->SCT &&
        NVME_STATUS_SUCCESS_COMPLETION == status->SC)
        return FIRMWARE_STATUS_SUCCESS;

    if(status->SCT == NVME_STATUS_TYPE_COMMAND_SPECIFIC)
    {
        switch(status->SC)
        {
        case NVME_STATUS_INVALID_LOG_PAGE:
            return FIRMWARE_STATUS_ILLEGAL_REQUEST;
        default:
            return FIRMWARE_STATUS_ERROR;
        }
    }

    return FIRMWARE_STATUS_CONTROLLER_ERROR;        
}

// 概要: 応答用構造体へ必要な情報を設定します。
static void FillFirmwareInfoV2(
    CNvmeDevice* nvme,
    PNVME_FIRMWARE_SLOT_INFO_LOG logpage,
    PSTORAGE_FIRMWARE_INFO_V2 ret_info)
{
    PNVME_IDENTIFY_CONTROLLER_DATA ctrl = &nvme->CtrlIdent;

    ret_info->ActiveSlot = logpage->AFI.ActiveSlot;
    ret_info->PendingActivateSlot = logpage->AFI.PendingActivateSlot;
    ret_info->UpgradeSupport = (ctrl->FRMW.SlotCount > 1);
    ret_info->SlotCount = ctrl->FRMW.SlotCount;
    ret_info->Version = STORAGE_FIRMWARE_DOWNLOAD_STRUCTURE_VERSION_V2;
    ret_info->Size = sizeof(STORAGE_FIRMWARE_INFO_V2);
    ret_info->FirmwareShared = TRUE;

    if (ctrl->FWUG == 0xFF || 0 == ctrl->FWUG)
        ret_info->ImagePayloadAlignment = sizeof(ULONG);
    else
        ret_info->ImagePayloadAlignment = (ULONG)(PAGE_SIZE * ctrl->FWUG);
    ret_info->ImagePayloadMaxSize = min(nvme->MaxTxSize, (128 * PAGE_SIZE));

    for (UCHAR i = 0; i < ctrl->FRMW.SlotCount; i++)
    {
        PSTORAGE_FIRMWARE_SLOT_INFO_V2 slot = &ret_info->Slot[i];
        slot->ReadOnly = FALSE;
        slot->SlotNumber = i + 1;
        RtlZeroMemory(slot->Revision, STORAGE_FIRMWARE_SLOT_INFO_V2_REVISION_LENGTH);
        RtlCopyMemory(slot->Revision, &logpage->FRS[i], sizeof(ULONGLONG));
    }

    if (ctrl->FRMW.Slot1ReadOnly)
        ret_info->Slot[0].ReadOnly = TRUE;
}

// 概要: 応答用構造体へ必要な情報を設定します。
static void FillFirmwareInfoV1(
    CNvmeDevice* nvme,
    PNVME_FIRMWARE_SLOT_INFO_LOG logpage,
    PSTORAGE_FIRMWARE_INFO ret_info)
{
    PNVME_IDENTIFY_CONTROLLER_DATA ctrl = &nvme->CtrlIdent;

    ret_info->Version = STORAGE_FIRMWARE_DOWNLOAD_STRUCTURE_VERSION;
    ret_info->Size = sizeof(STORAGE_FIRMWARE_INFO);
    ret_info->ActiveSlot = logpage->AFI.ActiveSlot;
    ret_info->PendingActivateSlot = logpage->AFI.PendingActivateSlot;
    ret_info->UpgradeSupport = (ctrl->FRMW.SlotCount > 1);
    ret_info->SlotCount = ctrl->FRMW.SlotCount;

    for (UCHAR i = 0; i < ctrl->FRMW.SlotCount; i++)
    {
        PSTORAGE_FIRMWARE_SLOT_INFO slot = &ret_info->Slot[i];
        slot->ReadOnly = FALSE;
        slot->SlotNumber = i + 1;
        slot->Revision.AsUlonglong = logpage->FRS[i];
    }
    if (ctrl->FRMW.Slot1ReadOnly)
        ret_info->Slot[0].ReadOnly = TRUE;
}

// 概要: コマンド完了後の後処理と SRB 完了通知を行います。
VOID Complete_FirmwareInfo(SPCNVME_SRBEXT *srbext)
{
    CNvmeDevice* nvme = srbext->DevExt;
    PSRB_IO_CONTROL ioctl = (PSRB_IO_CONTROL)srbext->DataBuf();
    PFIRMWARE_REQUEST_BLOCK request = (PFIRMWARE_REQUEST_BLOCK)(ioctl + 1);
    PSTORAGE_FIRMWARE_INFO buffer = (PSTORAGE_FIRMWARE_INFO)((PUCHAR)ioctl + request->DataBufferOffset);
    PNVME_FIRMWARE_SLOT_INFO_LOG logpage = (PNVME_FIRMWARE_SLOT_INFO_LOG)srbext->ExtBuf;
    PNVME_IDENTIFY_CONTROLLER_DATA ctrl = &nvme->CtrlIdent;
    ULONG buf_len = request->DataBufferLength;
    UCHAR total_slots = ctrl->FRMW.SlotCount;
    UCHAR srb_status = SRB_STATUS_INTERNAL_ERROR;
    ULONG fw_status = FIRMWARE_STATUS_ERROR;

    if (0 == request->DataBufferOffset)
    {
        fw_status = FIRMWARE_STATUS_OUTPUT_BUFFER_TOO_SMALL;
        srb_status = SRB_STATUS_INVALID_PARAMETER;
        goto END;
    }

    if (buf_len < (sizeof(STORAGE_FIRMWARE_INFO) + (sizeof(STORAGE_FIRMWARE_SLOT_INFO) * total_slots)))
    {
        fw_status = FIRMWARE_STATUS_OUTPUT_BUFFER_TOO_SMALL;
        srb_status = SRB_STATUS_INVALID_PARAMETER;
        goto END;
    }
    fw_status = NvmeStatus2FirmwareStatus(&srbext->NvmeCpl.DW3.Status);
    if(FIRMWARE_STATUS_SUCCESS != fw_status)
        goto END;

    if (buf_len >= (sizeof(STORAGE_FIRMWARE_INFO_V2) +
        sizeof(STORAGE_FIRMWARE_SLOT_INFO_V2) * ctrl->FRMW.SlotCount))
    {
        FillFirmwareInfoV2(nvme, logpage, (PSTORAGE_FIRMWARE_INFO_V2)buffer);
    }
    else
    {
        FillFirmwareInfoV1(nvme, logpage, buffer);
    }
    fw_status = FIRMWARE_STATUS_SUCCESS;
    srb_status = SRB_STATUS_SUCCESS;

END:
    ioctl->ReturnCode = fw_status;
    srbext->CleanUp();
    srbext->CompleteSrb(srb_status);
}

// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_GetInfo(PSPCNVME_SRBEXT srbext)
{
    PNVME_FIRMWARE_SLOT_INFO_LOG logpage = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    logpage = new NVME_FIRMWARE_SLOT_INFO_LOG();
    srbext->ResetExtBuf(logpage);
    srbext->CompletionCB = Complete_FirmwareInfo;

    if(srbext->DevExt->NvmeVer.MNR > 0)
        BuildCmd_GetFirmwareSlotsInfo(srbext, logpage);
    else
        BuildCmd_GetFirmwareSlotsInfoV1(srbext, logpage);

    status = srbext->DevExt->SubmitAdmCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
    {
        delete logpage;
        return SRB_STATUS_INTERNAL_ERROR;
    }
    
    return SRB_STATUS_PENDING;
}
// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_DownloadToAdapter(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request)
{
    UNREFERENCED_PARAMETER(srbext);
    UNREFERENCED_PARAMETER(ioctl);
    UNREFERENCED_PARAMETER(request);
    return SRB_STATUS_INVALID_REQUEST;
}
// 概要: ファームウェア関連要求を処理します。
UCHAR Firmware_ActivateSlot(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request)
{
    UNREFERENCED_PARAMETER(srbext);
    UNREFERENCED_PARAMETER(ioctl);
    UNREFERENCED_PARAMETER(request);
    return SRB_STATUS_INVALID_REQUEST;
}

// NVMe の PRP(Physical Region Page) を組み立てる処理です。
// データバッファの物理ページを PRP1/PRP2 または PRP list に変換し、
// コントローラが DMA で参照できる形にします。
// 概要: 内部状態やデバイス情報を取得します。
static inline size_t GetDistanceToNextPage(PUCHAR ptr)
{
    return (((PUCHAR)PAGE_ALIGN(ptr) + PAGE_SIZE) - ptr);
}

// 概要: データ転送用の PRP 情報を構築します。
static inline void BuildPrp1(ULONG64 &prp1, PVOID ptr)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(ptr);
    prp1 = pa.QuadPart;
}
// 概要: データ転送用の PRP 情報を構築します。
static inline void BuildPrp2(ULONG64& prp2, PVOID ptr)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(ptr);
    prp2 = pa.QuadPart;
}

// 概要: データ転送用の PRP 情報を構築します。
static void BuildPrp2List(PVOID prp2, PVOID ptr, size_t size)
{
    PHYSICAL_ADDRESS pa = {0};
    PULONG64 prp2list = (PULONG64)prp2;
    PUCHAR cursor = (PUCHAR)ptr;
    size_t size_left = size;
    ULONG prp2_index = 0;

    while(size_left > 0)
    {
        pa = MmGetPhysicalAddress(cursor);
        prp2list[prp2_index] = pa.QuadPart;
        if(size_left <= PAGE_SIZE)
        {
            size_left = 0;
        }
        else
        {
            size_left -= PAGE_SIZE;
            cursor += PAGE_SIZE;
        }
        prp2_index++;
    }
}

// 概要: データ転送用の PRP 情報を構築します。
bool BuildPrp(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd, PVOID buffer, size_t buf_size)
{
    PUCHAR cursor = (PUCHAR) buffer;
    size_t size_left = buf_size;
    size_t distance = 0;

    BuildPrp1(cmd->PRP1, cursor);
    distance = GetDistanceToNextPage(cursor);

    if(distance > size_left)
        return true;

    size_left -= distance;
    cursor += distance;
    if(size_left <= PAGE_SIZE)
    {
        BuildPrp2(cmd->PRP2, cursor);
        return true;
    }
    else
    {
        srbext->Prp2VA = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, TAG_PRP2);
        if(NULL == srbext->Prp2VA)
            return false;

        RtlZeroMemory(srbext->Prp2VA, PAGE_SIZE);
        srbext->FreePrp2List = TRUE;
        srbext->Prp2PA = MmGetPhysicalAddress(srbext->Prp2VA);
        BuildPrp2List(srbext->Prp2VA, cursor, size_left);
        cmd->PRP2 = srbext->Prp2PA.QuadPart;
    }
    return true;
}

// SCSI の READ/WRITE 系 CDB を NVMe Read/Write コマンドへ橋渡しします。
// SCSI の転送長とオフセットはブロック単位なので、Namespace ID と範囲を確認してから
// I/O Queue へ投入します。

// 概要: コマンド完了後の後処理と SRB 完了通知を行います。
void Complete_ScsiReadWrite(SPCNVME_SRBEXT *srbext)
{
    srbext->CleanUp();
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG len, bool is_write)
{
    UCHAR srb_status = SRB_STATUS_PENDING;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG nsid = srbext->Lun() + 1;

    if (!srbext->DevExt->IsInValidIoRange(nsid, offset, len))
        return SRB_STATUS_ERROR;

    BuiildCmd_ReadWrite(srbext, offset, len, is_write);
    status = srbext->DevExt->SubmitIoCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
    {
        if(STATUS_DEVICE_BUSY == status)
            srb_status = SRB_STATUS_BUSY;
        else
            srb_status = SRB_STATUS_ERROR;
    
    }
    else
        srb_status = SRB_STATUS_PENDING;

    return srb_status;
}

// STORAGE_REQUEST_BLOCK(SRB) に紐付くドライバ専用拡張領域です。
// 元 SRB、NVMe コマンド、一時バッファ、PRP list、完了コールバックをまとめて保持し、
// BuildIo から Queue 完了まで同じ文脈を受け渡します。
// 概要: 内部変数やデバイス状態を初期化します。
_SPCNVME_SRBEXT* _SPCNVME_SRBEXT::InitSrbExt(PVOID devext, PSTORAGE_REQUEST_BLOCK srb)
{
	PSPCNVME_SRBEXT srbext = (PSPCNVME_SRBEXT)SrbGetMiniportContext(srb);
	srbext->Init(devext, srb);
	return srbext;
}
// 概要: 内部状態やデバイス情報を取得します。
_SPCNVME_SRBEXT* _SPCNVME_SRBEXT::GetSrbExt(PSTORAGE_REQUEST_BLOCK srb)
{
    PSPCNVME_SRBEXT ret = (PSPCNVME_SRBEXT)SrbGetMiniportContext(srb);
    ASSERT(ret->Srb != NULL);
    return ret;
}

// 概要: 内部変数やデバイス状態を初期化します。
void _SPCNVME_SRBEXT::Init(PVOID devext, STORAGE_REQUEST_BLOCK* srb)
{
    RtlZeroMemory(this, sizeof(_SPCNVME_SRBEXT));

    DevExt = (CNvmeDevice*)devext;
    Srb = srb;
    SrbStatus = SRB_STATUS_PENDING;
    InitOK = TRUE;
    Tag = ScsiQTag();
}

// 概要: 要求で使用した一時リソースを解放します。
void _SPCNVME_SRBEXT::CleanUp()
{
    ResetExtBuf(NULL);
    if(NULL != Prp2VA)
    { 
        ExFreePoolWithTag(Prp2VA, TAG_PRP2);
        Prp2VA = NULL;
    }
}
// 概要: コマンド完了後の後処理と SRB 完了通知を行います。
void _SPCNVME_SRBEXT::CompleteSrb(NVME_COMMAND_STATUS &nvme_status)
{
    UCHAR status = NvmeToSrbStatus(nvme_status);
    CompleteSrb(status);
}
// 概要: コマンド完了後の後処理と SRB 完了通知を行います。
void _SPCNVME_SRBEXT::CompleteSrb(UCHAR status)
{
    if (NULL != Srb)
    {
        SetScsiSenseBySrbStatus(Srb, status);
        SrbSetSrbStatus(Srb, status);
        StorPortNotification(RequestComplete, DevExt, Srb);
    }
}
// 概要: FuncCode の処理を行います。
ULONG _SPCNVME_SRBEXT::FuncCode()
{
    if(NULL == Srb)
        return SRB_FUNCTION_SPC_INTERNAL;
    return SrbGetSrbFunction(Srb);
}
// 概要: ScsiQTag の処理を行います。
ULONG _SPCNVME_SRBEXT::ScsiQTag()
{
    if (NULL == Srb)
        return 0;
    return SrbGetQueueTag(Srb);
}
// 概要: Cdb の処理を行います。
PCDB _SPCNVME_SRBEXT::Cdb()
{
    if (NULL == Srb)
        return NULL;
    return SrbGetCdb(Srb);
}
// 概要: CdbLen の処理を行います。
UCHAR _SPCNVME_SRBEXT::CdbLen() {
    if (NULL == Srb)
        return 0;
    return SrbGetCdbLength(Srb);
}
// 概要: PathID の処理を行います。
UCHAR _SPCNVME_SRBEXT::PathID() {
    if (NULL == Srb)
        return INVALID_PATH_ID;
    return SrbGetPathId(Srb);
}
// 概要: TargetID の処理を行います。
UCHAR _SPCNVME_SRBEXT::TargetID() {
    if (NULL == Srb)
        return INVALID_TARGET_ID;
    return SrbGetTargetId(Srb);
}
// 概要: Lun の処理を行います。
UCHAR _SPCNVME_SRBEXT::Lun() {
    if (NULL == Srb)
        return INVALID_LUN_ID;
    return SrbGetLun(Srb);
}
// 概要: DataBuf の処理を行います。
PVOID _SPCNVME_SRBEXT::DataBuf() {
    if (NULL == Srb)
        return NULL;
    return SrbGetDataBuffer(Srb);
}
// 概要: DataBufLen の処理を行います。
ULONG _SPCNVME_SRBEXT::DataBufLen() {
    if (NULL == Srb)
        return 0;
    return SrbGetDataTransferLength(Srb);
}

// 概要: デバイスまたは内部設定を更新します。
void _SPCNVME_SRBEXT::SetTransferLength(ULONG length)
{
    if(NULL != Srb)
        SrbSetDataTransferLength(Srb, length);
}

// 概要: 内部状態や未完了要求を初期状態へ戻します。
void _SPCNVME_SRBEXT::ResetExtBuf(PVOID new_buffer)
{
    if(NULL != ExtBuf)
        delete[] ExtBuf;
    ExtBuf = new_buffer;
}

PSRBEX_DATA_PNP _SPCNVME_SRBEXT::SrbDataPnp()
{
    if (NULL != Srb)
        return (PSRBEX_DATA_PNP)SrbGetSrbExDataByType(Srb, SrbExDataTypePnP);
    return NULL;
}

// NVMe Admin/I/O コマンドの Command Dword を設定するビルダ群です。
// SCSI ハンドラや初期化処理は、ここで作った NVME_COMMAND を Queue に投入します。
// 概要: 対応する NVMe コマンドを構築します。
void BuiildCmd_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG blocks, bool is_write)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    ULONG nsid = srbext->Lun() + 1;

    cmd->CDW0.OPC = (is_write)? NVME_NVM_COMMAND_WRITE : NVME_NVM_COMMAND_READ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;
    BuildPrp(srbext, cmd, srbext->DataBuf(), srbext->DataBufLen());
    cmd->u.READWRITE.LBALOW = (ULONG)(offset & 0xFFFFFFFFULL);
    cmd->u.READWRITE.LBAHIGH = (ULONG)(offset >> 32);
    cmd->u.READWRITE.CDW12.NLB = blocks - 1;
}

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentCtrler(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA data)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_CONTROLLER;
    cmd->u.IDENTIFY.CDW10.CNTID = 0;
    BuildPrp(srbext, cmd, (PVOID) data, sizeof(NVME_IDENTIFY_CONTROLLER_DATA));
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentActiveNsidList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, size_t list_size)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_ACTIVE_NAMESPACES;
    BuildPrp(srbext, cmd, nsid_list, list_size);
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentSpecifiedNS(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_NAMESPACE_DATA data, ULONG nsid)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_SPECIFIC_NAMESPACE;

    BuildPrp(srbext, cmd, (PVOID)data, sizeof(NVME_IDENTIFY_NAMESPACE_DATA));
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_IdentAllNSList(PSPCNVME_SRBEXT srbext, PVOID ns_buf, size_t buf_size)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_ALLOCATED_NAMESPACE_LIST;

    BuildPrp(srbext, cmd, (PVOID)ns_buf, buf_size);
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_SetIoQueueCount(PSPCNVME_SRBEXT srbext, USHORT count)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_NUMBER_OF_QUEUES;

    cmd->u.SETFEATURES.CDW11.NumberOfQueues.NSQ =
        cmd->u.SETFEATURES.CDW11.NumberOfQueues.NCQ = count - 1;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_RegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue *queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    PHYSICAL_ADDRESS paddr = {0};
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    queue->GetSubQAddr(&paddr);

    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_CREATE_IO_SQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->PRP1 = (ULONG64)paddr.QuadPart;
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOSQ.CDW10.QID = queue->QueueID;
    cmd->u.CREATEIOSQ.CDW10.QSIZE = queue->Depth - 1;
    cmd->u.CREATEIOSQ.CDW11.CQID = queue->QueueID;;
    cmd->u.CREATEIOSQ.CDW11.PC = TRUE;
    cmd->u.CREATEIOSQ.CDW11.QPRIO = NVME_NVM_QUEUE_PRIORITY_HIGH;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_RegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    PHYSICAL_ADDRESS paddr = { 0 };
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    queue->GetCplQAddr(&paddr);

    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_CREATE_IO_CQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->PRP1 = (ULONG64)paddr.QuadPart;
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOCQ.CDW10.QID = queue->QueueID;
    cmd->u.CREATEIOCQ.CDW10.QSIZE = queue->Depth - 1;
    cmd->u.CREATEIOCQ.CDW11.IEN = TRUE;
    cmd->u.CREATEIOCQ.CDW11.IV = (queue->Type == QUEUE_TYPE::ADM_QUEUE) ? 0 : queue->QueueID;
    cmd->u.CREATEIOCQ.CDW11.PC = TRUE;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_UnRegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_DELETE_IO_SQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOSQ.CDW10.QID = queue->QueueID;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_UnRegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_DELETE_IO_CQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOSQ.CDW10.QID = queue->QueueID;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_InterruptCoalescing(PSPCNVME_SRBEXT srbext, UCHAR threshold, UCHAR interval)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_INTERRUPT_COALESCING;
    cmd->u.SETFEATURES.CDW11.InterruptCoalescing.THR = threshold;
    cmd->u.SETFEATURES.CDW11.InterruptCoalescing.TIME = interval;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_SetArbitration(PSPCNVME_SRBEXT srbext)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_ARBITRATION;
    cmd->u.SETFEATURES.CDW11.Arbitration.AB = NVME_CONST::AB_BURST;
    cmd->u.SETFEATURES.CDW11.Arbitration.HPW = NVME_CONST::AB_HPW;
    cmd->u.SETFEATURES.CDW11.Arbitration.MPW = NVME_CONST::AB_MPW;
    cmd->u.SETFEATURES.CDW11.Arbitration.LPW = NVME_CONST::AB_LPW;
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_SyncHostTime(PSPCNVME_SRBEXT srbext, LARGE_INTEGER *timestamp)
{
    UNREFERENCED_PARAMETER(timestamp);
    LARGE_INTEGER systime = { 0 };
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    LARGE_INTEGER elapsed = { 0 };

    KeQuerySystemTime(&systime);
    RtlTimeToSecondsSince1970(&systime, &elapsed.LowPart);
    elapsed.QuadPart = elapsed.LowPart * 1000;

    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_TIMESTAMP;



}

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_GetFirmwareSlotsInfo(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_GET_LOG_PAGE;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;

    ULONG dword_count = (ULONG)(sizeof(NVME_FIRMWARE_SLOT_INFO_LOG) >> 2);
    cmd->u.GETLOGPAGE.CDW10_V13.NUMDL = (USHORT) (dword_count & 0x0000FFFF);
    cmd->u.GETLOGPAGE.CDW10_V13.LID = NVME_LOG_PAGE_FIRMWARE_SLOT_INFO;
    cmd->u.GETLOGPAGE.CDW11.NUMDU = (USHORT) (dword_count >> 16);

    BuildPrp(srbext, cmd, info, sizeof(NVME_FIRMWARE_SLOT_INFO_LOG));
}

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_GetFirmwareSlotsInfoV1(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_GET_LOG_PAGE;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;

    USHORT dword_count = (USHORT)(sizeof(NVME_FIRMWARE_SLOT_INFO_LOG) >> 2);
    cmd->u.GETLOGPAGE.CDW10.NUMD = dword_count & (USHORT)0x0FFF;
    cmd->u.GETLOGPAGE.CDW10.LID = NVME_LOG_PAGE_FIRMWARE_SLOT_INFO;

    BuildPrp(srbext, cmd, info, sizeof(NVME_FIRMWARE_SLOT_INFO_LOG));
}

// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_AdminSecuritySend(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SECURITY_SEND;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;

    cmd->u.SECURITYSEND.CDW10.SECP = cdb->SECURITY_PROTOCOL_OUT.SecurityProtocol;
    USHORT spsp = 0;
    REVERSE_BYTES_2(&spsp, cdb->SECURITY_PROTOCOL_OUT.SecurityProtocolSpecific);
    cmd->u.SECURITYSEND.CDW10.SPSP = spsp;
    ULONG payload_size = 0;
    REVERSE_BYTES_4(&payload_size, cdb->SECURITY_PROTOCOL_OUT.AllocationLength);
    cmd->u.SECURITYSEND.CDW11.TL = payload_size;

    BuildPrp(srbext, cmd, srbext->DataBuf(), srbext->DataBufLen());
}
// 概要: 対応する NVMe コマンドを構築します。
void BuildCmd_AdminSecurityRecv(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SECURITY_RECEIVE;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;

    cmd->u.SECURITYSEND.CDW10.SECP = cdb->SECURITY_PROTOCOL_IN.SecurityProtocol;
    USHORT spsp = 0;
    REVERSE_BYTES_2(&spsp, cdb->SECURITY_PROTOCOL_IN.SecurityProtocolSpecific);
    cmd->u.SECURITYSEND.CDW10.SPSP = spsp;
    ULONG payload_size = 0;
    REVERSE_BYTES_4(&payload_size, cdb->SECURITY_PROTOCOL_IN.AllocationLength);
    cmd->u.SECURITYSEND.CDW11.TL = payload_size;

    BuildPrp(srbext, cmd, srbext->DataBuf(), srbext->DataBufLen());
}

// 10 バイト CDB の SCSI コマンドを処理します。
// READ/WRITE/READ CAPACITY/MODE SENSE など、OS が通常のディスクとして認識するための
// 基本応答を NVMe の名前空間情報や I/O コマンドから生成します。
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read10(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB10, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write10(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB10, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadCapacity10(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    ULONG ret_size = 0;
    PREAD_CAPACITY_DATA cap = (PREAD_CAPACITY_DATA)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;
    UCHAR lun = srbext->Lun();

    if(lun >= srbext->DevExt->NamespaceCount)
    { 
        srb_status = SRB_STATUS_INVALID_LUN;
        goto END;
    }
    if(!srbext->DevExt->IsWorking())
    {
        srb_status = SRB_STATUS_NO_DEVICE;
        goto END;
    }
    
    if (srbext->DataBufLen() < sizeof(READ_CAPACITY_DATA))
    {
        srb_status = SRB_STATUS_DATA_OVERRUN;
        ret_size = sizeof(READ_CAPACITY_DATA);
        goto END;
    }
    
    srbext->DevExt->GetNamespaceTotalBlocks(lun + 1, blocks);
    srbext->DevExt->GetNamespaceBlockSize(lun + 1, block_size);
    if (blocks > MAXULONG32)
    {
        srb_status = SRB_STATUS_INVALID_REQUEST;
        ret_size = 0;
        goto END;
    }
    blocks -= 1;

    cap->LogicalBlockAddress = cap->BytesPerBlock = 0;
    REVERSE_BYTES_4(&cap->BytesPerBlock, &block_size);
    REVERSE_BYTES_4(&cap->LogicalBlockAddress, &blocks);

    ret_size = sizeof(READ_CAPACITY_DATA);
    srb_status = SRB_STATUS_SUCCESS;

END:
    srbext->SetTransferLength(ret_size);
    return srb_status;
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;



}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSelect10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSense10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

}


// 12 バイト CDB の SCSI コマンドを処理します。
// REPORT LUNS、READ/WRITE12、SECURITY PROTOCOL IN/OUT などを扱います。
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReportLuns12(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read12(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB12, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write12(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB12, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify12(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;




}


// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_SecurityProtocolIn(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN is_support = srbext->DevExt->CtrlIdent.OACS.SecurityCommands;
    if(!is_support)
        return SRB_STATUS_ERROR;

    BuildCmd_AdminSecurityRecv(srbext, NVME_CONST::DEFAULT_CTRLID, srbext->Cdb());
    status = srbext->DevExt->SubmitAdmCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        srb_status = SRB_STATUS_ERROR;
    else
        srb_status = SRB_STATUS_PENDING;

    return srb_status;
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_SecurityProtocolOut(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN is_support = srbext->DevExt->CtrlIdent.OACS.SecurityCommands;
    if (!is_support)
        return SRB_STATUS_ERROR;

    BuildCmd_AdminSecuritySend(srbext, NVME_CONST::DEFAULT_CTRLID, srbext->Cdb());
    status = srbext->DevExt->SubmitAdmCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        srb_status = SRB_STATUS_ERROR;
    else
        srb_status = SRB_STATUS_PENDING;

    return srb_status;
}

// 16 バイト CDB の SCSI コマンドを処理します。
// 大容量ディスクで必要になる READ/WRITE16 と READ CAPACITY16 が中心です。
// 概要: 応答用構造体へ必要な情報を設定します。
inline void FillReadCapacityEx(UCHAR lun, PSPCNVME_SRBEXT srbext)
{
    PREAD_CAPACITY_DATA_EX cap = (PREAD_CAPACITY_DATA_EX)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;
    srbext->DevExt->GetNamespaceBlockSize(lun+1, block_size);

    srbext->DevExt->GetNamespaceTotalBlocks(lun+1, blocks);
    blocks -= 1;
    REVERSE_BYTES_4(&cap->BytesPerBlock, &block_size);
    REVERSE_BYTES_8(&cap->LogicalBlockAddress.QuadPart, &blocks);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read16(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB16, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write16(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB16, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify16(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;



}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ReadCapacity16(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    ULONG ret_size = 0;
    UCHAR lun = srbext->Lun();
    PREAD_CAPACITY_DATA_EX cap = (PREAD_CAPACITY_DATA_EX)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;

    if (lun >= srbext->DevExt->NamespaceCount)
    {
        srb_status = SRB_STATUS_INVALID_LUN;
        goto END;
    }
    if (!srbext->DevExt->IsWorking())
    {
        srb_status = SRB_STATUS_NO_DEVICE;
        goto END;
    }

    if (srbext->DataBufLen() < sizeof(READ_CAPACITY_DATA_EX))
    {
        srb_status = SRB_STATUS_DATA_OVERRUN;
        ret_size = sizeof(READ_CAPACITY_DATA_EX);
        goto END;
    }

    srbext->DevExt->GetNamespaceBlockSize(lun + 1, block_size);
    srbext->DevExt->GetNamespaceTotalBlocks(lun + 1, blocks);
    blocks -= 1;
    REVERSE_BYTES_4(&cap->BytesPerBlock, &block_size);
    REVERSE_BYTES_8(&cap->LogicalBlockAddress.QuadPart, &blocks);
    ret_size = sizeof(READ_CAPACITY_DATA_EX);
    srb_status = SRB_STATUS_SUCCESS;

END:
    srbext->SetTransferLength(ret_size);
    return srb_status;
}

// 6 バイト CDB の SCSI コマンドを処理します。
// INQUIRY、REQUEST SENSE、MODE SENSE など、デバイス認識時に頻繁に呼ばれる
// レガシー互換の応答をここで生成します。
typedef struct _CDB6_REQUESTSENSE
{
    UCHAR OperationCode;
    UCHAR DescFormat : 1;
    UCHAR Reserved1 : 7;
    UCHAR Reserved2[2];
    UCHAR AllocSize;
    struct {
        UCHAR Link : 1;
        UCHAR Flag : 1;
        UCHAR NormalACA : 1;
        UCHAR Reserved : 3;
        UCHAR VenderSpecific : 2;
    }Control;
}CDB6_REQUESTSENSE, *PCDB6_REQUESTSENSE;

// 概要: SCSI 要求に対する応答バッファを作成します。
static UCHAR Reply_VpdSupportPages(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    ULONG buf_size = srbext->DataBufLen();
    ret_size = 0;

    UCHAR valid_pages = 5;
    buf_size = (FIELD_OFFSET(VPD_SUPPORTED_PAGES_PAGE, SupportedPageList) +
        valid_pages * sizeof(UCHAR));

    SPC::CAutoPtr<UCHAR, PagedPool, TAG_VPDPAGE>
            page_ptr(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);
    if(page_ptr.IsNull())
        return SRB_STATUS_INSUFFICIENT_RESOURCES;
    PVPD_SUPPORTED_PAGES_PAGE page = (PVPD_SUPPORTED_PAGES_PAGE)page_ptr.Get();
    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_SUPPORTED_PAGES;
    page->PageLength = valid_pages;
    page->SupportedPageList[0] = VPD_SUPPORTED_PAGES;
    page->SupportedPageList[1] = VPD_SERIAL_NUMBER;
    page->SupportedPageList[2] = VPD_DEVICE_IDENTIFIERS;
    page->SupportedPageList[3] = VPD_BLOCK_LIMITS;
    page->SupportedPageList[4] = VPD_BLOCK_DEVICE_CHARACTERISTICS;

    ret_size = min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);
    return SRB_STATUS_SUCCESS;
}
// 概要: SCSI 要求に対する応答バッファを作成します。
static UCHAR Reply_VpdSerialNumber(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    PUCHAR buffer = (PUCHAR)srbext->DataBuf();
    ULONG buf_size = srbext->DataBufLen();
    size_t sn_len = strlen((char*)srbext->DevExt->CtrlIdent.SN);
    sn_len = (sn_len, 255);
    ULONG size = (ULONG)(sn_len + sizeof(VPD_SERIAL_NUMBER_PAGE) + 1);
    ret_size = size;

    if(size < srbext->DataBufLen())
        return SRB_STATUS_INSUFFICIENT_RESOURCES;

    PVPD_SERIAL_NUMBER_PAGE page = (PVPD_SERIAL_NUMBER_PAGE)srbext->DataBuf();
    RtlZeroMemory(buffer, buf_size);
    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_SERIAL_NUMBER;
    page->PageLength = (UCHAR) sn_len;
    page++;
    memcpy(page, srbext->DevExt->CtrlIdent.SN, sn_len);

    return SRB_STATUS_SUCCESS;
}
// 概要: SCSI 要求に対する応答バッファを作成します。
static UCHAR Reply_VpdIdentifier(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    char *subnqn = (char*)srbext->DevExt->CtrlIdent.SUBNQN;
    ULONG nqn_size = (ULONG)strlen((char*)subnqn);
    ULONG vid_size = (ULONG)strlen((char*)NVME_CONST::VENDOR_ID);
    size_t buf_size = (ULONG)sizeof(VPD_IDENTIFICATION_PAGE) +
                        (ULONG)sizeof(VPD_IDENTIFICATION_DESCRIPTOR)
                        + nqn_size + vid_size + 1;
    SPC::CAutoPtr<VPD_IDENTIFICATION_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    PVPD_IDENTIFICATION_DESCRIPTOR desc = NULL;
    ULONG size = (ULONG)buf_size - sizeof(VPD_IDENTIFICATION_PAGE);
    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_DEVICE_IDENTIFIERS;
    page->PageLength = (UCHAR) min(size, MAXUCHAR);
    desc = (PVPD_IDENTIFICATION_DESCRIPTOR)page->Descriptors;

    desc->CodeSet = VpdCodeSetAscii;
    desc->IdentifierType = VpdIdentifierTypeVendorId;
    desc->Association = VpdAssocDevice;
    size = size - sizeof(VPD_IDENTIFICATION_DESCRIPTOR);
    desc->IdentifierLength = (UCHAR)min(size, 255);
    RtlCopyMemory(desc->Identifier, NVME_CONST::VENDOR_ID, vid_size);
    RtlCopyMemory(&desc->Identifier[vid_size], "_", 1);
    RtlCopyMemory(&desc->Identifier[vid_size + 1], subnqn, nqn_size);

    ret_size = (ULONG)min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);

    return SRB_STATUS_SUCCESS;
}
// 概要: SCSI 要求に対する応答バッファを作成します。
static UCHAR Reply_VpdBlockLimits(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    ULONG buf_size = sizeof(VPD_BLOCK_LIMITS_PAGE);
    SPC::CAutoPtr<VPD_BLOCK_LIMITS_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_BLOCK_LIMITS;
    REVERSE_BYTES_2(page->PageLength, &buf_size);

    ULONG max_tx = srbext->DevExt->MaxTxSize;
    REVERSE_BYTES_4(page->MaximumTransferLength, &max_tx);
    REVERSE_BYTES_4(page->OptimalTransferLength, &max_tx);

    USHORT granularity = 4;
    REVERSE_BYTES_2(page->OptimalTransferLengthGranularity, &granularity);

    ret_size = min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);

    return SRB_STATUS_SUCCESS;
}
// 概要: SCSI 要求に対する応答バッファを作成します。
static UCHAR Reply_VpdBlockDeviceCharacteristics(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    ULONG buf_size = sizeof(VPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE);
    SPC::CAutoPtr<VPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_BLOCK_DEVICE_CHARACTERISTICS;
    page->PageLength = (UCHAR)buf_size;
    page->MediumRotationRateLsb = 1;
    page->NominalFormFactor = 0;

    ret_size = min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);

    return SRB_STATUS_SUCCESS;
}
// 概要: Storport または SCSI のイベントを処理します。
static UCHAR HandleInquiryVPD(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    PCDB cdb = srbext->Cdb();
    UCHAR srb_status = SRB_STATUS_INVALID_REQUEST;

    switch (cdb->CDB6INQUIRY.PageCode)
    {
        case VPD_SUPPORTED_PAGES:
            srb_status = Reply_VpdSupportPages(srbext, ret_size);
            break;
        case VPD_SERIAL_NUMBER:
            srb_status = Reply_VpdSerialNumber(srbext, ret_size);
            break;
        case VPD_DEVICE_IDENTIFIERS:
            srb_status = Reply_VpdIdentifier(srbext, ret_size);
            break;
        case VPD_BLOCK_LIMITS:
            srb_status = Reply_VpdBlockLimits(srbext, ret_size);
            break;
        case VPD_BLOCK_DEVICE_CHARACTERISTICS:
            srb_status = Reply_VpdBlockDeviceCharacteristics(srbext, ret_size);
            break;
        default:
            srb_status = SRB_STATUS_ERROR;
            break;
    }

    return srb_status;
}
// 概要: 要求処理に必要なデータを構築します。
static void BuildInquiryData(PINQUIRYDATA data, char* vid, char* pid, char* rev)
{
    data->DeviceType = DIRECT_ACCESS_DEVICE;
    data->DeviceTypeQualifier = DEVICE_CONNECTED;
    data->RemovableMedia = 0;
    data->Versions = 0x06;
    data->NormACA = 0;
    data->HiSupport = 0;
    data->ResponseDataFormat = 2;
    data->AdditionalLength = INQUIRYDATABUFFERSIZE - 5;
    data->EnclosureServices = 0;
    data->MediumChanger = 0;
    data->CommandQueue = 1;
    data->Wide16Bit = 0;
    data->Addr16 = 0;
    data->Synchronous = 0;
    data->Reserved3[0] = 0;

    data->Wide32Bit = TRUE;
    data->LinkedCommands = FALSE;
    RtlCopyMemory((PUCHAR)&data->VendorId[0], vid, sizeof(data->VendorId));
    RtlCopyMemory((PUCHAR)&data->ProductId[0], pid, sizeof(data->ProductId));
    RtlCopyMemory((PUCHAR)&data->ProductRevisionLevel[0], rev, sizeof(data->ProductRevisionLevel));
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_RequestSense6(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;






}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Read6(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB6READWRITE, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Write6(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0;
    ULONG len = 0;
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB6READWRITE, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Inquiry6(PSPCNVME_SRBEXT srbext) 
{
    ULONG ret_size = 0;
    UCHAR srb_status = SRB_STATUS_ERROR;
    PCDB cdb = srbext->Cdb();

    if (cdb->CDB6INQUIRY3.EnableVitalProductData)
    {
        srb_status = HandleInquiryVPD(srbext, ret_size);
    }
    else
    {
        if (cdb->CDB6INQUIRY3.PageCode > 0) 
        {
            srb_status = SRB_STATUS_ERROR;
        }
        else
        {
            PINQUIRYDATA data = (PINQUIRYDATA)srbext->DataBuf();
            ULONG size = srbext->DataBufLen();
            ret_size = 0;
            srb_status = SRB_STATUS_DATA_OVERRUN;
            if(size >= INQUIRYDATABUFFERSIZE)
            {
                RtlZeroMemory(srbext->DataBuf(), srbext->DataBufLen());
                BuildInquiryData(data, (char*)NVME_CONST::VENDOR_ID,
                                (char*)NVME_CONST::PRODUCT_ID, 
                                (char*)NVME_CONST::PRODUCT_REV);
                srb_status = SRB_STATUS_SUCCESS;
                ret_size = size;
            }
        }
    }
    
    SrbSetDataTransferLength(srbext->Srb, ret_size);
    return srb_status;
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_Verify6(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSelect6(PSPCNVME_SRBEXT srbext)
{
    CDB::_MODE_SELECT *select = &srbext->Cdb()->MODE_SELECT;
    PUCHAR buffer = (PUCHAR)srbext->DataBuf();
    ULONG mode_data_size = 0;
    ULONG offset = 0;
    PMODE_PARAMETER_HEADER header = (PMODE_PARAMETER_HEADER)buffer;
    PMODE_PARAMETER_BLOCK param_block = (PMODE_PARAMETER_BLOCK)(header+1);
    PUCHAR cursor = ((PUCHAR)param_block + header->BlockDescriptorLength);

    if(0 == select->PFBit)
        return SRB_STATUS_INVALID_REQUEST;

#if DBG
    DbgBreakPoint();
#endif
    if(0 == header->BlockDescriptorLength)
        param_block = NULL;

    mode_data_size = srbext->DataBufLen() - sizeof(MODE_PARAMETER_HEADER) - header->BlockDescriptorLength;
    offset = 0;
    while(mode_data_size > 0)
    {
        PMODE_CACHING_PAGE page = (PMODE_CACHING_PAGE)(cursor + offset);
        mode_data_size -= (page->PageLength+2);
        offset += (page->PageLength + 2);

        if (0 == page->PageLength)
            break;

        if(page->PageCode != MODE_PAGE_CACHING || page->PageLength != (sizeof(MODE_CACHING_PAGE)-2))
            continue;

#if DBG
        DbgBreakPoint();
#endif
        srbext->DevExt->ReadCacheEnabled = !page->ReadDisableCache;
        srbext->DevExt->WriteCacheEnabled = page->WriteCacheEnable;
    }

    SrbSetDataTransferLength(srbext->Srb, 0);
    return SRB_STATUS_SUCCESS;
}

// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_ModeSense6(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_ERROR;
    PCDB cdb = srbext->Cdb();
    PUCHAR buffer = (PUCHAR)srbext->DataBuf();
    PMODE_PARAMETER_HEADER header = (PMODE_PARAMETER_HEADER)buffer;
    ULONG buf_size = srbext->DataBufLen();
    ULONG ret_size = 0;
    ULONG page_size = 0;

    if (buf_size < sizeof(MODE_PARAMETER_HEADER) || NULL == buffer)
    {
        srb_status = SRB_STATUS_DATA_OVERRUN;
        ret_size = sizeof(MODE_PARAMETER_HEADER);
        goto end;
    }

    FillParamHeader(header);
    buffer += sizeof(MODE_PARAMETER_HEADER);
    buf_size -= sizeof(MODE_PARAMETER_HEADER);
    ret_size += sizeof(MODE_PARAMETER_HEADER);

    switch (cdb->MODE_SENSE.PageCode)
    {
    case MODE_PAGE_CACHING:
    {
        page_size = ReplyModePageCaching(srbext->DevExt, buffer, buf_size, ret_size);
        header->ModeDataLength += (UCHAR)page_size;
        srb_status = SRB_STATUS_SUCCESS;
        break;
    }
    case MODE_PAGE_CONTROL:
    {
        page_size = ReplyModePageControl(buffer, buf_size, ret_size);
        header->ModeDataLength += (UCHAR)page_size;
        srb_status = SRB_STATUS_SUCCESS;
        break;
    }
    case MODE_PAGE_FAULT_REPORTING:
    {
        page_size = ReplyModePageInfoExceptionCtrl(buffer, buf_size, ret_size);
        header->ModeDataLength += (UCHAR)page_size;
        srb_status = SRB_STATUS_SUCCESS;
        break;
    }
    case MODE_SENSE_RETURN_ALL:
    {
        if (buf_size > 0)
        {
            page_size = ReplyModePageCaching(srbext->DevExt, buffer, buf_size, ret_size);
            header->ModeDataLength += (UCHAR)page_size;
        }
        if (buf_size > 0)
        {
            page_size = ReplyModePageControl(buffer, buf_size, ret_size);
            header->ModeDataLength += (UCHAR)page_size;
        }
        if (buf_size > 0)
        {
            page_size = ReplyModePageInfoExceptionCtrl(buffer, buf_size, ret_size);
            header->ModeDataLength += (UCHAR)page_size;
        }

        srb_status = SRB_STATUS_SUCCESS;
        break;
    }
    default:
    {
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    }
    }

end:
    SrbSetDataTransferLength(srbext->Srb, ret_size);
    return srb_status;
}
// 概要: 対応する SCSI コマンドを処理します。
UCHAR Scsi_TestUnitReady(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}

// NVMe Completion Status を Storport/SCSI の SRB_STATUS へ翻訳します。
// 上位スタックは NVMe の詳細コードを直接理解しないため、意味の近い SRB_STATUS と
// Sense 情報へ寄せて返します。
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeGenericToSrbStatus(NVME_COMMAND_STATUS &status)
{
    switch(status.SC)
    {
    case NVME_STATUS_INVALID_COMMAND_OPCODE:
        return SRB_STATUS_INVALID_REQUEST;
    case NVME_STATUS_INVALID_FIELD_IN_COMMAND:
    case NVME_STATUS_COMMAND_ID_CONFLICT:
        return SRB_STATUS_INVALID_PARAMETER;

    case NVME_STATUS_INVALID_USE_OF_CONTROLLER_MEMORY_BUFFER:
    case NVME_STATUS_PRP_OFFSET_INVALID:
    case NVME_STATUS_ATOMIC_WRITE_UNIT_EXCEEDED:
    case NVME_STATUS_OPERATION_DENIED:
    case NVME_STATUS_RESERVED:
    case NVME_STATUS_HOST_IDENTIFIER_INCONSISTENT_FORMAT:
    case NVME_STATUS_KEEP_ALIVE_TIMEOUT_EXPIRED:
    case NVME_STATUS_KEEP_ALIVE_TIMEOUT_INVALID:
    case NVME_STATUS_SANITIZE_FAILED:
    case NVME_STATUS_SGL_DATA_BLOCK_GRANULARITY_INVALID:
    case NVME_STATUS_DIRECTIVE_TYPE_INVALID:
    case NVME_STATUS_DIRECTIVE_ID_INVALID:
    case NVME_STATUS_NVM_LBA_OUT_OF_RANGE:
    case NVME_STATUS_NVM_CAPACITY_EXCEEDED:
    case NVME_STATUS_NVM_RESERVATION_CONFLICT:
    case NVME_STATUS_INTERNAL_DEVICE_ERROR:
    case NVME_STATUS_INVALID_NAMESPACE_OR_FORMAT:
    case NVME_STATUS_COMMAND_SEQUENCE_ERROR:
    case NVME_STATUS_INVALID_SGL_LAST_SEGMENT_DESCR:
    case NVME_STATUS_INVALID_NUMBER_OF_SGL_DESCR:
    case NVME_STATUS_DATA_SGL_LENGTH_INVALID:
    case NVME_STATUS_METADATA_SGL_LENGTH_INVALID:
    case NVME_STATUS_SGL_DESCR_TYPE_INVALID:
    case NVME_STATUS_SGL_OFFSET_INVALID:
    case NVME_STATUS_DATA_TRANSFER_ERROR:
        return SRB_STATUS_INTERNAL_ERROR;

    case NVME_STATUS_COMMAND_ABORTED_DUE_TO_POWER_LOSS_NOTIFICATION:
        return SRB_STATUS_NOT_POWERED;

    case NVME_STATUS_COMMAND_ABORT_REQUESTED:
    case NVME_STATUS_COMMAND_ABORTED_DUE_TO_SQ_DELETION:
    case NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_FUSED_COMMAND:
    case NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_MISSING_COMMAND:
    case NVME_STATUS_COMMAND_ABORTED_DUE_TO_PREEMPT_ABORT:
        return SRB_STATUS_ABORTED;

    case NVME_STATUS_SANITIZE_IN_PROGRESS:
    case NVME_STATUS_FORMAT_IN_PROGRESS:
        return SRB_STATUS_BUSY;
    case NVME_STATUS_NVM_NAMESPACE_NOT_READY:
        return SRB_STATUS_INVALID_LUN;
    }
    
    return SRB_STATUS_ERROR;
}
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeCmdSpecificToSrbStatus(NVME_COMMAND_STATUS &status)
{
    UNREFERENCED_PARAMETER(status);
    return SRB_STATUS_ERROR;
}
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeMediaErrorToSrbStatus(NVME_COMMAND_STATUS &status)
{
    UNREFERENCED_PARAMETER(status);
    return SRB_STATUS_ERROR;
}


#if 0

typedef enum {

    NVME_STATUS_SUCCESS_COMPLETION = 0x00,
    NVME_STATUS_INVALID_COMMAND_OPCODE = 0x01,
    NVME_STATUS_INVALID_FIELD_IN_COMMAND = 0x02,
    NVME_STATUS_COMMAND_ID_CONFLICT = 0x03,
    NVME_STATUS_DATA_TRANSFER_ERROR = 0x04,
    NVME_STATUS_COMMAND_ABORTED_DUE_TO_POWER_LOSS_NOTIFICATION = 0x05,
    NVME_STATUS_INTERNAL_DEVICE_ERROR = 0x06,
    NVME_STATUS_COMMAND_ABORT_REQUESTED = 0x07,
    NVME_STATUS_COMMAND_ABORTED_DUE_TO_SQ_DELETION = 0x08,
    NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_FUSED_COMMAND = 0x09,
    NVME_STATUS_COMMAND_ABORTED_DUE_TO_FAILED_MISSING_COMMAND = 0x0A,
    NVME_STATUS_INVALID_NAMESPACE_OR_FORMAT = 0x0B,
    NVME_STATUS_COMMAND_SEQUENCE_ERROR = 0x0C,
    NVME_STATUS_INVALID_SGL_LAST_SEGMENT_DESCR = 0x0D,
    NVME_STATUS_INVALID_NUMBER_OF_SGL_DESCR = 0x0E,
    NVME_STATUS_DATA_SGL_LENGTH_INVALID = 0x0F,
    NVME_STATUS_METADATA_SGL_LENGTH_INVALID = 0x10,
    NVME_STATUS_SGL_DESCR_TYPE_INVALID = 0x11,
    NVME_STATUS_INVALID_USE_OF_CONTROLLER_MEMORY_BUFFER = 0x12,
    NVME_STATUS_PRP_OFFSET_INVALID = 0x13,
    NVME_STATUS_ATOMIC_WRITE_UNIT_EXCEEDED = 0x14,
    NVME_STATUS_OPERATION_DENIED = 0x15,
    NVME_STATUS_SGL_OFFSET_INVALID = 0x16,
    NVME_STATUS_RESERVED = 0x17,
    NVME_STATUS_HOST_IDENTIFIER_INCONSISTENT_FORMAT = 0x18,
    NVME_STATUS_KEEP_ALIVE_TIMEOUT_EXPIRED = 0x19,
    NVME_STATUS_KEEP_ALIVE_TIMEOUT_INVALID = 0x1A,
    NVME_STATUS_COMMAND_ABORTED_DUE_TO_PREEMPT_ABORT = 0x1B,
    NVME_STATUS_SANITIZE_FAILED = 0x1C,
    NVME_STATUS_SANITIZE_IN_PROGRESS = 0x1D,
    NVME_STATUS_SGL_DATA_BLOCK_GRANULARITY_INVALID = 0x1E,

    NVME_STATUS_DIRECTIVE_TYPE_INVALID = 0x70,
    NVME_STATUS_DIRECTIVE_ID_INVALID = 0x71,

    NVME_STATUS_NVM_LBA_OUT_OF_RANGE = 0x80,
    NVME_STATUS_NVM_CAPACITY_EXCEEDED = 0x81,
    NVME_STATUS_NVM_NAMESPACE_NOT_READY = 0x82,
    NVME_STATUS_NVM_RESERVATION_CONFLICT = 0x83,
    NVME_STATUS_FORMAT_IN_PROGRESS = 0x84,

} NVME_STATUS_GENERIC_COMMAND_CODES;

typedef enum {

    NVME_STATUS_COMPLETION_QUEUE_INVALID = 0x00,
    NVME_STATUS_INVALID_QUEUE_IDENTIFIER = 0x01,
    NVME_STATUS_MAX_QUEUE_SIZE_EXCEEDED = 0x02,
    NVME_STATUS_ABORT_COMMAND_LIMIT_EXCEEDED = 0x03,
    NVME_STATUS_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED = 0x05,
    NVME_STATUS_INVALID_FIRMWARE_SLOT = 0x06,
    NVME_STATUS_INVALID_FIRMWARE_IMAGE = 0x07,
    NVME_STATUS_INVALID_INTERRUPT_VECTOR = 0x08,
    NVME_STATUS_INVALID_LOG_PAGE = 0x09,
    NVME_STATUS_INVALID_FORMAT = 0x0A,
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_CONVENTIONAL_RESET = 0x0B,
    NVME_STATUS_INVALID_QUEUE_DELETION = 0x0C,
    NVME_STATUS_FEATURE_ID_NOT_SAVEABLE = 0x0D,
    NVME_STATUS_FEATURE_NOT_CHANGEABLE = 0x0E,
    NVME_STATUS_FEATURE_NOT_NAMESPACE_SPECIFIC = 0x0F,
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_NVM_SUBSYSTEM_RESET = 0x10,
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_RESET = 0x11,
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_MAX_TIME_VIOLATION = 0x12,
    NVME_STATUS_FIRMWARE_ACTIVATION_PROHIBITED = 0x13,
    NVME_STATUS_OVERLAPPING_RANGE = 0x14,

    NVME_STATUS_NAMESPACE_INSUFFICIENT_CAPACITY = 0x15,
    NVME_STATUS_NAMESPACE_IDENTIFIER_UNAVAILABLE = 0x16,
    NVME_STATUS_NAMESPACE_ALREADY_ATTACHED = 0x18,
    NVME_STATUS_NAMESPACE_IS_PRIVATE = 0x19,
    NVME_STATUS_NAMESPACE_NOT_ATTACHED = 0x1A,
    NVME_STATUS_NAMESPACE_THIN_PROVISIONING_NOT_SUPPORTED = 0x1B,
    NVME_STATUS_CONTROLLER_LIST_INVALID = 0x1C,

    NVME_STATUS_DEVICE_SELF_TEST_IN_PROGRESS = 0x1D,

    NVME_STATUS_BOOT_PARTITION_WRITE_PROHIBITED = 0x1E,

    NVME_STATUS_INVALID_CONTROLLER_IDENTIFIER = 0x1F,
    NVME_STATUS_INVALID_SECONDARY_CONTROLLER_STATE = 0x20,
    NVME_STATUS_INVALID_NUMBER_OF_CONTROLLER_RESOURCES = 0x21,
    NVME_STATUS_INVALID_RESOURCE_IDENTIFIER = 0x22,

    NVME_STATUS_SANITIZE_PROHIBITED_ON_PERSISTENT_MEMORY = 0x23,

    NVME_STATUS_INVALID_ANA_GROUP_IDENTIFIER = 0x24,
    NVME_STATUS_ANA_ATTACH_FAILED = 0x25,

    NVME_IO_COMMAND_SET_NOT_SUPPORTED = 0x29,
    NVME_IO_COMMAND_SET_NOT_ENABLED = 0x2A,
    NVME_IO_COMMAND_SET_COMBINATION_REJECTED = 0x2B,
    NVME_IO_COMMAND_SET_INVALID = 0x2C,

    NVME_STATUS_STREAM_RESOURCE_ALLOCATION_FAILED = 0x7F,
    NVME_STATUS_ZONE_INVALID_FORMAT = 0x7F,

    NVME_STATUS_NVM_CONFLICTING_ATTRIBUTES = 0x80,
    NVME_STATUS_NVM_INVALID_PROTECTION_INFORMATION = 0x81,
    NVME_STATUS_NVM_ATTEMPTED_WRITE_TO_READ_ONLY_RANGE = 0x82,
    NVME_STATUS_NVM_COMMAND_SIZE_LIMIT_EXCEEDED = 0x83,

    NVME_STATUS_ZONE_BOUNDARY_ERROR = 0xB8,
    NVME_STATUS_ZONE_FULL = 0xB9,
    NVME_STATUS_ZONE_READ_ONLY = 0xBA,
    NVME_STATUS_ZONE_OFFLINE = 0xBB,
    NVME_STATUS_ZONE_INVALID_WRITE = 0xBC,
    NVME_STATUS_ZONE_TOO_MANY_ACTIVE = 0xBD,
    NVME_STATUS_ZONE_TOO_MANY_OPEN = 0xBE,
    NVME_STATUS_ZONE_INVALID_STATE_TRANSITION = 0xBF,

} NVME_STATUS_COMMAND_SPECIFIC_CODES;

typedef enum {

    NVME_STATUS_NVM_WRITE_FAULT = 0x80,
    NVME_STATUS_NVM_UNRECOVERED_READ_ERROR = 0x81,
    NVME_STATUS_NVM_END_TO_END_GUARD_CHECK_ERROR = 0x82,
    NVME_STATUS_NVM_END_TO_END_APPLICATION_TAG_CHECK_ERROR = 0x83,
    NVME_STATUS_NVM_END_TO_END_REFERENCE_TAG_CHECK_ERROR = 0x84,
    NVME_STATUS_NVM_COMPARE_FAILURE = 0x85,
    NVME_STATUS_NVM_ACCESS_DENIED = 0x86,
    NVME_STATUS_NVM_DEALLOCATED_OR_UNWRITTEN_LOGICAL_BLOCK = 0x87,

} NVME_STATUS_MEDIA_ERROR_CODES;

#endif


// SRB_STATUS と SCSI Sense Data の補助処理です。
// エラー時に単に SRB_STATUS_ERROR を返すだけでは原因が伝わりにくいため、
// 必要に応じて Sense Key/ASC/ASCQ を補います。
// 概要: NVMe の状態や完了コードを変換します。
UCHAR NvmeToSrbStatus(NVME_COMMAND_STATUS& status)
{
    if(0 == (status.SCT & status.SC))
        return SRB_STATUS_SUCCESS;

    switch(status.SCT)
    {
    case NVME_STATUS_TYPE_GENERIC_COMMAND:
        return NvmeGenericToSrbStatus(status);
    case NVME_STATUS_TYPE_COMMAND_SPECIFIC:
        return NvmeCmdSpecificToSrbStatus(status);
    case NVME_STATUS_TYPE_MEDIA_ERROR:
        return NvmeMediaErrorToSrbStatus(status);
    }
    return SRB_STATUS_INTERNAL_ERROR;
}
// 概要: デバイスまたは内部設定を更新します。
void SetScsiSenseBySrbStatus(PSTORAGE_REQUEST_BLOCK srb, UCHAR &status)
{
    switch (status)
    {
        case SRB_STATUS_SUCCESS:
            SrbSetScsiStatus(srb, SCSISTAT_GOOD);
            break;
        case SRB_STATUS_BUSY:
            SrbSetScsiStatus(srb, SCSISTAT_BUSY);
            break;
        case SRB_STATUS_ERROR:
        {

            PSENSE_DATA sdata = (PSENSE_DATA)SrbGetSenseInfoBuffer(srb);
            UCHAR sdata_size = SrbGetSenseInfoBufferLength(srb);
            if (NULL == sdata || sdata_size == 0)
            {
                SrbSetScsiStatus(srb, SCSISTAT_CONDITION_MET);
            }
            else
            {
                RtlZeroMemory(sdata, sdata_size);
                sdata->ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
                sdata->Valid = 0;
                sdata->AdditionalSenseLength = sdata_size - FIELD_OFFSET(SENSE_DATA, AdditionalSenseLength);
                sdata->AdditionalSenseCodeQualifier = 0;
                sdata->SenseKey = SCSI_SENSE_ILLEGAL_REQUEST;
                sdata->AdditionalSenseCode = SCSI_ADSENSE_ILLEGAL_COMMAND;
                SrbSetScsiStatus(srb, SCSISTAT_CHECK_CONDITION);
                status = status | SRB_STATUS_AUTOSENSE_VALID;
            }
        }
			break;
    }
}


// Windows がドライバをロードしたときの入口です。
// HW_INITIALIZATION_DATA に Storport ミニポートのコールバックと拡張領域サイズを設定し、
// StorPortInitialize で OS のストレージスタックへ登録します。
EXTERN_C_START
sp_DRIVER_INITIALIZE DriverEntry;
// 概要: ドライバを Storport ミニポートとして初期登録します。
ULONG DriverEntry(IN PVOID DrvObj, IN PVOID RegPath)
{
    CDebugCallInOut inout(__FUNCTION__);
    if (IsSupportedOS(10) == FALSE)
        return STOR_STATUS_UNSUPPORTED_VERSION;

    HW_INITIALIZATION_DATA init_data = { 0 };
    ULONG status = 0;

    init_data.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);

    init_data.HwInitialize = HwInitialize;
    init_data.HwBuildIo = HwBuildIo;
    init_data.HwStartIo = HwStartIo;
    init_data.HwFindAdapter = HwFindAdapter;
    init_data.HwResetBus = HwResetBus;
    init_data.HwAdapterControl = HwAdapterControl;
    init_data.HwTracingEnabled = HwTracingEnabled;
    init_data.HwCleanupTracing = HwCleanupTracing;

    init_data.HwProcessServiceRequest = HwProcessServiceRequest;
    init_data.HwCompleteServiceIrp = HwCompleteServiceIrp;

    init_data.AutoRequestSense = TRUE;
    init_data.NeedPhysicalAddresses = TRUE;
    init_data.AdapterInterfaceType = PCIBus;
    init_data.MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    init_data.TaggedQueuing = TRUE;
    init_data.MultipleRequestPerLu = TRUE;
    init_data.NumberOfAccessRanges = 2;
    init_data.SrbTypeFlags = SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK;
    
    init_data.FeatureSupport = STOR_FEATURE_FULL_PNP_DEVICE_CAPABILITIES /* | STOR_FEATURE_NVME*/;

    /* Set required extension sizes. */
    init_data.DeviceExtensionSize = sizeof(CNvmeDevice);
    init_data.SrbExtensionSize = sizeof(SPCNVME_SRBEXT);

    status = StorPortInitialize(DrvObj, RegPath, &init_data, NULL);

    return status;
}
EXTERN_C_END

// Storport から直接呼ばれるミニポートコールバック群です。
// アダプタ検出、初期化、I/O 受付、リセット、PnP/電源制御、トレースなど、
// ドライバの外向きの振る舞いはこのセクションが中心です。
// 概要: 応答用構造体へ必要な情報を設定します。
static void FillPortConfiguration(PPORT_CONFIGURATION_INFORMATION portcfg, CNvmeDevice* nvme)
{
    portcfg->MaximumTransferLength = nvme->MaxTxSize;
    portcfg->NumberOfPhysicalBreaks = nvme->MaxTxPages;
    portcfg->AlignmentMask = FILE_LONG_ALIGNMENT;
    portcfg->MiniportDumpData = NULL;
    portcfg->InitiatorBusId[0] = 1;
    portcfg->CachesData = FALSE;
    portcfg->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    portcfg->MaximumNumberOfTargets = NVME_CONST::MAX_TARGETS;
    portcfg->SrbType = SRB_TYPE_STORAGE_REQUEST_BLOCK;
    portcfg->DeviceExtensionSize = sizeof(CNvmeDevice);
    portcfg->SrbExtensionSize = sizeof(SPCNVME_SRBEXT);
    portcfg->MaximumNumberOfLogicalUnits = NVME_CONST::MAX_LU;
    portcfg->SynchronizationModel = StorSynchronizeFullDuplex;
    portcfg->HwMSInterruptRoutine = CNvmeDevice::NvmeMsixISR;
    portcfg->InterruptSynchronizationMode = InterruptSynchronizePerMessage;
    portcfg->NumberOfBuses = 1;
    portcfg->ScatterGather = TRUE;
    portcfg->Master = TRUE;
    portcfg->AddressType = STORAGE_ADDRESS_TYPE_BTL8;
    portcfg->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;
    portcfg->MaxNumberOfIO = NVME_CONST::MAX_IO_PER_LU * NVME_CONST::MAX_LU;
    portcfg->MaxIOsPerLun = NVME_CONST::MAX_IO_PER_LU;

    portcfg->InitialLunQueueDepth = NVME_CONST::MAX_IO_PER_LU;

    portcfg->RequestedDumpBufferSize = 0;
    portcfg->DumpMode = 0;
    portcfg->DumpRegion.VirtualBase = NULL;
    portcfg->DumpRegion.PhysicalBase.QuadPart = NULL;
    portcfg->DumpRegion.Length = 0;
    portcfg->FeatureSupport = STOR_ADAPTER_DMA_V3_PREFERRED;
}

_Use_decl_annotations_ ULONG HwFindAdapter(
    _In_ PVOID devext,
    _In_ PVOID ctx,
    _In_ PVOID businfo,
    _In_z_ PCHAR arg_str,
    _Inout_ PPORT_CONFIGURATION_INFORMATION port_cfg,
    _In_ PBOOLEAN Reserved3)
{
    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(businfo);
    UNREFERENCED_PARAMETER(arg_str);
    UNREFERENCED_PARAMETER(Reserved3);

    CNvmeDevice* nvme = (CNvmeDevice*)devext;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    status = nvme->Setup(port_cfg);
    if (!NT_SUCCESS(status))
        goto error;

    status = nvme->InitController();
    if (!NT_SUCCESS(status))
        goto error;

    status = nvme->IdentifyController(NULL, &nvme->CtrlIdent, true);
    if (!NT_SUCCESS(status))
        goto error;
    
    FillPortConfiguration(port_cfg, nvme);

    return SP_RETURN_FOUND;

error:
    nvme->Teardown();
    return SP_RETURN_ERROR;
}

_Use_decl_annotations_ BOOLEAN HwInitialize(PVOID devext)
{

    CDebugCallInOut inout(__FUNCTION__);
    CNvmeDevice* nvme = (CNvmeDevice*)devext;
    NTSTATUS status = nvme->SetPerfOpts();

    if(!NT_SUCCESS(status))
        return FALSE;

    StorPortEnablePassiveInitialization(devext, HwPassiveInitialize);
    return TRUE;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
BOOLEAN HwPassiveInitialize(PVOID devext)
{
    CDebugCallInOut inout(__FUNCTION__);
    CNvmeDevice* nvme = (CNvmeDevice*)devext;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if(!nvme->IsWorking())
        return FALSE;
    
    StorPortPause(devext, MAXULONG);

    status = nvme->InitNvmeStage1();
    if (!NT_SUCCESS(status))
        return FALSE;

    status = nvme->InitNvmeStage2();
    if (!NT_SUCCESS(status))
        return FALSE;

    status = nvme->CreateIoQueues();
    if (!NT_SUCCESS(status))
        return FALSE;

    status = nvme->RegisterIoQueues(NULL);
    if (!NT_SUCCESS(status))
        return FALSE;

    StorPortResume(devext);
    return TRUE;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
BOOLEAN HwBuildIo(_In_ PVOID devext,_In_ PSCSI_REQUEST_BLOCK srb)
{
    PSPCNVME_SRBEXT srbext = SPCNVME_SRBEXT::InitSrbExt(devext, (PSTORAGE_REQUEST_BLOCK)srb);
    BOOLEAN need_startio = FALSE;

    switch (srbext->FuncCode())
    {
    case SRB_FUNCTION_ABORT_COMMAND:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RESET_BUS:
		srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
        need_startio = FALSE;
        break;

    case SRB_FUNCTION_POWER:
        need_startio = BuildIo_SrbPowerHandler(srbext);
        break;
    case SRB_FUNCTION_EXECUTE_SCSI:
        need_startio = BuildIo_ScsiHandler(srbext);
        break;
    case SRB_FUNCTION_IO_CONTROL:
        need_startio = BuildIo_IoctlHandler(srbext);
        break;
    case SRB_FUNCTION_PNP:
        need_startio = BuildIo_SrbPnpHandler(srbext);
        break;
	default:
        need_startio = BuildIo_DefaultHandler(srbext);
        break;

    }
    return need_startio;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
BOOLEAN HwStartIo(PVOID devext, PSCSI_REQUEST_BLOCK srb)
{
    PSPCNVME_SRBEXT srbext = SPCNVME_SRBEXT::GetSrbExt((PSTORAGE_REQUEST_BLOCK)srb);
    UCHAR srb_status = SRB_STATUS_ERROR;

    switch (srbext->FuncCode())
    {
    case SRB_FUNCTION_EXECUTE_SCSI:
        srb_status = StartIo_ScsiHandler(srbext);
        break;
    case SRB_FUNCTION_IO_CONTROL:
        srb_status = StartIo_IoctlHandler(srbext);
        break;
    default:
        srb_status = StartIo_DefaultHandler(srbext);
        break;

    }

    if (srb_status != SRB_STATUS_PENDING)
        srbext->CompleteSrb(srb_status);

    return TRUE;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
BOOLEAN HwResetBus(
    PVOID DeviceExtension,
    ULONG PathId
)
{
    UNREFERENCED_PARAMETER(PathId);
    CNvmeDevice* nvme = (CNvmeDevice*)DeviceExtension;
    DbgBreakPoint();
    nvme->ResetOutstandingCmds();
    return TRUE;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
SCSI_ADAPTER_CONTROL_STATUS HwAdapterControl(
    PVOID DeviceExtension,
    SCSI_ADAPTER_CONTROL_TYPE ControlType,
    PVOID Parameters
)
{
    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(ControlType);
    UNREFERENCED_PARAMETER(Parameters);
    SCSI_ADAPTER_CONTROL_STATUS status = ScsiAdapterControlUnsuccessful;
    CNvmeDevice *nvme = (CNvmeDevice*)DeviceExtension;

    switch (ControlType)
    {
    case ScsiQuerySupportedControlTypes:
    {
        status = Handle_QuerySupportedControlTypes((PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters);
        break;
    }
    case ScsiStopAdapter:
    {
        status = ScsiAdapterControlSuccess;
        break;
    }
    case ScsiRestartAdapter:
    {
        status = Handle_RestartAdapter(nvme);
        break;
    }
    case ScsiAdapterSurpriseRemoval:
    {
        nvme->Teardown();
        status = ScsiAdapterControlSuccess;
        break;
    }
    case ScsiPowerSettingNotification:
    {
        STOR_POWER_SETTING_INFO* info = (STOR_POWER_SETTING_INFO*) Parameters;
        UNREFERENCED_PARAMETER(info);
        status = ScsiAdapterControlSuccess;
        break;
    }

    case ScsiAdapterPower:
    {
      STOR_ADAPTER_CONTROL_POWER *power = (STOR_ADAPTER_CONTROL_POWER *)Parameters;
      UNREFERENCED_PARAMETER(power);
      status = ScsiAdapterControlSuccess;
      break;
    }

#pragma region === Some explain of un-implemented control codes ===

#pragma endregion

    default:
        status = ScsiAdapterControlUnsuccessful;
    }
    return status;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
void HwProcessServiceRequest(
    PVOID DeviceExtension,
    PVOID Irp
)
{
    
    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(DeviceExtension);
    PIRP irp = (PIRP) Irp;

    irp->IoStatus.Information = 0;
    irp->IoStatus.Status = STATUS_SUCCESS;
    StorPortCompleteServiceIrp(DeviceExtension, irp);
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
void HwCompleteServiceIrp(PVOID DeviceExtension)
{


    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(DeviceExtension);
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
SCSI_UNIT_CONTROL_STATUS HwUnitControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_UNIT_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(ControlType);
    UNREFERENCED_PARAMETER(Parameters);


    
    return ScsiUnitControlSuccess;
}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
VOID HwTracingEnabled(
    _In_ PVOID HwDeviceExtension,
    _In_ BOOLEAN Enabled
)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(Enabled);

}

// 概要: Storport から呼ばれるミニポートコールバックを処理します。
_Use_decl_annotations_
VOID HwCleanupTracing(
    _In_ PVOID  Arg1
)
{
    UNREFERENCED_PARAMETER(Arg1);
}

// NVMe コントローラを表す CNvmeDevice の実装です。
// PCI BAR のマップ、レジスタアクセス、Admin Queue/IO Queue の作成、Identify、
// Feature 設定、リセット、シャットダウンまでを管理します。
// 概要: NVMe の状態や完了コードを変換します。
BOOLEAN CNvmeDevice::NvmeMsixISR(IN PVOID devext, IN ULONG msgid)
{
    CNvmeDevice* nvme = (CNvmeDevice*)devext;
    CNvmeQueue *queue = (msgid == 0)? nvme->AdmQueue : nvme->IoQueue[msgid-1];
    if (NULL == queue || !nvme->IsWorking())
        goto END;

    BOOLEAN ok = FALSE;
    ok = StorPortIssueDpc(devext, &queue->QueueCplDpc, NULL, NULL);
END:
    return TRUE;
}
// 概要: RestartAdapterDpc の処理を行います。
void CNvmeDevice::RestartAdapterDpc(
    IN PSTOR_DPC  Dpc,
    IN PVOID  DevExt,
    IN PVOID  Arg1,
    IN PVOID  Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    CNvmeDevice *nvme = (CNvmeDevice*)DevExt;
    ULONG stor_status = STOR_STATUS_SUCCESS;
    if(!nvme->IsWorking())
        return;

    StorPortInitializeWorker(nvme, &nvme->RestartWorker);
    stor_status = StorPortQueueWorkItem(DevExt, CNvmeDevice::RestartAdapterWorker, nvme->RestartWorker, NULL);
    ASSERT(stor_status == STOR_STATUS_SUCCESS);
}
// 概要: RestartAdapterWorker の処理を行います。
void CNvmeDevice::RestartAdapterWorker(
    _In_ PVOID DevExt,
    _In_ PVOID Context,
    _In_ PVOID Worker)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Worker);

    CNvmeDevice* nvme = (CNvmeDevice*)DevExt;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (!nvme->IsWorking())
        return;

    status = nvme->InitNvmeStage1();
    ASSERT(NT_SUCCESS(status));

    status = nvme->InitNvmeStage2();
    ASSERT(NT_SUCCESS(status));
    StorPortResume(DevExt);
    StorPortFreeWorker(nvme, &nvme->RestartWorker);
    nvme->RestartWorker = NULL;
}

#pragma region ======== CSpcNvmeDevice inline routines ======== 
// 概要: レジスタまたはデバイス情報を読み取ります。
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier)
{
    if(barrier)
        MemoryBarrier();
    cc.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->CC.AsUlong);
}
// 概要: レジスタまたはデバイス情報を読み取ります。
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    csts.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->CSTS.AsUlong);
}
// 概要: レジスタまたはデバイス情報を読み取ります。
inline void CNvmeDevice::ReadNvmeRegister(NVME_VERSION& ver, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    ver.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->VS.AsUlong);
}
// 概要: レジスタまたはデバイス情報を読み取ります。
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_CAPABILITIES& cap, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    cap.AsUlonglong = StorPortReadRegisterUlong64(this, &CtrlReg->CAP.AsUlonglong);
}
// 概要: レジスタまたはデバイス情報を読み取ります。
inline void CNvmeDevice::ReadNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES& aqa,
    NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS& asq,
    NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS& acq,
    bool barrier)
{
    if (barrier)
        MemoryBarrier();

    aqa.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->AQA.AsUlong);
    asq.AsUlonglong = StorPortReadRegisterUlong64(this, &CtrlReg->ASQ.AsUlonglong);
    acq.AsUlonglong = StorPortReadRegisterUlong64(this, &CtrlReg->ACQ.AsUlonglong);
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
inline void CNvmeDevice::WriteNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    StorPortWriteRegisterUlong(this, &CtrlReg->CC.AsUlong, cc.AsUlong);
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
inline void CNvmeDevice::WriteNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    StorPortWriteRegisterUlong(this, &CtrlReg->CSTS.AsUlong, csts.AsUlong);
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
inline void CNvmeDevice::WriteNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES& aqa,
    NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS& asq,
    NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS& acq,
    bool barrier)
{
    if (barrier)
        MemoryBarrier();

    StorPortWriteRegisterUlong(this, &CtrlReg->AQA.AsUlong, aqa.AsUlong);
    StorPortWriteRegisterUlong64(this, &CtrlReg->ASQ.AsUlonglong, asq.AsUlonglong);
    StorPortWriteRegisterUlong64(this, &CtrlReg->ACQ.AsUlonglong, acq.AsUlonglong);
}
// 概要: 指定条件を満たすかどうかを判定します。
inline BOOLEAN CNvmeDevice::IsControllerEnabled(bool barrier)
{
    NVME_CONTROLLER_CONFIGURATION cc = {0};
    ReadNvmeRegister(cc, barrier);
    return (TRUE == cc.EN)?TRUE:FALSE;
}
// 概要: 指定条件を満たすかどうかを判定します。
inline BOOLEAN CNvmeDevice::IsControllerReady(bool barrier)
{
    NVME_CONTROLLER_STATUS csts = { 0 };
    ReadNvmeRegister(csts, barrier);
    return (TRUE == csts.RDY && FALSE == csts.CFS) ? TRUE : FALSE;
}
// 概要: 内部状態やデバイス情報を取得します。
inline void CNvmeDevice::GetAdmQueueDbl(PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL& sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL& cpl)
{
    GetQueueDbl(0, sub, cpl);
}
// 概要: 内部状態やデバイス情報を取得します。
inline void CNvmeDevice::GetQueueDbl(ULONG qid, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL& sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL& cpl)
{
    if (NULL == Doorbells)
    {
        sub = NULL;
        cpl = NULL;
        return;
    }

    sub = (PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL)&Doorbells[qid*2];
    cpl = (PNVME_COMPLETION_QUEUE_HEAD_DOORBELL)&Doorbells[qid*2+1];
}
// 概要: デバイス能力から内部キャッシュ値を更新します。
inline void CNvmeDevice::UpdateMaxTxSize()
{
    this->MaxTxSize = (ULONG)((1 << this->CtrlIdent.MDTS) * this->MinPageSize);
    this->MaxTxPages = (ULONG)(this->MaxTxSize / PAGE_SIZE);
}
#if 0
// 概要: MinPageSize の処理を行います。
ULONG CNvmeDevice::MinPageSize()
{
    return (ULONG)(1 << (12 + CtrlCap.MPSMIN));
}
// 概要: MaxPageSize の処理を行います。
ULONG CNvmeDevice::MaxPageSize()
{
    return (ULONG)(1 << (12 + CtrlCap.MPSMAX));
}
// 概要: MaxTxSize の処理を行います。
ULONG CNvmeDevice::MaxTxSize()
{
    return (ULONG)((1 << this->CtrlIdent.MDTS) * MinPageSize());
}
// 概要: MaxTxPages の処理を行います。
ULONG CNvmeDevice::MaxTxPages()
{
    return (ULONG)(MaxTxSize() / PAGE_SIZE);
}
// 概要: NsCount の処理を行います。
ULONG CNvmeDevice::NsCount()
{
    return NamespaceCount;
}
#endif
bool CNvmeDevice::IsWorking() { return (State == NVME_STATE::RUNNING); }
// 概要: return の処理を行います。
bool CNvmeDevice::IsSetup() { return (State == NVME_STATE::SETUP); }
// 概要: return の処理を行います。
bool CNvmeDevice::IsTeardown() { return (State == NVME_STATE::TEARDOWN); }
// 概要: return の処理を行います。
bool CNvmeDevice::IsStop() { return (State == NVME_STATE::STOP); }
#pragma endregion

#pragma region ======== CSpcNvmeDevice ======== 
#if 0
// 概要: 内部状態やデバイス情報を取得します。
bool CNvmeDevice::GetMsixTable()
{
}
#endif 
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::Setup(PPORT_CONFIGURATION_INFORMATION pci)
{
    NTSTATUS status = STATUS_SUCCESS;
    if(NVME_STATE::STOP != State)
        return STATUS_INVALID_DEVICE_STATE;

    State = NVME_STATE::SETUP;
    InitVars();
    LoadRegistry();
    GetPciBusData(pci->AdapterInterfaceType, pci->SystemIoBusNumber, pci->SlotNumber);
    PortCfg = pci;

    AccessRangeCount = min(ACCESS_RANGE_COUNT, PortCfg->NumberOfAccessRanges);
    RtlCopyMemory(AccessRanges, PortCfg->AccessRanges, 
            sizeof(ACCESS_RANGE) * AccessRangeCount);
    if(!MapCtrlRegisters())
        return STATUS_NOT_MAPPED_DATA;

    ReadCtrlCap();

    status = CreateAdmQ();
    if (!NT_SUCCESS(status))
        return status;

    State = NVME_STATE::RUNNING;
    return STATUS_SUCCESS;
}
// 概要: デバイスまたはキューの終了処理を行います。
void CNvmeDevice::Teardown()
{
    if (!IsWorking())
        return;

    State = NVME_STATE::TEARDOWN;
    DeleteIoQ();
    DeleteAdmQ();
    State = NVME_STATE::STOP;
}
// 概要: EnableController の処理を行います。
NTSTATUS CNvmeDevice::EnableController()
{
        return STATUS_SUCCESS;

    bool ok = WaitForCtrlerState(DeviceTimeout, FALSE, FALSE);
    if (!ok)
        return STATUS_INVALID_DEVICE_STATE;

    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    cc.CSS = NVME_CSS_NVM_COMMAND_SET;
    cc.AMS = NVME_AMS_ROUND_ROBIN;
    cc.SHN = NVME_CC_SHN_NO_NOTIFICATION;
    cc.IOSQES = NVME_CONST::IOSQES;
    cc.IOCQES = NVME_CONST::IOCQES;
    cc.EN = 0;
    WriteNvmeRegister(cc);

    StorPortStallExecution(StallDelay);

    cc.EN = 1;
    WriteNvmeRegister(cc);

    ok = WaitForCtrlerState(DeviceTimeout, TRUE);

    if(!ok)
        return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}
// 概要: DisableController の処理を行います。
NTSTATUS CNvmeDevice::DisableController()
{

    if(!IsControllerReady())
        return STATUS_SUCCESS;

    bool ok = WaitForCtrlerState(DeviceTimeout, TRUE, TRUE);
    if (!ok)
        return STATUS_INVALID_DEVICE_STATE;

    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    ReadNvmeRegister(cc);
    cc.EN = 0;
    WriteNvmeRegister(cc);

    StorPortStallExecution(StallDelay);
    ok = WaitForCtrlerState(DeviceTimeout, FALSE);

    if (!ok)
        return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

// 概要: ShutdownController の処理を行います。
NTSTATUS CNvmeDevice::ShutdownController()
{
    if (!IsStop() && !IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    State = NVME_STATE::SHUTDOWN;
    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    bool ok = WaitForCtrlerState(DeviceTimeout, TRUE, TRUE);
    if (!ok)
        goto ERROR_BSOD;

    ReadNvmeRegister(cc);
    cc.SHN = NVME_CC_SHN_NORMAL_SHUTDOWN;
    WriteNvmeRegister(cc);

    ok = WaitForCtrlerShst(DeviceTimeout);

    return DisableController();

ERROR_BSOD:
    NVME_CONTROLLER_STATUS csts = { 0 };
    ReadNvmeRegister(cc);
    ReadNvmeRegister(csts);
    KeBugCheckEx(BUGCHECK_ADAPTER, (ULONG_PTR)this, (ULONG_PTR)cc.AsUlong, (ULONG_PTR)csts.AsUlong, 0);
}
// 概要: 内部変数やデバイス状態を初期化します。
NTSTATUS CNvmeDevice::InitController()
{
        return STATUS_INVALID_DEVICE_STATE;

    NTSTATUS status = STATUS_SUCCESS;
    status = DisableController();
    
    RegisteredIoQ = 0;

    if(!NT_SUCCESS(status))
        return status;
    status = RegisterAdmQ();
    if (!NT_SUCCESS(status))
        return status;
    status = EnableController();
    return status;
}
// 概要: 内部変数やデバイス状態を初期化します。
NTSTATUS CNvmeDevice::InitNvmeStage1()
{
    NTSTATUS status = STATUS_SUCCESS;

    status = IdentifyController(NULL, &this->CtrlIdent);
    if (!NT_SUCCESS(status))
        return status;

    if (1 == this->NvmeVer.MJR && 0 == this->NvmeVer.MNR)
        status = IdentifyFirstNamespace();
    else
        status = IdentifyAllNamespaces();

    if (!NT_SUCCESS(status))
        return status;

    return status;
}
// 概要: 内部変数やデバイス状態を初期化します。
NTSTATUS CNvmeDevice::InitNvmeStage2()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    status = SetInterruptCoalescing();
    if (!NT_SUCCESS(status))
        return status;
    status = SetArbitration();
    if (!NT_SUCCESS(status))
        return status;
    status = SetSyncHostTime();
    if (!NT_SUCCESS(status))
        return status;
    status = SetPowerManagement();
    if (!NT_SUCCESS(status))
        return status;
    status = SetAsyncEvent();
    return status;
}
// 概要: RestartController の処理を行います。
NTSTATUS CNvmeDevice::RestartController()
{
    BOOLEAN ok = FALSE;
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    StorPortPause(this, MAXULONG);

    ok = StorPortIssueDpc(this, &this->RestartDpc, NULL, NULL);
    ASSERT(ok);
    return STATUS_SUCCESS;
}
// 概要: IdentifyAllNamespaces の処理を行います。
NTSTATUS CNvmeDevice::IdentifyAllNamespaces()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    CAutoPtr<ULONG, NonPagedPool, DEV_POOL_TAG> idlist(new ULONG[NVME_CONST::MAX_NS_COUNT]);
    ULONG ret_count = 0;
    status = IdentifyActiveNamespaceIdList(NULL, idlist, ret_count);
    if(!NT_SUCCESS(status))
        return status;

    ULONG *nsid_list = idlist;
    this->NamespaceCount = min(ret_count, NVME_CONST::SUPPORT_NAMESPACES);
    for (ULONG i = 0; i < NamespaceCount; i++)
    {
        if(0 == nsid_list[i])
            break;

        status = IdentifyNamespace(NULL, nsid_list[i], &this->NsData[i]);
        if (!NT_SUCCESS(status))
            return status;
    }
    return STATUS_SUCCESS;
}
// 概要: IdentifyFirstNamespace の処理を行います。
NTSTATUS CNvmeDevice::IdentifyFirstNamespace()
{
    NTSTATUS status = IdentifyNamespace(NULL, 1, &this->NsData[0]);
    if(NT_SUCCESS(status))
        NamespaceCount = 1;
    return status;
}
// 概要: NVMe キューまたは関連リソースを作成します。
NTSTATUS CNvmeDevice::CreateIoQueues(bool force)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if(force)
        DeleteIoQ();

    if(0 == this->DesiredIoQ)
    {
        status = SetNumberOfIoQueue((USHORT)this->DesiredIoQ);
        if(!NT_SUCCESS(status))
            return status;
    }
    status = CreateIoQ();
    return status;
}
// 概要: IdentifyController の処理を行います。
NTSTATUS CNvmeDevice::IdentifyController(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA ident, bool poll)
{
        return STATUS_INVALID_DEVICE_STATE;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> srbext_ptr;
    PSPCNVME_SRBEXT my_srbext = srbext;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    
    if(NULL == srbext)
    {
        srbext_ptr.Reset(new SPCNVME_SRBEXT());
        srbext_ptr->Init(this, NULL);
        my_srbext = srbext_ptr.Get();
    }
    
    BuildCmd_IdentCtrler(my_srbext, ident);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);
    
    if(!NT_SUCCESS(status))
        goto END;

    do
    {
        StorPortStallExecution(StallDelay);
        if(poll)
        {
            AdmQueue->CompleteCmd();
        }
    }while(SRB_STATUS_PENDING == my_srbext->SrbStatus);

    if (SRB_STATUS_SUCCESS == my_srbext->SrbStatus)
    {
        UpdateMaxTxSize();
        status = STATUS_SUCCESS;
    }
    else
        status = STATUS_UNSUCCESSFUL;
END:
    return status;
}
// 概要: IdentifyNamespace の処理を行います。
NTSTATUS CNvmeDevice::IdentifyNamespace(PSPCNVME_SRBEXT srbext, ULONG nsid, PNVME_IDENTIFY_NAMESPACE_DATA data)
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> srbext_ptr;
    PSPCNVME_SRBEXT my_srbext = srbext;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (NULL == my_srbext)
    {
        srbext_ptr.Reset(new SPCNVME_SRBEXT());
        srbext_ptr->Init(this, NULL);
        my_srbext = srbext_ptr.Get();
    }

    BuildCmd_IdentSpecifiedNS(my_srbext, data, nsid);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        goto END;

    do
    {
        StorPortStallExecution(StallDelay);
    } while (SRB_STATUS_PENDING == my_srbext->SrbStatus);

    if (SRB_STATUS_SUCCESS == my_srbext->SrbStatus)
        status = STATUS_SUCCESS;
    else
        status = STATUS_UNSUCCESSFUL;
END:
    return status;
}
// 概要: IdentifyActiveNamespaceIdList の処理を行います。
NTSTATUS CNvmeDevice::IdentifyActiveNamespaceIdList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, ULONG& ret_count)
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    if (NULL == nsid_list)
        return STATUS_INVALID_PARAMETER;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> srbext_ptr;
    PSPCNVME_SRBEXT my_srbext = srbext;
    NTSTATUS status = STATUS_SUCCESS;
    if (NULL == my_srbext)
    {
        srbext_ptr.Reset(new SPCNVME_SRBEXT());
        srbext_ptr->Init(this, NULL);
        my_srbext = srbext_ptr.Get();
    }

    BuildCmd_IdentActiveNsidList(my_srbext, nsid_list, PAGE_SIZE);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        return status;

    do
    {
        StorPortStallExecution(StallDelay);
    } while (SRB_STATUS_PENDING == my_srbext->SrbStatus);

    if (my_srbext->SrbStatus != SRB_STATUS_SUCCESS)
        return STATUS_UNSUCCESSFUL;

    ret_count = 0;
    ULONG *idlist = (ULONG*)nsid_list;
    for(ULONG i=0; i < NVME_CONST::MAX_NS_COUNT; i++)
    {
        if(0 == idlist[i])
            break;
        ret_count ++;
    }

    return STATUS_SUCCESS;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetNumberOfIoQueue(USHORT count)
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> my_srbext(new SPCNVME_SRBEXT());
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    my_srbext->Init(this, NULL);

    BuildCmd_SetIoQueueCount(my_srbext, count);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);

    if (!NT_SUCCESS(status))
        goto END;

    do
    {
        StorPortStallExecution(StallDelay);
    } while (SRB_STATUS_PENDING == my_srbext->SrbStatus);

    if (SRB_STATUS_SUCCESS == my_srbext->SrbStatus)
    {
        status = STATUS_SUCCESS;
        DesiredIoQ = (MAXUSHORT & (my_srbext->NvmeCpl.DW0 + 1));
    }
    else
        status = STATUS_UNSUCCESSFUL;
END:

    return status;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetInterruptCoalescing()
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetAsyncEvent() still not implemented yet!!\n");
    return STATUS_SUCCESS;
#if 0
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> my_srbext(new SPCNVME_SRBEXT());
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    my_srbext->Init(this, NULL);

    BuildCmd_InterruptCoalescing(my_srbext, CoalescingThreshold, CoalescingTime);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);

    if (!NT_SUCCESS(status))
        return status;

    do
    {
        StorPortStallExecution(StallDelay);
    } while (SRB_STATUS_PENDING == my_srbext->SrbStatus);

    if (SRB_STATUS_SUCCESS == my_srbext->SrbStatus)
        status = STATUS_SUCCESS;
    else
        status = STATUS_UNSUCCESSFUL;

    return status;
#endif
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetAsyncEvent()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetAsyncEvent() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetArbitration()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetArbitration() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetSyncHostTime()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetSyncHostTime() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetPowerManagement()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetPowerManagement() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
// 概要: 内部状態やデバイス情報を取得します。
NTSTATUS CNvmeDevice::GetLbaFormat(ULONG nsid, NVME_LBA_FORMAT& format)
{
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    UCHAR lba_index = NsData[nsid-1].FLBAS.LbaFormatIndex;
    RtlCopyMemory(&format, &NsData[nsid - 1].LBAF[lba_index], sizeof(NVME_LBA_FORMAT));
    return STATUS_SUCCESS;
}
// 概要: 内部状態やデバイス情報を取得します。
NTSTATUS CNvmeDevice::GetNamespaceBlockSize(ULONG nsid, ULONG& size)
{
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    UCHAR lba_index = NsData[nsid - 1].FLBAS.LbaFormatIndex;
    size = (1 << NsData[nsid - 1].LBAF[lba_index].LBADS);
    return STATUS_SUCCESS;
}
// 概要: 内部状態やデバイス情報を取得します。
NTSTATUS CNvmeDevice::GetNamespaceTotalBlocks(ULONG nsid, ULONG64& blocks)
{
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    blocks = NsData[nsid - 1].NSZE;
    return STATUS_SUCCESS;
}
// 概要: 構築済みコマンドを NVMe キューへ投入します。
NTSTATUS CNvmeDevice::SubmitAdmCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd)
{
    if(!IsWorking() || NULL == AdmQueue)
        return STATUS_DEVICE_NOT_READY;
    return AdmQueue->SubmitCmd(srbext, cmd);
}
// 概要: 構築済みコマンドを NVMe キューへ投入します。
NTSTATUS CNvmeDevice::SubmitIoCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd)
{
    if (!IsWorking() || NULL == IoQueue)
        return STATUS_DEVICE_NOT_READY;

    ULONG cpu_idx = KeGetCurrentProcessorNumberEx(NULL);
    srbext->IoQueueIndex = (cpu_idx % RegisteredIoQ);
    return IoQueue[srbext->IoQueueIndex]->SubmitCmd(srbext, cmd);
}
// 概要: 内部状態や未完了要求を初期状態へ戻します。
void CNvmeDevice::ResetOutstandingCmds()
{
    if (!IsWorking() || NULL == IoQueue || NULL == AdmQueue)
        return;

    State = NVME_STATE::RESETBUS;
    AdmQueue->ResetAllCmd();
    for(ULONG i=0; i<RegisteredIoQ;i++)
        IoQueue[i]->ResetAllCmd();

    State = NVME_STATE::RUNNING;
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeDevice::SetPerfOpts()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    PERF_CONFIGURATION_DATA set_perf = { 0 };
    PERF_CONFIGURATION_DATA supported = { 0 };
    ULONG stor_status = STOR_STATUS_SUCCESS;
    supported.Version = STOR_PERF_VERSION_5;
    supported.Size = sizeof(PERF_CONFIGURATION_DATA);
    stor_status = StorPortInitializePerfOpts(this, TRUE, &supported);
    if (STOR_STATUS_SUCCESS != stor_status)
        return FALSE;

    set_perf.Version = STOR_PERF_VERSION_5;
    set_perf.Size = sizeof(PERF_CONFIGURATION_DATA);

    if(0 != (supported.Flags & STOR_PERF_CONCURRENT_CHANNELS))
    {
        set_perf.Flags |= STOR_PERF_CONCURRENT_CHANNELS;
        set_perf.ConcurrentChannels = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    }
    if (0 != (supported.Flags & STOR_PERF_NO_SGL))
        set_perf.Flags |= STOR_PERF_NO_SGL;
    if (0 != (supported.Flags & STOR_PERF_DPC_REDIRECTION_CURRENT_CPU))
    {
        set_perf.Flags |= STOR_PERF_DPC_REDIRECTION_CURRENT_CPU;
    }

    if (0 != (supported.Flags & STOR_PERF_DPC_REDIRECTION))
        set_perf.Flags |= STOR_PERF_DPC_REDIRECTION;

    stor_status = StorPortInitializePerfOpts(this, FALSE, &set_perf);
    if(STOR_STATUS_SUCCESS != stor_status)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}
// 概要: 指定条件を満たすかどうかを判定します。
bool CNvmeDevice::IsInValidIoRange(ULONG nsid, ULONG64 offset, ULONG len)
{
    if (0 == nsid || 0 == NamespaceCount)
        return false;

    ULONG64 total_blocks = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    status = GetNamespaceTotalBlocks(nsid, total_blocks);
    if(!NT_SUCCESS(status))
        return false;
    ULONG64 max_block_id = total_blocks - 1;

    if((offset + len - 1) > max_block_id)
        return false;
    return true;
}
// 概要: 作成済みキューを NVMe コントローラへ登録します。
NTSTATUS CNvmeDevice::RegisterIoQueues(PSPCNVME_SRBEXT srbext)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> temp(new SPCNVME_SRBEXT());

    if (!IsWorking())
    { 
        status = STATUS_INVALID_DEVICE_STATE;
        goto END;
    }

    for (ULONG i = 0; i < AllocatedIoQ; i++)
    {
        temp->Init(this, NULL);
        BuildCmd_RegIoCplQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if(!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            StorPortStallExecution(StallDelay);
        } while (SRB_STATUS_PENDING == temp->SrbStatus);

        if (temp->SrbStatus != SRB_STATUS_SUCCESS)
            goto END;

        temp->Init(this, NULL);
        BuildCmd_RegIoSubQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if (!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            StorPortStallExecution(StallDelay);
        } while (SRB_STATUS_PENDING == temp->SrbStatus);

        if (temp->SrbStatus != SRB_STATUS_SUCCESS)
            goto END;

        RegisteredIoQ++;
    }
    status = STATUS_SUCCESS;
END:
    if (RegisteredIoQ == DesiredIoQ)
    {
        status = STATUS_SUCCESS;
        if (NULL != srbext)
            srbext->CompleteSrb(SRB_STATUS_SUCCESS);
    }
    else
    {
        if (NULL != srbext)
            srbext->CompleteSrb(SRB_STATUS_ERROR);
    }
    return status;
}
// 概要: NVMe コントローラからキュー登録を解除します。
NTSTATUS CNvmeDevice::UnregisterIoQueues(PSPCNVME_SRBEXT srbext)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> temp(new SPCNVME_SRBEXT());
    if (!IsWorking())
    {
        status = STATUS_INVALID_DEVICE_STATE;
        goto END;
    }
    if (temp.IsNull())
    {
        status = STATUS_MEMORY_NOT_ALLOCATED;
        goto END;
    }

    for (ULONG i = 0; i < DesiredIoQ; i++)
    {
        temp->Init(this, NULL);
        BuildCmd_UnRegIoSubQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if (!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            StorPortStallExecution(StallDelay);
        } while (SRB_STATUS_PENDING == temp->SrbStatus);

        if (temp->SrbStatus != SRB_STATUS_SUCCESS)
            goto END;

        temp->Init(this, NULL);
        BuildCmd_UnRegIoCplQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if (!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            StorPortStallExecution(StallDelay);
        } while (SRB_STATUS_PENDING == temp->SrbStatus);

        if (temp->SrbStatus != SRB_STATUS_SUCCESS)
            goto END;

        RegisteredIoQ++;
    }

END:
    if (RegisteredIoQ == DesiredIoQ)
        status = STATUS_SUCCESS;

    if (NULL != srbext)
    {
        if (NT_SUCCESS(status))
            srbext->CompleteSrb(SRB_STATUS_SUCCESS);
        else
            srbext->CompleteSrb(SRB_STATUS_ERROR);
    }
    return status;
}

// 概要: NVMe キューまたは関連リソースを作成します。
NTSTATUS CNvmeDevice::CreateAdmQ()
{
    if(NULL != AdmQueue)
        return STATUS_ALREADY_INITIALIZED;

    QUEUE_PAIR_CONFIG cfg = {0};
    cfg.Depth = (USHORT)AdmDepth;
    cfg.QID = 0;
    cfg.DevExt = this;
    cfg.NumaNode = MM_ANY_NODE_OK;
    cfg.Type = QUEUE_TYPE::ADM_QUEUE;
    cfg.HistoryDepth = NVME_CONST::MAX_IO_PER_LU;
    GetAdmQueueDbl(cfg.SubDbl , cfg.CplDbl);
    AdmQueue = new CNvmeQueue(&cfg);
    if(!AdmQueue->IsInitOK())
        return STATUS_MEMORY_NOT_ALLOCATED;
    return STATUS_SUCCESS;
}
// 概要: 作成済みキューを NVMe コントローラへ登録します。
NTSTATUS CNvmeDevice::RegisterAdmQ()
{
    if(IsControllerReady() || NULL == AdmQueue)
    {
        NVME_CONTROLLER_STATUS csts = { 0 };
        ReadNvmeRegister(csts, true);
        KeBugCheckEx(BUGCHECK_INVALID_STATE, (ULONG_PTR) this, (ULONG_PTR) csts.AsUlong, 0, 0);
    }

    PHYSICAL_ADDRESS subq = { 0 };
    PHYSICAL_ADDRESS cplq = { 0 };
    NVME_ADMIN_QUEUE_ATTRIBUTES aqa = { 0 };
    NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS asq = { 0 };
    NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS acq = { 0 };
    AdmQueue->GetQueueAddr(&subq, &cplq);

    if(0 == subq.QuadPart || NULL == cplq.QuadPart)
        return STATUS_MEMORY_NOT_ALLOCATED;
    aqa.ASQS = AdmDepth - 1;
    aqa.ACQS = AdmDepth - 1;
    asq.AsUlonglong = (ULONGLONG)subq.QuadPart;
    acq.AsUlonglong = (ULONGLONG)cplq.QuadPart;
    WriteNvmeRegister(aqa, asq, acq);
    return STATUS_SUCCESS;
}
// 概要: NVMe コントローラからキュー登録を解除します。
NTSTATUS CNvmeDevice::UnregisterAdmQ()
{
    if (IsControllerReady())
    {
        NVME_CONTROLLER_STATUS csts = { 0 };
        ReadNvmeRegister(csts, true);
        KeBugCheckEx(BUGCHECK_INVALID_STATE, (ULONG_PTR)this, (ULONG_PTR)csts.AsUlong, 0, 0);
    }

    NVME_ADMIN_QUEUE_ATTRIBUTES aqa = { 0 };
    NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS asq = { 0 };
    NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS acq = { 0 };
    WriteNvmeRegister(aqa, asq, acq);
    return STATUS_SUCCESS;
}
// 概要: NVMe キューまたは関連リソースを削除します。
NTSTATUS CNvmeDevice::DeleteAdmQ()
{
    if(NULL == AdmQueue)
        return STATUS_MEMORY_NOT_ALLOCATED;

    AdmQueue->Teardown();
    delete AdmQueue;
    AdmQueue = NULL;
    return STATUS_SUCCESS;
}
// 概要: レジスタまたはデバイス情報を読み取ります。
void CNvmeDevice::ReadCtrlCap()
{
    
    ReadNvmeRegister(NvmeVer);
    ReadNvmeRegister(CtrlCap);
    DeviceTimeout = ((UCHAR)CtrlCap.TO) * (500 * 1000);
    MinPageSize = (ULONG)(1 << (12 + CtrlCap.MPSMIN));
    MaxPageSize = (ULONG)(1 << (12 + CtrlCap.MPSMAX));
    MaxTxSize = (ULONG)((1 << this->CtrlIdent.MDTS) * MinPageSize);
    if(0 == MaxTxSize)
        MaxTxSize = NVME_CONST::DEFAULT_MAX_TXSIZE;
    MaxTxPages = (ULONG)(MaxTxSize / PAGE_SIZE);

    AdmDepth = min((USHORT)CtrlCap.MQES + 1, AdmDepth);
    IoDepth = min((USHORT)CtrlCap.MQES + 1, IoDepth);
}
// 概要: PCI リソースを仮想アドレスへマップします。
bool CNvmeDevice::MapCtrlRegisters()
{
    BOOLEAN in_iospace = FALSE;
    STOR_PHYSICAL_ADDRESS bar0 = { 0 };
    INTERFACE_TYPE type = PortCfg->AdapterInterfaceType;
    bar0.LowPart = (PciCfg.u.type0.BaseAddresses[0] & 0xFFFFC000);
    bar0.HighPart = PciCfg.u.type0.BaseAddresses[1];

    for (ULONG i = 0; i < AccessRangeCount; i++)
    {
        PACCESS_RANGE range = &AccessRanges[i];
        if(true == IsAddrEqual(range->RangeStart, bar0))
        {
            in_iospace = !range->RangeInMemory;
            PUCHAR addr = (PUCHAR)StorPortGetDeviceBase(
                                    this, type, 
                                    PortCfg->SystemIoBusNumber, bar0, 
                                    range->RangeLength, in_iospace);
            if (NULL != addr)
            {
                CtrlReg = (PNVME_CONTROLLER_REGISTERS)addr;
                Bar0Size = range->RangeLength;
                Doorbells = CtrlReg->Doorbells;
                return true;
            }
        }
    }

    return false;
}
// 概要: 内部状態やデバイス情報を取得します。
bool CNvmeDevice::GetPciBusData(INTERFACE_TYPE type, ULONG bus, ULONG slot)
{
    ULONG size = sizeof(PciCfg);
    ULONG status = StorPortGetBusData(this, type, bus, slot, &PciCfg, size);

    if (2 == status || status != size)
        return false;

    VendorID = PciCfg.VendorID;
    DeviceID = PciCfg.DeviceID;
    return true;
}
// 概要: コントローラ状態が期待値になるまで待機します。
bool CNvmeDevice::WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy)
{
    ULONG elapsed = 0;
    BOOLEAN is_ready = IsControllerReady();

    while(is_ready != csts_rdy)
    {
        StorPortStallExecution(StallDelay);
        elapsed += StallDelay;
        if(elapsed > time_us)
            return false;

        is_ready = IsControllerReady();
    }

    return true;
}
// 概要: コントローラ状態が期待値になるまで待機します。
bool CNvmeDevice::WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy, BOOLEAN cc_en)
{
    ULONG elapsed = 0;
    BOOLEAN is_enable = IsControllerEnabled();
    BOOLEAN is_ready = IsControllerReady();

    while (is_ready != csts_rdy || is_enable != cc_en)
    {
        StorPortStallExecution(StallDelay);
        elapsed += StallDelay;
        if (elapsed > time_us)
            return false;

        is_enable = IsControllerEnabled();
        is_ready = IsControllerReady();
    }

    return true;
}
// 概要: コントローラ状態が期待値になるまで待機します。
bool CNvmeDevice::WaitForCtrlerShst(ULONG time_us)
{
    ULONG elapsed = 0;
    BOOLEAN shn_done = FALSE;

    while (!shn_done)
    {
        NVME_CONTROLLER_STATUS csts = { 0 };
        ReadNvmeRegister(csts);
        if(csts.SHST == NVME_CSTS_SHST_SHUTDOWN_COMPLETED)
            shn_done = TRUE;
        else
        {
            StorPortStallExecution(StallDelay);
            elapsed += StallDelay;
            if (elapsed > time_us)
                return false;
        }
    }

    return true;
}
// 概要: 内部変数やデバイス状態を初期化します。
void CNvmeDevice::InitVars()
{
    CtrlReg = NULL;
    PortCfg = NULL;
    Doorbells = NULL;

    VendorID = 0;
    DeviceID = 0;
    State = NVME_STATE::STOP;
    CpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    TotalNumaNodes = 0;
    RegisteredIoQ = 0;
    AllocatedIoQ = 0;
    DesiredIoQ = NVME_CONST::IO_QUEUE_COUNT;
    DeviceTimeout = 2000 * NVME_CONST::STALL_TIME_US;
    StallDelay = NVME_CONST::STALL_TIME_US;
    AccessRangeCount = 0;
    NamespaceCount = 0;
    CoalescingThreshold = DEFAULT_INT_COALESCE_COUNT;
    CoalescingTime = DEFAULT_INT_COALESCE_TIME;

    ReadCacheEnabled = WriteCacheEnabled = false;

    RtlZeroMemory(AccessRanges, sizeof(ACCESS_RANGE)* ACCESS_RANGE_COUNT);

    MaxNamespaces = NVME_CONST::SUPPORT_NAMESPACES;
    AdmDepth = NVME_CONST::ADMIN_QUEUE_DEPTH;
    IoDepth = NVME_CONST::IO_QUEUE_DEPTH;
    AdmQueue = NULL;
    RtlZeroMemory(IoQueue, sizeof(CNvmeQueue*) * MAX_IO_QUEUE_COUNT);
    RtlZeroMemory(&PciCfg, sizeof(PCI_COMMON_CONFIG));
    RtlZeroMemory(&NvmeVer, sizeof(NVME_VERSION));
    RtlZeroMemory(&CtrlCap, sizeof(NVME_CONTROLLER_CAPABILITIES));
    RtlZeroMemory(&CtrlIdent, sizeof(NVME_IDENTIFY_CONTROLLER_DATA));
    RtlZeroMemory(MsgGroupAffinity, sizeof(MsgGroupAffinity));
    for(ULONG i=0; i< NVME_CONST::MAX_INT_COUNT; i++)
    {
        MsgGroupAffinity[i].Mask = MAXULONG_PTR;
    }

    for(ULONG i=0; i< NVME_CONST::SUPPORT_NAMESPACES; i++)
    {
        memset(NsData + i, 0xFF, sizeof(NVME_IDENTIFY_NAMESPACE_DATA));
    }

    StorPortInitializeDpc(this, &this->RestartDpc, CNvmeDevice::RestartAdapterDpc);
}
// 概要: レジストリからドライバ設定を読み込みます。
void CNvmeDevice::LoadRegistry()
{
    ULONG size = sizeof(ULONG);
    ULONG ret_size = 0;
    BOOLEAN ok = FALSE;
    UCHAR* buffer = StorPortAllocateRegistryBuffer(this, &size);

    if (buffer == NULL)
        return;

    RtlZeroMemory(buffer, size);
    ret_size = size;    
    ok = StorPortRegistryRead(this, REGNAME_COALESCE_COUNT, TRUE, 
        REG_DWORD, (PUCHAR) buffer, &ret_size);
    if(ok)
        CoalescingThreshold = (UCHAR) *((ULONG*)buffer);

    RtlZeroMemory(buffer, size);
    ret_size = size;
    ok = StorPortRegistryRead(this, REGNAME_COALESCE_TIME, TRUE,
        REG_DWORD, (PUCHAR)buffer, &ret_size);
    if (ok)
        CoalescingTime = (UCHAR) *((ULONG*)buffer);
    
    RtlZeroMemory(buffer, size);
    ret_size = size;
    ok = StorPortRegistryRead(this, REGNAME_ADMQ_DEPTH, TRUE,
        REG_DWORD, (PUCHAR)buffer, &ret_size);
    if (ok)
        AdmDepth = (USHORT) *((ULONG*)buffer);

    RtlZeroMemory(buffer, size);
    ret_size = size;
    ok = StorPortRegistryRead(this, REGNAME_IOQ_DEPTH, TRUE,
        REG_DWORD, (PUCHAR)buffer, &ret_size);
    if (ok)
        IoDepth = (USHORT) *((ULONG*)buffer);

    RtlZeroMemory(buffer, size);
    ret_size = size;
    ok = StorPortRegistryRead(this, REGNAME_IOQ_COUNT, TRUE,
        REG_DWORD, (PUCHAR)buffer, &ret_size);
    if (ok)
        DesiredIoQ = *((ULONG*)buffer);

    StorPortFreeRegistryBuffer(this, buffer);
}
// 概要: NVMe キューまたは関連リソースを作成します。
NTSTATUS CNvmeDevice::CreateIoQ()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    QUEUE_PAIR_CONFIG cfg = { 0 };
    cfg.DevExt = this;
    cfg.Depth = IoDepth;
    cfg.NumaNode = 0;
    cfg.Type = QUEUE_TYPE::IO_QUEUE;
    cfg.HistoryDepth = NVME_CONST::MAX_IO_PER_LU;

    for(USHORT i=0; i<DesiredIoQ; i++)
    {
        if(NULL != IoQueue[i])
            continue;
        CNvmeQueue* queue = new CNvmeQueue();
        cfg.QID = i + 1;
        this->GetQueueDbl(cfg.QID, cfg.SubDbl, cfg.CplDbl);
        status = queue->Setup(&cfg);
        if(!NT_SUCCESS(status))
        {
            delete queue;
            return status;
        }
        IoQueue[i] = queue;
        AllocatedIoQ++;
    }

    return STATUS_SUCCESS;
}
// 概要: NVMe キューまたは関連リソースを削除します。
NTSTATUS CNvmeDevice::DeleteIoQ()
{
    for(CNvmeQueue* &queue : IoQueue)
    {
        if(NULL == queue)
            continue;

        queue->Teardown();
        delete queue;
        AllocatedIoQ--;
    }

    RtlZeroMemory(IoQueue, sizeof(CNvmeQueue*) * MAX_IO_QUEUE_COUNT);
    return STATUS_SUCCESS;
}

#pragma endregion


// NVMe Submission Queue / Completion Queue の管理実装です。
// Command ID の割り当て、Doorbell 更新、Completion Entry の回収、SRB 完了通知を担います。
// 概要: CalcQueueBufferSize の処理を行います。
static __inline size_t CalcQueueBufferSize(USHORT depth)
{
    size_t page_count = BYTES_TO_PAGES(depth * sizeof(NVME_COMMAND)) +
                    BYTES_TO_PAGES(depth * sizeof(NVME_COMPLETION_ENTRY));
    return page_count * PAGE_SIZE;
}

// 概要: レジスタまたはデバイス情報を読み取ります。
static __inline ULONG ReadDbl(PVOID devext, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL dbl)
{
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    return StorPortReadRegisterUlong(devext, &dbl->AsUlong);
}
// 概要: レジスタまたはデバイス情報を読み取ります。
static __inline ULONG ReadDbl(PVOID devext, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL dbl)
{
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    return StorPortReadRegisterUlong(devext, &dbl->AsUlong);
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
static __inline void WriteDbl(PVOID devext, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL dbl, ULONG value)
{
#if !defined(DBG)
    UNREFERENCED_PARAMETER(devext);
#endif
    MemoryBarrier();
    StorPortWriteRegisterUlong(devext, &dbl->AsUlong, value);
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
static __inline void WriteDbl(PVOID devext, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL dbl, ULONG value)
{
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    StorPortWriteRegisterUlong(devext, &dbl->AsUlong, value);
}
// 概要: 指定条件を満たすかどうかを判定します。
static __inline bool IsValidQid(ULONG qid)
{
    return (qid != NVME_INVALID_QID);
}
// 概要: NewCplArrived の処理を行います。
static __inline bool NewCplArrived(PNVME_COMPLETION_ENTRY entry, USHORT current_tag)
{
    if (entry->DW3.Status.P == current_tag)
        return true;
    return false;
}
// 概要: デバイス能力から内部キャッシュ値を更新します。
static __inline void UpdateCplHead(ULONG &cpl_head, USHORT depth)
{
    cpl_head = (cpl_head + 1) % depth;
}
// 概要: デバイス能力から内部キャッシュ値を更新します。
static __inline void UpdateCplHeadAndPhase(ULONG& cpl_head, USHORT& phase, USHORT depth)
{
    UpdateCplHead(cpl_head, depth);
    if (0 == cpl_head)
        phase = !phase;
}
#pragma region ======== class CNvmeQueue ========

// 概要: QueueCplDpcRoutine の処理を行います。
VOID CNvmeQueue::QueueCplDpcRoutine(
    _In_ PSTOR_DPC dpc,
    _In_ PVOID devext,
    _In_opt_ PVOID sysarg1,
    _In_opt_ PVOID sysarg2
)
{
    UNREFERENCED_PARAMETER(devext);
    UNREFERENCED_PARAMETER(sysarg1);
    UNREFERENCED_PARAMETER(sysarg2);
    CNvmeQueue* queue = CONTAINING_RECORD(dpc, CNvmeQueue, QueueCplDpc);
    queue->CompleteCmd();
}

// 概要: オブジェクトの初期化と内部状態の準備を行います。
CNvmeQueue::CNvmeQueue()
{
    KeInitializeSpinLock(&SubLock);
}
// 概要: オブジェクトの初期化と内部状態の準備を行います。
CNvmeQueue::CNvmeQueue(QUEUE_PAIR_CONFIG* config)
    : CNvmeQueue()
{
    Setup(config);
}
// 概要: オブジェクトの終了処理と保持リソースの解放を行います。
CNvmeQueue::~CNvmeQueue()
{
    Teardown();
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CNvmeQueue::Setup(QUEUE_PAIR_CONFIG* config)
{
    bool ok = false;
    NTSTATUS status = STATUS_SUCCESS;

    DevExt = config->DevExt;
    QueueID = config->QID;
    Depth = config->Depth;
    NumaNode = config->NumaNode;
    Type = config->Type;
    Buffer = config->PreAllocBuffer;
    BufferSize = config->PreAllocBufSize;
    SubDbl = config->SubDbl;
    CplDbl = config->CplDbl;
    HistoryDepth = config->HistoryDepth;
    StorPortInitializeDpc(DevExt, &QueueCplDpc, QueueCplDpcRoutine);

    if(NULL == Buffer)
    {
        BufferSize = CalcQueueBufferSize(Depth);
        ok = AllocQueueBuffer();
        if(!ok)
        {
            status = STATUS_MEMORY_NOT_ALLOCATED;
            goto ERROR;
        }
    }
    else
    {
        UseExtBuffer = true;
    }

    ok = InitQueueBuffer();
    if(!ok)
    {
        status = STATUS_INTERNAL_ERROR;
        goto ERROR;
    }

    status = History.Setup(this, this->DevExt, this->HistoryDepth, this->NumaNode);
    if (!NT_SUCCESS(status))
    {
        goto ERROR;
    }

    IsReady = true;
    return STATUS_SUCCESS;
ERROR:
    Teardown();
    return status;
}
// 概要: デバイスまたはキューの終了処理を行います。
void CNvmeQueue::Teardown()
{
    this->IsReady = false;
    DeallocQueueBuffer();
    History.Teardown();
}
// 概要: 構築済みコマンドを NVMe キューへ投入します。
NTSTATUS CNvmeQueue::SubmitCmd(SPCNVME_SRBEXT* srbext, PNVME_COMMAND src_cmd)
{
    CSpinLock lock(&SubLock);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG cid = 0;
    if (!this->IsReady)
        return STATUS_DEVICE_NOT_READY;

    if(!IsSafeForSubmit())
        return STATUS_DEVICE_BUSY;

    cid = (ULONG)(src_cmd->CDW0.CID & 0xFFFF);
    ASSERT(cid == src_cmd->CDW0.CID);
    status = History.Push(cid, srbext);
    if (STATUS_ALREADY_COMMITTED == status)
    {
        DbgBreakPoint();
        PSPCNVME_SRBEXT old_srbext = NULL;
        History.Pop(cid, old_srbext);
        old_srbext->CompleteSrb(SRB_STATUS_BUSY);

        History.Push(cid, srbext);
    }
    else if (!NT_SUCCESS(status))
        return status;

    srbext->SubIndex = SubTail;
    RtlCopyMemory((SubQ_VA+SubTail), src_cmd, sizeof(NVME_COMMAND));
    srbext->SubmittedCmd = (SubQ_VA + SubTail);
    srbext->SubmittedQ = this;
    InterlockedIncrement(&InflightCmds);
    SubTail = (SubTail + 1) % Depth;
    WriteDbl(this->DevExt, this->SubDbl, this->SubTail);

    return STATUS_SUCCESS;
}
// 概要: 内部状態や未完了要求を初期状態へ戻します。
void CNvmeQueue::ResetAllCmd()
{
    CSpinLock lock(&SubLock);
    History.Reset();
    InflightCmds = 0;
    while(CplHead != SubTail)
    {
        UpdateCplHeadAndPhase(CplHead, PhaseTag, Depth);
    }
}
// 概要: コマンド完了後の後処理と SRB 完了通知を行います。
void CNvmeQueue::CompleteCmd(ULONG max_count)
{
    PNVME_COMPLETION_ENTRY cpl = &CplQ_VA[CplHead];
    PSPCNVME_SRBEXT srbext = NULL;
    USHORT cid = 0;
    ULONG update_count = 0;

    if(0 == max_count)
        max_count = this->Depth;

    while(NewCplArrived(cpl, this->PhaseTag))
    {
        cid = cpl->DW3.CID;
        SubHead = cpl->DW2.SQHD;
        History.Pop(cid, srbext);

        if (NULL != srbext)
        {
            RtlCopyMemory(&srbext->NvmeCpl, cpl, sizeof(NVME_COMPLETION_ENTRY));
            if (srbext->CompletionCB)
                srbext->CompletionCB(srbext);

            srbext->CompleteSrb(srbext->NvmeCpl.DW3.Status);
            srbext->CleanUp();
            InterlockedDecrement(&InflightCmds);
        }
        else
            DbgBreakPoint();

        UpdateCplHeadAndPhase(CplHead, PhaseTag, Depth);
        update_count++;
        cpl = &CplQ_VA[CplHead];
        if(max_count <= update_count)
            break;
    }

    if(update_count != 0)
        WriteDbl(DevExt, CplDbl, CplHead);
}
// 概要: 内部状態やデバイス情報を取得します。
void CNvmeQueue::GetQueueAddr(PVOID* subq, PVOID* cplq)
{  
    if(subq != NULL)
        *subq = SubQ_VA;

    if (cplq != NULL)
        *cplq = CplQ_VA;
}
// 概要: 内部状態やデバイス情報を取得します。
void CNvmeQueue::GetQueueAddr(PVOID* subva, PHYSICAL_ADDRESS* subpa, PVOID* cplva, PHYSICAL_ADDRESS* cplpa)
{
    GetQueueAddr(subva, cplva);
    GetQueueAddr(subpa, cplpa);
}
// 概要: 内部状態やデバイス情報を取得します。
void CNvmeQueue::GetQueueAddr(PHYSICAL_ADDRESS* subq, PHYSICAL_ADDRESS* cplq)
{
    GetSubQAddr(subq);
    GetCplQAddr(cplq);
}
// 概要: 内部状態やデバイス情報を取得します。
void CNvmeQueue::GetSubQAddr(PHYSICAL_ADDRESS* subq)
{
    subq->QuadPart = SubQ_PA.QuadPart;
}
// 概要: 内部状態やデバイス情報を取得します。
void CNvmeQueue::GetCplQAddr(PHYSICAL_ADDRESS* cplq)
{
    cplq->QuadPart = CplQ_PA.QuadPart;
}
// 概要: 指定条件を満たすかどうかを判定します。
bool CNvmeQueue::IsSafeForSubmit()
{
    return ((Depth - NVME_CONST::SAFE_SUBMIT_THRESHOLD) > (USHORT)InflightCmds)? true : false;
}
// 概要: レジスタまたはデバイス情報を読み取ります。
ULONG CNvmeQueue::ReadSubTail()
{
    if (IsValidQid(QueueID) && NULL != SubDbl)
        return ReadDbl(DevExt, SubDbl);
    KdBreakPoint();
    return NVME_CONST::INVALID_DBL_VALUE;
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
void CNvmeQueue::WriteSubTail(ULONG value)
{
    if (IsValidQid(QueueID) && NULL != SubDbl)
        return WriteDbl(DevExt, SubDbl, value);
    KdBreakPoint();
}
// 概要: レジスタまたはデバイス情報を読み取ります。
ULONG CNvmeQueue::ReadCplHead()
{
    if (IsValidQid(QueueID) && NULL != CplDbl)
        return ReadDbl(DevExt, CplDbl);
    KdBreakPoint();
    return NVME_CONST::INVALID_DBL_VALUE;
}
// 概要: レジスタまたはデバイス状態へ値を書き込みます。
void CNvmeQueue::WriteCplHead(ULONG value)
{
    if (IsValidQid(QueueID) && NULL != CplDbl)
        return WriteDbl(DevExt, CplDbl, value);
    KdBreakPoint();
}

// 概要: キュー処理に必要なメモリを確保します。
bool CNvmeQueue::AllocQueueBuffer()
{
    PHYSICAL_ADDRESS low = { 0 };
    PHYSICAL_ADDRESS high = { 0 };
    PHYSICAL_ADDRESS align = { 0 };
    
    low.HighPart = 0X000000001;
    high.QuadPart = (LONGLONG)-1;

    ULONG status = StorPortAllocateContiguousMemorySpecifyCacheNode(
        this->DevExt, this->BufferSize,
        low, high, align,
        CNvmeQueue::CacheType, this->NumaNode,
        &this->Buffer);

    if(STOR_STATUS_SUCCESS != status)
    {
        KdBreakPoint();
        return false;
    }

    BufferPA = MmGetPhysicalAddress(Buffer);

    return true;
}
// 概要: 内部変数やデバイス状態を初期化します。
bool CNvmeQueue::InitQueueBuffer()
{
    this->SubQ_Size = this->Depth * sizeof(NVME_COMMAND);
    this->CplQ_Size = this->Depth * sizeof(NVME_COMPLETION_ENTRY);

    PUCHAR cursor = (PUCHAR) this->Buffer;
    this->SubQ_VA = (PNVME_COMMAND)ROUND_TO_PAGES(cursor);
    cursor += this->SubQ_Size;
    this->CplQ_VA = (PNVME_COMPLETION_ENTRY)ROUND_TO_PAGES(cursor);

    if((cursor + this->CplQ_Size) > ((PUCHAR)this->Buffer + this->BufferSize))
        goto ERROR;

    RtlZeroMemory(this->Buffer, this->BufferSize);
    this->SubQ_PA = MmGetPhysicalAddress(this->SubQ_VA);
    this->CplQ_PA = MmGetPhysicalAddress(this->CplQ_VA);
    return true;

ERROR:
    this->SubQ_Size = 0;
    this->SubQ_VA = NULL;
    this->CplQ_Size = 0;
    this->CplQ_VA = NULL;
    return false;
}
// 概要: キュー処理で確保したメモリを解放します。
void CNvmeQueue::DeallocQueueBuffer()
{
    if(UseExtBuffer)
        return;

    if(NULL != this->Buffer)
    { 
        StorPortFreeContiguousMemorySpecifyCache(
                DevExt, this->Buffer, this->BufferSize, CNvmeQueue::CacheType);
    }

    this->Buffer = NULL;
    this->BufferSize = 0;
}
#pragma endregion

#pragma region ======== class CCmdHistory ========
// 概要: オブジェクトの初期化と内部状態の準備を行います。
CCmdHistory::CCmdHistory()
{
}
// 概要: オブジェクトの初期化と内部状態の準備を行います。
CCmdHistory::CCmdHistory(class CNvmeQueue* parent, PVOID devext, USHORT depth, ULONG numa_node)
        : CCmdHistory()
{   Setup(parent, devext, depth, numa_node);    }
// 概要: オブジェクトの終了処理と保持リソースの解放を行います。
CCmdHistory::~CCmdHistory()
{
    Teardown();
}
// 概要: デバイスまたは内部設定を更新します。
NTSTATUS CCmdHistory::Setup(class CNvmeQueue* parent, PVOID devext, USHORT depth, ULONG numa_node)
{
    this->Parent = parent;
    this->NumaNode = numa_node;
    this->Depth = depth;
    this->DevExt = devext;
    this->BufferSize = depth * sizeof(PSPCNVME_SRBEXT);
    this->QueueID = parent->QueueID;

    ULONG status = StorPortAllocatePool(devext, (ULONG)this->BufferSize, this->BufferTag, &this->Buffer);
    if(STOR_STATUS_SUCCESS != status)
        return STATUS_MEMORY_NOT_ALLOCATED;

    RtlZeroMemory(this->Buffer, this->BufferSize);
    this->History = (PSPCNVME_SRBEXT *)this->Buffer;
    return STATUS_SUCCESS;
}
// 概要: デバイスまたはキューの終了処理を行います。
void CCmdHistory::Teardown()
{

    if(NULL != this->Buffer)
        StorPortFreePool(this->DevExt, this->Buffer);

    this->Buffer = NULL;
}
// 概要: 内部状態や未完了要求を初期状態へ戻します。
void CCmdHistory::Reset()
{
    if(NULL == this->History)
        return;

    for(ULONG i=0; i<Depth; i++)
    {
        if(History[i] != NULL)
        {
            History[i]->CompleteSrb(SRB_STATUS_BUS_RESET);
            History[i] = NULL;
        }
    }
}
// 概要: コマンド履歴へ SRB 拡張を登録します。
NTSTATUS CCmdHistory::Push(ULONG index, PSPCNVME_SRBEXT srbext)
{
    if(index >= Depth)
        return STATUS_UNSUCCESSFUL;

    PVOID old_ptr = InterlockedCompareExchangePointer(
                        (volatile PVOID*)&History[index], srbext, NULL);

    if(NULL != old_ptr)
        return STATUS_ALREADY_COMMITTED;

    return STATUS_SUCCESS;
}
// 概要: コマンド履歴から SRB 拡張を取り出します。
NTSTATUS CCmdHistory::Pop(ULONG index, PSPCNVME_SRBEXT& srbext)
{
    if (index >= Depth)
    {
        DbgBreakPoint();
        return STATUS_UNSUCCESSFUL;
    }

    srbext = (PSPCNVME_SRBEXT)InterlockedExchangePointer((volatile PVOID*)&History[index], NULL);
    if(NULL == srbext)
    {
        DbgBreakPoint();
        return STATUS_INVALID_DEVICE_STATE;
    }
    return STATUS_SUCCESS;
}
#pragma endregion

// HwStartIo から呼ばれる要求本体のディスパッチャです。
// BuildIo で検証済みの SRB を SCSI CDB、IOCTL、既定エラー処理へ振り分けます。
// 概要: StartIo_DefaultHandler の処理を行います。
UCHAR StartIo_DefaultHandler(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}
// 概要: StartIo_ScsiHandler の処理を行います。
UCHAR StartIo_ScsiHandler(PSPCNVME_SRBEXT srbext)
{
    UCHAR opcode = srbext->Cdb()->CDB6GENERIC.OperationCode;
    UCHAR srb_status = SRB_STATUS_ERROR;
    DebugScsiOpCode(opcode);

    switch(opcode)
    {
#if 0
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_REZERO_UNIT:
    case SCSIOP_REQUEST_BLOCK_ADDR:
    case SCSIOP_FORMAT_UNIT:
    case SCSIOP_READ_BLOCK_LIMITS:
    case SCSIOP_REASSIGN_BLOCKS:
    case SCSIOP_SEEK6:
    case SCSIOP_SEEK_BLOCK:
    case SCSIOP_PARTITION:
    case SCSIOP_READ_REVERSE:
    case SCSIOP_WRITE_FILEMARKS:
    case SCSIOP_SPACE:
    case SCSIOP_RECOVER_BUF_DATA:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RELEASE_UNIT:
    case SCSIOP_COPY:
    case SCSIOP_ERASE:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_RECEIVE_DIAGNOSTIC:
    case SCSIOP_SEND_DIAGNOSTIC:
    case SCSIOP_MEDIUM_REMOVAL:

    case SCSIOP_REQUEST_SENSE:
        srb_status = Scsi_RequestSense6(srbext);
        break;
#endif
    case SCSIOP_MODE_SELECT:
        srb_status = Scsi_ModeSelect6(srbext);
        break;
    case SCSIOP_READ6:
        srb_status = Scsi_Read6(srbext);
        break;
    case SCSIOP_WRITE6:
        srb_status = Scsi_Write6(srbext);
        break;
    case SCSIOP_INQUIRY:
        srb_status = Scsi_Inquiry6(srbext);
        break;
    case SCSIOP_MODE_SENSE:
        srb_status = Scsi_ModeSense6(srbext);
        break;
    case SCSIOP_VERIFY6:
        srb_status = Scsi_Verify6(srbext);
        break;

#if 0
    case SCSIOP_READ_FORMATTED_CAPACITY:
    case SCSIOP_SEEK:
    case SCSIOP_WRITE_VERIFY:
    case SCSIOP_SEARCH_DATA_HIGH:
    case SCSIOP_SEARCH_DATA_EQUAL:
    case SCSIOP_SEARCH_DATA_LOW:
    case SCSIOP_SET_LIMITS:
    case SCSIOP_READ_POSITION:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_COMPARE:
    case SCSIOP_COPY_COMPARE:
    case SCSIOP_WRITE_DATA_BUFF:
    case SCSIOP_READ_DATA_BUFF:
    case SCSIOP_WRITE_LONG:
    case SCSIOP_CHANGE_DEFINITION:
    case SCSIOP_WRITE_SAME:
    case SCSIOP_READ_SUB_CHANNEL:
    case SCSIOP_READ_TOC:
    case SCSIOP_READ_HEADER:
    case SCSIOP_PLAY_AUDIO:
    case SCSIOP_GET_CONFIGURATION:
    case SCSIOP_PLAY_AUDIO_MSF:
    case SCSIOP_PLAY_TRACK_INDEX:
    case SCSIOP_PLAY_TRACK_RELATIVE:
    case SCSIOP_GET_EVENT_STATUS:
    case SCSIOP_PAUSE_RESUME:
    case SCSIOP_LOG_SELECT:
    case SCSIOP_LOG_SENSE:
    case SCSIOP_STOP_PLAY_SCAN:
    case SCSIOP_XDWRITE:
    case SCSIOP_XPWRITE:
    case SCSIOP_READ_TRACK_INFORMATION:
    case SCSIOP_XDWRITE_READ:
    case SCSIOP_SEND_OPC_INFORMATION:
    case SCSIOP_RESERVE_UNIT10:
    case SCSIOP_RELEASE_UNIT10:
    case SCSIOP_REPAIR_TRACK:
    case SCSIOP_CLOSE_TRACK_SESSION:
    case SCSIOP_READ_BUFFER_CAPACITY:
    case SCSIOP_SEND_CUE_SHEET:
    case SCSIOP_PERSISTENT_RESERVE_IN:
    case SCSIOP_PERSISTENT_RESERVE_OUT:
#endif
    case SCSIOP_MODE_SELECT10:
        srb_status = Scsi_ModeSelect10(srbext);
        break;
    case SCSIOP_READ_CAPACITY:
        srb_status = Scsi_ReadCapacity10(srbext);
        break;
    case SCSIOP_READ:
        srb_status = Scsi_Read10(srbext);
        break;
    case SCSIOP_WRITE:
        srb_status = Scsi_Write10(srbext);
        break;
    case SCSIOP_VERIFY:
        srb_status = Scsi_Verify10(srbext);
        break;
    case SCSIOP_MODE_SENSE10:
        srb_status = Scsi_ModeSense10(srbext);
        break;

#if 0
    case SCSIOP_BLANK:
    case SCSIOP_SEND_EVENT:
    case SCSIOP_SEND_KEY:
    case SCSIOP_REPORT_KEY:
    case SCSIOP_MOVE_MEDIUM:
    case SCSIOP_LOAD_UNLOAD_SLOT:
    case SCSIOP_SET_READ_AHEAD:
    case SCSIOP_SERVICE_ACTION_OUT12:
    case SCSIOP_SEND_MESSAGE:
    case SCSIOP_GET_PERFORMANCE:
    case SCSIOP_READ_DVD_STRUCTURE:
    case SCSIOP_WRITE_VERIFY12:
    case SCSIOP_SEARCH_DATA_HIGH12:
    case SCSIOP_SEARCH_DATA_EQUAL12:
    case SCSIOP_SEARCH_DATA_LOW12:
    case SCSIOP_SET_LIMITS12:
    case SCSIOP_READ_ELEMENT_STATUS_ATTACHED:
    case SCSIOP_REQUEST_VOL_ELEMENT:
    case SCSIOP_SEND_VOLUME_TAG:
    case SCSIOP_READ_DEFECT_DATA:
    case SCSIOP_READ_ELEMENT_STATUS:
    case SCSIOP_READ_CD_MSF:
    case SCSIOP_SCAN_CD:
    case SCSIOP_SET_CD_SPEED:
    case SCSIOP_PLAY_CD:
    case SCSIOP_MECHANISM_STATUS:
    case SCSIOP_READ_CD:
    case SCSIOP_SEND_DVD_STRUCTURE:
    case SCSIOP_INIT_ELEMENT_RANGE:
#endif
    case SCSIOP_REPORT_LUNS:
        srb_status = Scsi_ReportLuns12(srbext);
        break;
    case SCSIOP_READ12:
        srb_status = Scsi_Read12(srbext);
        break;
    case SCSIOP_WRITE12:
        srb_status = Scsi_Write12(srbext);
        break;
    case SCSIOP_VERIFY12:
        srb_status = Scsi_Verify12(srbext);
        break;
    case SCSIOP_SECURITY_PROTOCOL_IN:
    case SCSIOP_SECURITY_PROTOCOL_OUT:
		srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
#if 0
    case SCSIOP_XDWRITE_EXTENDED16:
    case SCSIOP_REBUILD16:
    case SCSIOP_REGENERATE16:
    case SCSIOP_EXTENDED_COPY:
    case SCSIOP_RECEIVE_COPY_RESULTS:
    case SCSIOP_ATA_PASSTHROUGH16:
    case SCSIOP_ACCESS_CONTROL_IN:
    case SCSIOP_ACCESS_CONTROL_OUT:
    case SCSIOP_COMPARE_AND_WRITE:
    case SCSIOP_READ_ATTRIBUTES:
    case SCSIOP_WRITE_ATTRIBUTES:
    case SCSIOP_WRITE_VERIFY16:
    case SCSIOP_PREFETCH16:
    case SCSIOP_SYNCHRONIZE_CACHE16:
    case SCSIOP_LOCK_UNLOCK_CACHE16:
    case SCSIOP_WRITE_SAME16:
    case SCSIOP_ZBC_OUT:
    case SCSIOP_ZBC_IN:
    case SCSIOP_READ_DATA_BUFF16:
    case SCSIOP_SERVICE_ACTION_OUT16:
#endif

    case SCSIOP_READ16:
        srb_status = Scsi_Read16(srbext);
        break;
    case SCSIOP_WRITE16:
        srb_status = Scsi_Write16(srbext);
        break;
    case SCSIOP_VERIFY16:
        srb_status = Scsi_Verify16(srbext);
        break;
    case SCSIOP_READ_CAPACITY16:
        srb_status = Scsi_ReadCapacity16(srbext);
        break;

#if 0
    case SCSIOP_OPERATION32:
        break;
#endif
    default:
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    return srb_status;
}
// 概要: StartIo_IoctlHandler の処理を行います。
UCHAR StartIo_IoctlHandler(PSPCNVME_SRBEXT srbext)
{

    UCHAR srb_status = SRB_STATUS_INVALID_REQUEST;
    PSRB_IO_CONTROL ioctl = (PSRB_IO_CONTROL) srbext->DataBuf();
    size_t count = strlen(IOCTL_MINIPORT_SIGNATURE_SCSIDISK);

    switch (ioctl->ControlCode)
    {
        case IOCTL_SCSI_MINIPORT_FIRMWARE:
            if (0 == strncmp((const char*)ioctl->Signature, IOCTL_MINIPORT_SIGNATURE_FIRMWARE, count))
                srb_status = IoctlScsiMiniport_Firmware(srbext, ioctl);
            break;
        default:
            srb_status = SRB_STATUS_INVALID_REQUEST;
            break;
    }

    return srb_status;
}

// HwBuildIo から呼ばれる軽量ハンドラ群です。
// DISPATCH_LEVEL でも呼ばれ得るため、時間のかかる処理は避け、PnP/電源イベントや
// StartIo 前の検証を中心に行います。
// 概要: アダプタ PnP 要求に対する応答を作成します。
static UCHAR AdapterPnp_QueryCapHandler(PSPCNVME_SRBEXT srbext)
{
    PSTOR_DEVICE_CAPABILITIES_EX cap = (PSTOR_DEVICE_CAPABILITIES_EX)srbext->DataBuf();
    cap->Version = STOR_DEVICE_CAPABILITIES_EX_VERSION_1;
    cap->Size = sizeof(STOR_DEVICE_CAPABILITIES_EX);
    cap->DeviceD1 = 0;
    cap->DeviceD2 = 0;
    cap->LockSupported = 0;
    cap->EjectSupported = 0;
    cap->Removable = 1;
    cap->DockDevice = 0;
    cap->UniqueID = 0;
    cap->SilentInstall = 1;
    cap->SurpriseRemovalOK = 1;
    cap->NoDisplayInUI = 0;
    cap->Address = 0;
    cap->UINumber = 0xFFFFFFFF;
    return SRB_STATUS_SUCCESS;
}

// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_DefaultHandler(PSPCNVME_SRBEXT srbext)
{
	srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
    return FALSE;
}

// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_IoctlHandler(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return TRUE;
}

// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_ScsiHandler(PSPCNVME_SRBEXT srbext)
{
    DebugScsiOpCode(srbext->Cdb()->CDB6GENERIC.OperationCode);
    
    UCHAR path = 0, target = 0, lun = 0;
    SrbGetPathTargetLun(srbext->Srb, &path, &target, &lun);

    if(!(0==path && 0==target && 0==lun))
    {
        srbext->CompleteSrb(SRB_STATUS_INVALID_LUN);
        return FALSE;
    }
    return TRUE;
}

// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_SrbPowerHandler(PSPCNVME_SRBEXT srbext)
{
	srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
    return FALSE;
}

// 概要: 要求処理に必要なデータを構築します。
BOOLEAN BuildIo_SrbPnpHandler(PSPCNVME_SRBEXT srbext)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG flags = 0;
    UCHAR srb_status = SRB_STATUS_ERROR;

    STOR_PNP_ACTION action = STOR_PNP_ACTION::StorStartDevice;
    PSRBEX_DATA_PNP srb_pnp = srbext->SrbDataPnp();

    ASSERT(NULL != srb_pnp);
    flags = srb_pnp->SrbPnPFlags;
    action = srb_pnp->PnPAction;

    if(SRB_PNP_FLAGS_ADAPTER_REQUEST != (flags & SRB_PNP_FLAGS_ADAPTER_REQUEST))
    {
        goto END;
    }

    switch(action)
    {
        case StorQueryCapabilities:
            srb_status = AdapterPnp_QueryCapHandler(srbext);
            break;
        case StorRemoveDevice:
            status = srbext->DevExt->ShutdownController();
            if (!NT_SUCCESS(status))
            {
                KdBreakPoint();
            }
            srbext->DevExt->Teardown();
            srb_status = SRB_STATUS_SUCCESS;
            break;
        case StorSurpriseRemoval:
            srbext->DevExt->Teardown();
            srb_status = SRB_STATUS_SUCCESS;
            break;
    }

END:
    srbext->CompleteSrb(srb_status);
    return FALSE;
}
