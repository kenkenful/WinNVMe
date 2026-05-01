#include "winnvme.h"

// ===== Begin WdmUtils.cpp =====

void* __cdecl operator new(size_t size)
{
    return ExAllocatePoolWithTag(PagedPool, size, CPP_TAG);
}

void* operator new(size_t size, POOL_TYPE type, ULONG tag)
{
    return ExAllocatePoolWithTag(type, size, tag);
}

void* __cdecl operator new[](size_t size)
{
    return ExAllocatePoolWithTag(PagedPool, size, CPP_TAG);
}

void* operator new[](size_t size, POOL_TYPE type, ULONG tag)
{
    return ExAllocatePoolWithTag(type, size, tag);
}

void __cdecl operator delete(void* ptr, size_t size)
{
    UNREFERENCED_PARAMETER(size);
    ExFreePool(ptr);
}

void __cdecl operator delete[](void* ptr)
{
    ExFreePool(ptr);
}

void __cdecl operator delete[](void* ptr, size_t size)
{
    UNREFERENCED_PARAMETER(size);
    ExFreePool(ptr);
}

static __inline void DebugCallIn(const char* func_name, const char* prefix)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s [%s] IN =>\n", prefix, func_name);
}

static __inline void DebugCallOut(const char* func_name, const char* prefix)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s [%s] OUT <=\n", prefix, func_name);
}

CDebugCallInOut::CDebugCallInOut(const char* name)
    : CDebugCallInOut((char*)name)
{
}

CDebugCallInOut::CDebugCallInOut(char* name)
{
    this->Name = (char*)new(NonPagedPoolNx, CALLINOUT_TAG) char[this->BufSize];
    if (NULL != this->Name)
    {
        RtlStringCbCopyNA(this->Name, this->BufSize, name, this->BufSize - 1);
        DebugCallIn(this->Name, DEBUG_PREFIX);
    }
}

CDebugCallInOut::~CDebugCallInOut()
{
    if (NULL != this->Name)
    {
        DebugCallOut(this->Name, DEBUG_PREFIX);
        delete[] Name;
    }
}

CSpinLock::CSpinLock(KSPIN_LOCK* lock, bool acquire)
{
    this->Lock = lock;
    this->IsAcquired = false;
    if (acquire)
        DoAcquire();
}

CSpinLock::~CSpinLock()
{
    DoRelease();
    this->Lock = NULL;
    this->IsAcquired = false;
}

void CSpinLock::DoAcquire()
{
    if (!IsAcquired)
    {
        KeAcquireSpinLock(this->Lock, &this->OldIrql);
        IsAcquired = true;
    }
}

void CSpinLock::DoRelease()
{
    if (IsAcquired)
    {
        KeReleaseSpinLock(this->Lock, this->OldIrql);
        IsAcquired = false;
    }
}

CStorSpinLock::CStorSpinLock(PVOID devext, STOR_SPINLOCK reason, PVOID ctx)
{
    this->DevExt = devext;
    Acquire(reason, ctx);
}

CStorSpinLock::~CStorSpinLock()
{
    Release();
}

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

void DebugSrbFunctionCode(ULONG code)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s Got SRB cmd, code (0x%08X)\n", DEBUG_PREFIX, code);
}

void DebugScsiOpCode(UCHAR opcode)
{
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBG_FILTER, "%s Got SCSI Cmd(0x%02X)\n", DEBUG_PREFIX, opcode);
}

// ===== End WdmUtils.cpp =====

// ===== Begin IoctlScsiMiniport_Handlers.cpp =====

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

    //refer to https://docs.microsoft.com/en-us/windows-hardware/drivers/storage/upgrading-firmware-for-an-nvme-device
    //the input buffer layout should be [SRB_IO_CONTROL][FIRMWARE_REQUEST_BLOCK]
    //output buffer layout should be [SRB_IO_CONTROL][FIRMWARE_REQUEST_BLOCK][RET BUFFER]
    //all of them are located in srb data buffer.
    switch (request->Function)
    {
    case FIRMWARE_FUNCTION_GET_INFO:
        srb_status = Firmware_GetInfo(srbext);
        break;
    case FIRMWARE_FUNCTION_DOWNLOAD:
        //srb_status = Firmware_DownloadToAdapter(srbext, ioctl, request);
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    case FIRMWARE_FUNCTION_ACTIVATE:
        //srb_status = Firmware_ActivateSlot(srbext, ioctl, request);
        srb_status = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    //In Firmware commands, SRBSTATUS only indicates 
    //"is this command sent to device successfully?"
    //The result of command stored in ioctl->ReturnCode.
    return srb_status;
}
// ===== End IoctlScsiMiniport_Handlers.cpp =====

// ===== Begin HandleAdapterControl.cpp =====

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

SCSI_ADAPTER_CONTROL_STATUS Handle_RestartAdapter(CNvmeDevice* devext)
{
    devext->RestartController();
    return ScsiAdapterControlSuccess;
}
// ===== End HandleAdapterControl.cpp =====

// ===== Begin Ioctl_FirmwareFunctions.cpp =====
SPC_SRBEXT_COMPLETION Complete_FirmwareInfo;

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
    //max size of payload in single piece of download image cmd...
    //refer to https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/upgrading-firmware-for-an-nvme-device
    ret_info->ImagePayloadMaxSize = min(nvme->MaxTxSize, (128 * PAGE_SIZE));

    for (UCHAR i = 0; i < ctrl->FRMW.SlotCount; i++)
    {
        PSTORAGE_FIRMWARE_SLOT_INFO_V2 slot = &ret_info->Slot[i];
        slot->ReadOnly = FALSE;
        slot->SlotNumber = i + 1;   //slot id is 1-based.
        RtlZeroMemory(slot->Revision, STORAGE_FIRMWARE_SLOT_INFO_V2_REVISION_LENGTH);
        RtlCopyMemory(slot->Revision, &logpage->FRS[i], sizeof(ULONGLONG));
    }

    if (ctrl->FRMW.Slot1ReadOnly)
        ret_info->Slot[0].ReadOnly = TRUE;
}

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

    //Caller should provide V1 structure with enough slot space
    if (buf_len < (sizeof(STORAGE_FIRMWARE_INFO) + (sizeof(STORAGE_FIRMWARE_SLOT_INFO) * total_slots)))
    {
        fw_status = FIRMWARE_STATUS_OUTPUT_BUFFER_TOO_SMALL;
        srb_status = SRB_STATUS_INVALID_PARAMETER;
        goto END;
    }
    //translate NVME status code to firmware status code
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
UCHAR Firmware_DownloadToAdapter(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request)
{
    UNREFERENCED_PARAMETER(srbext);
    UNREFERENCED_PARAMETER(ioctl);
    UNREFERENCED_PARAMETER(request);
    return SRB_STATUS_INVALID_REQUEST;
}
UCHAR Firmware_ActivateSlot(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request)
{
    UNREFERENCED_PARAMETER(srbext);
    UNREFERENCED_PARAMETER(ioctl);
    UNREFERENCED_PARAMETER(request);
    return SRB_STATUS_INVALID_REQUEST;
}
// ===== End Ioctl_FirmwareFunctions.cpp =====

// ===== Begin NvmePrpBuilder.cpp =====

static inline size_t GetDistanceToNextPage(PUCHAR ptr)
{
    return (((PUCHAR)PAGE_ALIGN(ptr) + PAGE_SIZE) - ptr);
}

static inline void BuildPrp1(ULONG64 &prp1, PVOID ptr)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(ptr);
    prp1 = pa.QuadPart;
}
static inline void BuildPrp2(ULONG64& prp2, PVOID ptr)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(ptr);
    prp2 = pa.QuadPart;
}

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

bool BuildPrp(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd, PVOID buffer, size_t buf_size)
{
    //refer to NVMe 1.3 chapter 4.3
    //Physical Region Page Entry and List
    //The PBAO of PRP entry should align to DWORD.
    PUCHAR cursor = (PUCHAR) buffer;
    size_t size_left = buf_size;
    size_t distance = 0;

    BuildPrp1(cmd->PRP1, cursor);
    distance = GetDistanceToNextPage(cursor);

    //this buffer is smaller than PAGE_SIZE and not cross page boundary. 
    //Using PRP1 is enough...
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
        //PRP2 need list
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
// ===== End NvmePrpBuilder.cpp =====

// ===== Begin Scsi_Utils.cpp =====
SPC_SRBEXT_COMPLETION Complete_ScsiReadWrite;

void Complete_ScsiReadWrite(SPCNVME_SRBEXT *srbext)
{
    srbext->CleanUp();
    //srbext->CompleteSrb(srbext->NvmeCpl.DW3.Status);
}

UCHAR Scsi_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG len, bool is_write)
{
    //the SCSI I/O are based for BLOCKs of device, not bytes....
    UCHAR srb_status = SRB_STATUS_PENDING;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG nsid = srbext->Lun() + 1;

    if (!srbext->DevExt->IsInValidIoRange(nsid, offset, len))
        return SRB_STATUS_ERROR;

    BuiildCmd_ReadWrite(srbext, offset, len, is_write);
    //srbext->CompletionCB = NULL;
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
// ===== End Scsi_Utils.cpp =====

// ===== Begin SrbExt.cpp =====

_SPCNVME_SRBEXT* _SPCNVME_SRBEXT::InitSrbExt(PVOID devext, PSTORAGE_REQUEST_BLOCK srb)
{
	PSPCNVME_SRBEXT srbext = (PSPCNVME_SRBEXT)SrbGetMiniportContext(srb);
	srbext->Init(devext, srb);
	return srbext;
}
_SPCNVME_SRBEXT* _SPCNVME_SRBEXT::GetSrbExt(PSTORAGE_REQUEST_BLOCK srb)
{
    PSPCNVME_SRBEXT ret = (PSPCNVME_SRBEXT)SrbGetMiniportContext(srb);
    ASSERT(ret->Srb != NULL);
    return ret;
}

void _SPCNVME_SRBEXT::Init(PVOID devext, STORAGE_REQUEST_BLOCK* srb)
{
    RtlZeroMemory(this, sizeof(_SPCNVME_SRBEXT));

    DevExt = (CNvmeDevice*)devext;
    Srb = srb;
    SrbStatus = SRB_STATUS_PENDING;
    InitOK = TRUE;
    Tag = ScsiQTag();
}

void _SPCNVME_SRBEXT::CleanUp()
{
    ResetExtBuf(NULL);
    if(NULL != Prp2VA)
    { 
        ExFreePoolWithTag(Prp2VA, TAG_PRP2);
        Prp2VA = NULL;
    }
}
void _SPCNVME_SRBEXT::CompleteSrb(NVME_COMMAND_STATUS &nvme_status)
{
    UCHAR status = NvmeToSrbStatus(nvme_status);
    CompleteSrb(status);
}
void _SPCNVME_SRBEXT::CompleteSrb(UCHAR status)
{
    this->SrbStatus = status;
    if (NULL != Srb)
    {
        SetScsiSenseBySrbStatus(Srb, status);
        SrbSetSrbStatus(Srb, status);
        StorPortNotification(RequestComplete, DevExt, Srb);
    }
}
ULONG _SPCNVME_SRBEXT::FuncCode()
{
    if(NULL == Srb)
        return SRB_FUNCTION_SPC_INTERNAL;
    return SrbGetSrbFunction(Srb);
}
ULONG _SPCNVME_SRBEXT::ScsiQTag()
{
    if (NULL == Srb)
        return 0;
    return SrbGetQueueTag(Srb);
}
PCDB _SPCNVME_SRBEXT::Cdb()
{
    if (NULL == Srb)
        return NULL;
    return SrbGetCdb(Srb);
}
UCHAR _SPCNVME_SRBEXT::CdbLen() {
    if (NULL == Srb)
        return 0;
    return SrbGetCdbLength(Srb);
}
UCHAR _SPCNVME_SRBEXT::PathID() {
    if (NULL == Srb)
        return INVALID_PATH_ID;
    return SrbGetPathId(Srb);
}
UCHAR _SPCNVME_SRBEXT::TargetID() {
    if (NULL == Srb)
        return INVALID_TARGET_ID;
    return SrbGetTargetId(Srb);
}
UCHAR _SPCNVME_SRBEXT::Lun() {
    if (NULL == Srb)
        return INVALID_LUN_ID;
    return SrbGetLun(Srb);
}
PVOID _SPCNVME_SRBEXT::DataBuf() {
    if (NULL == Srb)
        return NULL;
    return SrbGetDataBuffer(Srb);
}
ULONG _SPCNVME_SRBEXT::DataBufLen() {
    if (NULL == Srb)
        return 0;
    return SrbGetDataTransferLength(Srb);
}

void _SPCNVME_SRBEXT::SetTransferLength(ULONG length)
{
    if(NULL != Srb)
        SrbSetDataTransferLength(Srb, length);
}

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
// ===== End SrbExt.cpp =====

// ===== Begin NvmeCmdBuilder.cpp =====
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

//to build NVME_COMMAND for IdentifyController command
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
void BuildCmd_IdentActiveNsidList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, size_t list_size)
{
//nsid_list is a ULONG array buffer to retrieve all nsid which is active in this NVMe.
//list_size is size IN BYTES of nsid_list.
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_ACTIVE_NAMESPACES;
    BuildPrp(srbext, cmd, nsid_list, list_size);
}
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
void BuildCmd_IdentAllNSList(PSPCNVME_SRBEXT srbext, PVOID ns_buf, size_t buf_size)
{
    //ns_buf is a array to retrieve all NameSpace list.
    //buf_size is SIZE IN BYTES of ns_buf.
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_IDENTIFY;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.IDENTIFY.CDW10.CNS = NVME_IDENTIFY_CNS_ALLOCATED_NAMESPACE_LIST;

    BuildPrp(srbext, cmd, (PVOID)ns_buf, buf_size);
}
void BuildCmd_SetIoQueueCount(PSPCNVME_SRBEXT srbext, USHORT count)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_NUMBER_OF_QUEUES;

    //NSQ and NCQ should be 0 based.
    cmd->u.SETFEATURES.CDW11.NumberOfQueues.NSQ =
        cmd->u.SETFEATURES.CDW11.NumberOfQueues.NCQ = count - 1;
}
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
    cmd->u.CREATEIOSQ.CDW10.QSIZE = queue->Depth - 1; //0-based value
    cmd->u.CREATEIOSQ.CDW11.CQID = queue->QueueID;;
    cmd->u.CREATEIOSQ.CDW11.PC = TRUE;
    cmd->u.CREATEIOSQ.CDW11.QPRIO = NVME_NVM_QUEUE_PRIORITY_HIGH;
}
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
    cmd->u.CREATEIOCQ.CDW10.QSIZE = queue->Depth - 1; //0-based value
    cmd->u.CREATEIOCQ.CDW11.IEN = TRUE;
    cmd->u.CREATEIOCQ.CDW11.IV = (queue->Type == QUEUE_TYPE::ADM_QUEUE) ? 0 : queue->QueueID;
    cmd->u.CREATEIOCQ.CDW11.PC = TRUE;
}
void BuildCmd_UnRegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_DELETE_IO_SQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOSQ.CDW10.QID = queue->QueueID;
}
void BuildCmd_UnRegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_DELETE_IO_CQ;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.CREATEIOSQ.CDW10.QID = queue->QueueID;
}
void BuildCmd_InterruptCoalescing(PSPCNVME_SRBEXT srbext, UCHAR threshold, UCHAR interval)
{
//threshold : how many interrupt collected then fire interrupt once?
//interval : how much time to waiting collect coalesced interrupt?
//=> if (merged interrupt >= threshold || waiting merge time >= interval), then device fire INTERRUPT once
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;
    cmd->u.SETFEATURES.CDW10.FID = NVME_FEATURE_INTERRUPT_COALESCING;
    cmd->u.SETFEATURES.CDW11.InterruptCoalescing.THR = threshold;
    cmd->u.SETFEATURES.CDW11.InterruptCoalescing.TIME = interval;
}
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
void BuildCmd_SyncHostTime(PSPCNVME_SRBEXT srbext, LARGE_INTEGER *timestamp)
{
    UNREFERENCED_PARAMETER(timestamp);
    //KeQuerySystemTime() get system tick(100 ns) count since 1601/1/1 00:00:00
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

    //PVOID timestamp = NULL;
    //ULONG size = PAGE_SIZE;
    //ULONG status = StorPortAllocatePool(devext, size, TAG_GENBUF, &timestamp);
    //if (STOR_STATUS_SUCCESS != status)
    //    return FALSE;
    //RtlZeroMemory(timestamp, size);
    //RtlCopyMemory(timestamp, &elapsed, sizeof(LARGE_INTEGER));

    //cmd.PRP1 = StorPortGetPhysicalAddress(devext, NULL, timestamp, &size).QuadPart;

    ////implement wait
    //return STATUS_INTERNAL_ERROR;
    ////submit command
    //bool ok = devext->AdminQueue->SubmitCmd(&cmd, NULL, CMD_CTX_TYPE::WAIT_EVENT);
}

void BuildCmd_GetFirmwareSlotsInfo(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_GET_LOG_PAGE;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = NVME_CONST::UNSPECIFIC_NSID;

    //In this command, we need "count of DWORD" not "size in bytes".
    ULONG dword_count = (ULONG)(sizeof(NVME_FIRMWARE_SLOT_INFO_LOG) >> 2);
    cmd->u.GETLOGPAGE.CDW10_V13.NUMDL = (USHORT) (dword_count & 0x0000FFFF);
    cmd->u.GETLOGPAGE.CDW10_V13.LID = NVME_LOG_PAGE_FIRMWARE_SLOT_INFO;
    cmd->u.GETLOGPAGE.CDW11.NUMDU = (USHORT) (dword_count >> 16);

    BuildPrp(srbext, cmd, info, sizeof(NVME_FIRMWARE_SLOT_INFO_LOG));
}

//NVMe v1.0 and v1.3 has different cmd structure in this command.
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

void BuildCmd_AdminSecuritySend(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SECURITY_SEND;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;

    //In this command, we need "count of DWORD" not "size in bytes".
    cmd->u.SECURITYSEND.CDW10.SECP = cdb->SECURITY_PROTOCOL_OUT.SecurityProtocol;
    USHORT spsp = 0;
    REVERSE_BYTES_2(&spsp, cdb->SECURITY_PROTOCOL_OUT.SecurityProtocolSpecific);
    cmd->u.SECURITYSEND.CDW10.SPSP = spsp;
    ULONG payload_size = 0;
    REVERSE_BYTES_4(&payload_size, cdb->SECURITY_PROTOCOL_OUT.AllocationLength);
    cmd->u.SECURITYSEND.CDW11.TL = payload_size;

    BuildPrp(srbext, cmd, srbext->DataBuf(), srbext->DataBufLen());
}
void BuildCmd_AdminSecurityRecv(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb)
{
    PNVME_COMMAND cmd = &srbext->NvmeCmd;
    RtlZeroMemory(cmd, sizeof(NVME_COMMAND));
    cmd->CDW0.OPC = NVME_ADMIN_COMMAND_SECURITY_RECEIVE;
    cmd->CDW0.CID = srbext->ScsiQTag();
    cmd->NSID = nsid;

    //In this command, we need "count of DWORD" not "size in bytes".
    cmd->u.SECURITYSEND.CDW10.SECP = cdb->SECURITY_PROTOCOL_IN.SecurityProtocol;
    USHORT spsp = 0;
    REVERSE_BYTES_2(&spsp, cdb->SECURITY_PROTOCOL_IN.SecurityProtocolSpecific);
    cmd->u.SECURITYSEND.CDW10.SPSP = spsp;
    ULONG payload_size = 0;
    REVERSE_BYTES_4(&payload_size, cdb->SECURITY_PROTOCOL_IN.AllocationLength);
    cmd->u.SECURITYSEND.CDW11.TL = payload_size;

    BuildPrp(srbext, cmd, srbext->DataBuf(), srbext->DataBufLen());
}
// ===== End NvmeCmdBuilder.cpp =====

// ===== Begin ScsiHandler_CDB10.cpp =====

UCHAR Scsi_Read10(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB10, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}
UCHAR Scsi_Write10(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB10, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

UCHAR Scsi_ReadCapacity10(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    ULONG ret_size = 0;
    PREAD_CAPACITY_DATA cap = (PREAD_CAPACITY_DATA)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;
    UCHAR lun = srbext->Lun();

    //LUN is zero based...
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
    
    //LogicalBlockAddress is MAX LBA index, it's zero-based id.
    //**this field is (total LBA count)-1.
    srbext->DevExt->GetNamespaceTotalBlocks(lun + 1, blocks);
    srbext->DevExt->GetNamespaceBlockSize(lun + 1, block_size);
    if (blocks > MAXULONG32)
    {
        srb_status = SRB_STATUS_INVALID_REQUEST;
        ret_size = 0;
        goto END;
    }
    //NO support thin-provisioning in current stage....
    //*From SBC - 3 r27:
    //    *If the RETURNED LOGICAL BLOCK ADDRESS field is set to FFFF_FFFFh,
    //    * then the application client should issue a READ CAPACITY(16)
    //    * command(see 5.16) to request that the device server transfer the
    //    * READ CAPACITY(16) parameter data to the data - in buffer.
    blocks -= 1;

    cap->LogicalBlockAddress = cap->BytesPerBlock = 0;
    REVERSE_BYTES_4(&cap->BytesPerBlock, &block_size);
    REVERSE_BYTES_4(&cap->LogicalBlockAddress, &blocks); //only reverse lower 4 bytes

    ret_size = sizeof(READ_CAPACITY_DATA);
    srb_status = SRB_STATUS_SUCCESS;

END:
    srbext->SetTransferLength(ret_size);
    return srb_status;
}
UCHAR Scsi_Verify10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

    ////todo: complete this handler for FULL support of verify
    //UCHAR srb_status = SRB_STATUS_ERROR;
    //CRamdisk* disk = srbext->DevExt->RamDisk;
    //PCDB cdb = srbext->Cdb;
    //UINT32 lba_start = 0;    //in Blocks, not bytes
    //REVERSE_BYTES_4(&lba_start, &cdb->CDB10.LogicalBlockByte0);
    //
    //UINT16 verify_len = 0;    //in Blocks, not bytes
    //REVERSE_BYTES_2(&verify_len, &cdb->CDB10.TransferBlocksMsb);

    //if(FALSE == disk->IsExceedLbaRange(lba_start, verify_len))
    //    srb_status = SRB_STATUS_SUCCESS;

    //SrbSetDataTransferLength(srbext->Srb, 0);
    //return srb_status;
}
UCHAR Scsi_ModeSelect10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

    //UCHAR srb_status = SRB_STATUS_INVALID_REQUEST;
    //UNREFERENCED_PARAMETER(srbext);
    //return srb_status;
}
UCHAR Scsi_ModeSense10(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

//    UCHAR srb_status = SRB_STATUS_ERROR;
//    PCDB cdb = srbext->Cdb;
//    PUCHAR buffer = (PUCHAR)srbext->DataBuffer;
//    PMODE_PARAMETER_HEADER10 header = (PMODE_PARAMETER_HEADER10)buffer;
//    ULONG buf_size = srbext->DataBufLen;
//    ULONG ret_size = 0;
//    ULONG page_size = 0;
//    ULONG mode_data_size = 0;
//
//    if (NULL == buffer || 0 == buf_size)
//        return SRB_STATUS_ERROR;
//
//    if (buf_size < sizeof(MODE_PARAMETER_HEADER10))
//    {
//        srb_status = SRB_STATUS_DATA_OVERRUN;
//        ret_size = sizeof(MODE_PARAMETER_HEADER10);
//        goto end;
//    }
//
//    FillParamHeader10(header);
//    buffer += sizeof(MODE_PARAMETER_HEADER10);
//    buf_size -= sizeof(MODE_PARAMETER_HEADER10);
//    ret_size += sizeof(MODE_PARAMETER_HEADER10);
//    REVERSE_BYTES_2(&mode_data_size, header->ModeDataLength);
//
//    // Todo: reply real mode sense data
//    switch (cdb->MODE_SENSE.PageCode)
//    {
//        case MODE_PAGE_CACHING:
//        {
//            ReplyModePageCaching(buffer, buf_size, ret_size);
//            mode_data_size += page_size;
//            srb_status = SRB_STATUS_SUCCESS;
//            break;
//        }
//        case MODE_PAGE_CONTROL:
//        {
//            ReplyModePageControl(buffer, buf_size, ret_size);
//            mode_data_size += page_size;
//            srb_status = SRB_STATUS_SUCCESS;
//            break;
//        }
//        case MODE_PAGE_FAULT_REPORTING:
//        {
//            //in HLK, it required "Information Exception Control Page".
//            //But it is renamed to MODE_PAGE_FAULT_REPORTING in Windows Storport ....
//            //refet to https://www.t10.org/ftp/t10/document.94/94-190r3.pdf
//            ReplyModePageInfoExceptionCtrl(buffer, buf_size, ret_size);
//            mode_data_size += page_size;
//            srb_status = SRB_STATUS_SUCCESS;
//            break;
//        }
//        case MODE_SENSE_RETURN_ALL:
//        {
//            if (buf_size > 0)
//            {
//                ReplyModePageCaching(buffer, buf_size, ret_size);
//                mode_data_size += page_size;
//            }
//            if (buf_size > 0)
//            {
//                ReplyModePageControl(buffer, buf_size, ret_size);
//                mode_data_size += page_size;
//            }
//            if (buf_size > 0)
//            {
//                ReplyModePageInfoExceptionCtrl(buffer, buf_size, ret_size);
//                mode_data_size += page_size;
//            }
//
//            srb_status = SRB_STATUS_SUCCESS;
//            break;
//        }
//        default:
//        {
//            page_size = 0;
//            srb_status = SRB_STATUS_SUCCESS;
//            break;
//        }
//    }
//    REVERSE_BYTES_2(header->ModeDataLength, &mode_data_size);
//
//end:
//    SrbSetDataTransferLength(srbext->Srb, ret_size);
//    return srb_status;
}

// ===== End ScsiHandler_CDB10.cpp =====

// ===== Begin ScsiHandler_CDB12.cpp =====

UCHAR Scsi_ReportLuns12(PSPCNVME_SRBEXT srbext)
{
//according SEAGATE SCSI reference, SCSIOP_REPORT_LUNS
//is used to query SCSI Logical Unit class address.
//each address are 8 bytes. In current windows system 
//I can't find reference data to determine LU address.
//MSDN also said it's NOT recommend to translate this SCSI 
//command for NVMe device.
//So I skip this command....
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}

UCHAR Scsi_Read12(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB12, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}

UCHAR Scsi_Write12(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB12, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

UCHAR Scsi_Verify12(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

    ////todo: complete this handler for FULL support of verify
    //UCHAR srb_status = SRB_STATUS_ERROR;
    //CRamdisk* disk = srbext->DevExt->RamDisk;
    //PCDB cdb = srbext->Cdb;
    //UINT32 lba_start = 0;    //in Blocks, not bytes
    //REVERSE_BYTES_4(&lba_start, cdb->CDB12.LogicalBlock);

    //UINT32 verify_len = 0;    //in Blocks, not bytes
    //REVERSE_BYTES_4(&verify_len, &cdb->CDB12.TransferLength);

    //if (FALSE == disk->IsExceedLbaRange(lba_start, verify_len))
    //    srb_status = SRB_STATUS_SUCCESS;

    //SrbSetDataTransferLength(srbext->Srb, 0);
    //return srb_status;
}

//In windows, SED(Self Encrypted Disk) features are implemented by 
//SCSIOP_SECURITY_PROTOCOL_IN and SCSIOP_SECURITY_PROTOCOL_OUT.
//SED tool (e.g. sed-utils) send SED cmd to disk via IOCTL_SCSI_PASS_THROUGH_DIRECT,
//which carried CDB data with SCSIOP_SECURITY_PROTOCOL_IN and SCSIOP_SECURITY_PROTOCOL_OUT.
//Then in disk.sys it picks CDB data and send to NVMe driver via SCSI command.

//SCSIOP_SECURITY_PROTOCOL_IN => Host retrieve security protocol data from device
UCHAR Scsi_SecurityProtocolIn(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN is_support = srbext->DevExt->CtrlIdent.OACS.SecurityCommands;
    if(!is_support)
        return SRB_STATUS_ERROR;

    //Note: In this command , payload data should be aligned to block size 
    //of namespace format. Usually it is PAGE_SIZE from app.
    BuildCmd_AdminSecurityRecv(srbext, NVME_CONST::DEFAULT_CTRLID, srbext->Cdb());
    status = srbext->DevExt->SubmitAdmCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        srb_status = SRB_STATUS_ERROR;
    else
        srb_status = SRB_STATUS_PENDING;

    return srb_status;
}
//SCSIOP_SECURITY_PROTOCOL_OUT => Host send security protocol data to device
UCHAR Scsi_SecurityProtocolOut(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN is_support = srbext->DevExt->CtrlIdent.OACS.SecurityCommands;
    if (!is_support)
        return SRB_STATUS_ERROR;

    //Note: In this command , payload data should be aligned to block size 
    //of namespace format. Usually it is PAGE_SIZE from app.
    BuildCmd_AdminSecuritySend(srbext, NVME_CONST::DEFAULT_CTRLID, srbext->Cdb());
    status = srbext->DevExt->SubmitAdmCmd(srbext, &srbext->NvmeCmd);
    if (!NT_SUCCESS(status))
        srb_status = SRB_STATUS_ERROR;
    else
        srb_status = SRB_STATUS_PENDING;

    return srb_status;
}
// ===== End ScsiHandler_CDB12.cpp =====

// ===== Begin ScsiHandler_CDB16.cpp =====
inline void FillReadCapacityEx(UCHAR lun, PSPCNVME_SRBEXT srbext)
{
    PREAD_CAPACITY_DATA_EX cap = (PREAD_CAPACITY_DATA_EX)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;
    srbext->DevExt->GetNamespaceBlockSize(lun+1, block_size);

    //LogicalBlockAddress is MAX LBA index, it's zero-based id.
    //**this field is (total LBA count)-1.
    srbext->DevExt->GetNamespaceTotalBlocks(lun+1, blocks);
    blocks -= 1;
    REVERSE_BYTES_4(&cap->BytesPerBlock, &block_size);
    REVERSE_BYTES_8(&cap->LogicalBlockAddress.QuadPart, &blocks);
}

UCHAR Scsi_Read16(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB16, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}

UCHAR Scsi_Write16(PSPCNVME_SRBEXT srbext)
{
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB16, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}

UCHAR Scsi_Verify16(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
    //todo: complete this handler for FULL support of verify
    //UCHAR srb_status = SRB_STATUS_ERROR;
    //CRamdisk* disk = srbext->DevExt->RamDisk;
    //PCDB cdb = srbext->Cdb;
    //INT64 lba_start = 0;    //in Blocks, not bytes
    //
    //REVERSE_BYTES_8(&lba_start, cdb->CDB16.LogicalBlock);

    //UINT32 verify_len = 0;    //in Blocks, not bytes
    //REVERSE_BYTES_4(&verify_len, &cdb->CDB16.TransferLength);

    //if (FALSE == disk->IsExceedLbaRange(lba_start, verify_len))
    //    srb_status = SRB_STATUS_SUCCESS;

    //SrbSetDataTransferLength(srbext->Srb, 0);
    //return srb_status;
}

UCHAR Scsi_ReadCapacity16(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_SUCCESS;
    ULONG ret_size = 0;
    UCHAR lun = srbext->Lun();
    PREAD_CAPACITY_DATA_EX cap = (PREAD_CAPACITY_DATA_EX)srbext->DataBuf();
    ULONG block_size = 0;
    ULONG64 blocks = 0;

    //LUN is zero based...
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
    //LogicalBlockAddress is MAX LBA index, it's zero-based id.
    //**this field is (total LBA count)-1.
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
// ===== End ScsiHandler_CDB16.cpp =====

// ===== Begin ScsiHandler_CDB6.cpp =====

typedef struct _CDB6_REQUESTSENSE
{
    UCHAR OperationCode;
    UCHAR DescFormat : 1;
    UCHAR Reserved1 : 7;
    UCHAR Reserved2[2];
    UCHAR AllocSize;
    struct {
        UCHAR Link : 1;         //obsoleted by T10
        UCHAR Flag : 1;         //obsoleted by T10
        UCHAR NormalACA : 1;
        UCHAR Reserved : 3;
        UCHAR VenderSpecific : 2;
    }Control;
}CDB6_REQUESTSENSE, *PCDB6_REQUESTSENSE;

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
static UCHAR Reply_VpdIdentifier(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    char *subnqn = (char*)srbext->DevExt->CtrlIdent.SUBNQN;
    ULONG nqn_size = (ULONG)strlen((char*)subnqn);
    ULONG vid_size = (ULONG)strlen((char*)NVME_CONST::VENDOR_ID);
    size_t buf_size = (ULONG)sizeof(VPD_IDENTIFICATION_PAGE) +
                        (ULONG)sizeof(VPD_IDENTIFICATION_DESCRIPTOR)
                        + nqn_size + vid_size + 1;      //1 more byte for "_"
    SPC::CAutoPtr<VPD_IDENTIFICATION_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    //NQN is too long. So only use VID + SN as Identifier.
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
static UCHAR Reply_VpdBlockLimits(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    //Max SCSI transfer block size
    //question : is it really used in modern windows system? Orz
    ULONG buf_size = sizeof(VPD_BLOCK_LIMITS_PAGE);
    SPC::CAutoPtr<VPD_BLOCK_LIMITS_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_BLOCK_LIMITS;
    REVERSE_BYTES_2(page->PageLength, &buf_size);

    ULONG max_tx = srbext->DevExt->MaxTxSize;
    //tell I/O system: max tx size and optimal tx size of this adapter.
    REVERSE_BYTES_4(page->MaximumTransferLength, &max_tx);
    REVERSE_BYTES_4(page->OptimalTransferLength, &max_tx);

    //Refer to SCSI SBC3 doc or SCSI reference Block Limits VPD page_ptr.
    //http://www.13thmonkey.org/documentation/SCSI/sbc3r25.pdf
    USHORT granularity = 4;
    REVERSE_BYTES_2(page->OptimalTransferLengthGranularity, &granularity);

    ret_size = min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);

    return SRB_STATUS_SUCCESS;
}
static UCHAR Reply_VpdBlockDeviceCharacteristics(PSPCNVME_SRBEXT srbext, ULONG& ret_size)
{
    ULONG buf_size = sizeof(VPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE);
    SPC::CAutoPtr<VPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE, PagedPool, TAG_VPDPAGE>
        page(new(PagedPool, TAG_VPDPAGE) UCHAR[buf_size]);

    page->DeviceType = DIRECT_ACCESS_DEVICE;
    page->DeviceTypeQualifier = DEVICE_CONNECTED;
    page->PageCode = VPD_BLOCK_DEVICE_CHARACTERISTICS;
    page->PageLength = (UCHAR)buf_size;
    page->MediumRotationRateLsb = 1;        //todo: what is this?
    page->NominalFormFactor = 0;

    ret_size = min(srbext->DataBufLen(), buf_size);
    RtlCopyMemory(srbext->DataBuf(), page, ret_size);

    return SRB_STATUS_SUCCESS;
}
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
static void BuildInquiryData(PINQUIRYDATA data, char* vid, char* pid, char* rev)
{
    data->DeviceType = DIRECT_ACCESS_DEVICE;
    data->DeviceTypeQualifier = DEVICE_CONNECTED;
    data->RemovableMedia = 0;
    data->Versions = 0x06;
    data->NormACA = 0;
    data->HiSupport = 0;
    data->ResponseDataFormat = 2;
    data->AdditionalLength = INQUIRYDATABUFFERSIZE - 5;  // Amount of data we are returning
    data->EnclosureServices = 0;
    data->MediumChanger = 0;
    data->CommandQueue = 1;
    data->Wide16Bit = 0;
    data->Addr16 = 0;
    data->Synchronous = 0;
    data->Reserved3[0] = 0;

    data->Wide32Bit = TRUE;
    data->LinkedCommands = FALSE;   // No Linked Commands
    RtlCopyMemory((PUCHAR)&data->VendorId[0], vid, sizeof(data->VendorId));
    RtlCopyMemory((PUCHAR)&data->ProductId[0], pid, sizeof(data->ProductId));
    RtlCopyMemory((PUCHAR)&data->ProductRevisionLevel[0], rev, sizeof(data->ProductRevisionLevel));
}

UCHAR Scsi_RequestSense6(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;

    ////request sense is used to query error information from device.
    ////device should reply SENSE_DATA structure.
    ////Todo: return real sense data back....
    //UCHAR srb_status = SRB_STATUS_ERROR;
    //PCDB cdb = srbext->Cdb;
    //PCDB6_REQUESTSENSE request = (PCDB6_REQUESTSENSE) cdb->AsByte;
    //UINT32 alloc_size = request->AllocSize;     //cdb->CDB6GENERIC.CommandUniqueBytes[2];
    //UINT8 format = request->DescFormat;   //cdb->CDB6GENERIC.Immediate;

    //ULONG copy_size = 0;

    ////DescFormat field indicates "which format of sense data should be returned?"
    ////1 == descriptor format sense data shall be returned. (desc header + multiple SENSE_DATA)
    ////0 == return fixed format data (just return one SENSE_DATA structure)
    //SENSE_DATA_EX data = {0};
    //if (1 == format)
    //{
    //    data.DescriptorData.ErrorCode = SCSI_SENSE_ERRORCODE_DESCRIPTOR_CURRENT;
    //    data.DescriptorData.SenseKey = SCSI_SENSE_NO_SENSE;
    //    data.DescriptorData.AdditionalSenseCode = SCSI_ADSENSE_NO_SENSE;
    //    data.DescriptorData.AdditionalSenseCodeQualifier = 0;
    //    data.DescriptorData.AdditionalSenseLength = 0;
    //    srb_status = SRB_STATUS_SUCCESS;
    //    copy_size = sizeof(DESCRIPTOR_SENSE_DATA);
    //}
    //else
    //{
    //    /* Fixed Format Sense Data */
    //    data.FixedData.ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
    //    data.FixedData.SenseKey = SCSI_SENSE_NO_SENSE;
    //    data.FixedData.AdditionalSenseCode = SCSI_ADSENSE_NO_SENSE;
    //    data.FixedData.AdditionalSenseCodeQualifier = 0;
    //    data.FixedData.AdditionalSenseLength = 0;

    //    srb_status = SRB_STATUS_SUCCESS;
    //    copy_size = sizeof(SENSE_DATA);
    //}

    //if (copy_size > alloc_size)
    //    copy_size = alloc_size;

    //StorPortCopyMemory(srbext->DataBuffer, &data, copy_size);
    //SrbSetDataTransferLength(srbext->Srb, copy_size);
    //return srb_status;
}
UCHAR Scsi_Read6(PSPCNVME_SRBEXT srbext)
{
//the SCSI I/O are based for BLOCKs of device, not bytes....
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB6READWRITE, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, false);
}
UCHAR Scsi_Write6(PSPCNVME_SRBEXT srbext)
{
    //the SCSI I/O are based for BLOCKs of device, not bytes....
    ULONG64 offset = 0; //in blocks
    ULONG len = 0;    //in blocks
    PCDB cdb = srbext->Cdb();

    ParseReadWriteOffsetAndLen(cdb->CDB6READWRITE, offset, len);
    return Scsi_ReadWrite(srbext, offset, len, true);
}
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
            //in Win2000 and older version, NT SCSI system only query 
            //INQUIRYDATABUFFERSIZE bytes.
            //Since WinXP, it should return sizeof(INQUIRYDATA) bytes data. 
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
UCHAR Scsi_Verify6(PSPCNVME_SRBEXT srbext)
{
////VERIFY(6) seems obsoleted? I didn't see description in Seagate SCSI reference.
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}
UCHAR Scsi_ModeSelect6(PSPCNVME_SRBEXT srbext)
{
    CDB::_MODE_SELECT *select = &srbext->Cdb()->MODE_SELECT;
    PUCHAR buffer = (PUCHAR)srbext->DataBuf();
    ULONG mode_data_size = 0;
    ULONG offset = 0;
    PMODE_PARAMETER_HEADER header = (PMODE_PARAMETER_HEADER)buffer;
    PMODE_PARAMETER_BLOCK param_block = (PMODE_PARAMETER_BLOCK)(header+1);
    PUCHAR cursor = ((PUCHAR)param_block + header->BlockDescriptorLength);
    //ParameterList Layout of ModeSelect is:
    //[MODE_PARAMETER_HEADER][MODE_PARAMETER_BLOCK][PAGE1][PAGE2][.....]
    //There are two problem here:
    //1.PMODE_PARAMETER_BLOCK is optional. Sometimes it is not exist 
    //  so header->BlockDescriptorLength == 0.
    //2.header->ModeDataLength IS ALWAYS 0. Don't use it to check following data.
    //  (refet to Seagate SCSI Command reference. This field is reserved in MODE_SELECT cmd)
    //  You should use total buffer length to check following data(mode page) blocks.

    //currently don't support VendorSpecific MODE_SELECT op.
    if(0 == select->PFBit)
        return SRB_STATUS_INVALID_REQUEST;

#if DBG
    DbgBreakPoint();
#endif
    if(0 == header->BlockDescriptorLength)
        param_block = NULL;

    //windows set header->ModeDataLength to 0. No idea if it is bug or lazy....
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

UCHAR Scsi_ModeSense6(PSPCNVME_SRBEXT srbext)
{
    UCHAR srb_status = SRB_STATUS_ERROR;
    PCDB cdb = srbext->Cdb();
    PUCHAR buffer = (PUCHAR)srbext->DataBuf();
    PMODE_PARAMETER_HEADER header = (PMODE_PARAMETER_HEADER)buffer;
    ULONG buf_size = srbext->DataBufLen();
    ULONG ret_size = 0;
    ULONG page_size = 0;    //this is "copied ModePage size", not OS PAGE_SIZE...

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

    // Todo: reply real mode sense data
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
        //in HLK, it required "Information Exception Control Page".
        //But it is renamed to MODE_PAGE_FAULT_REPORTING in Windows Storport ....
        //refet to https://www.t10.org/ftp/t10/document.94/94-190r3.pdf
        page_size = ReplyModePageInfoExceptionCtrl(buffer, buf_size, ret_size);
        header->ModeDataLength += (UCHAR)page_size;
        srb_status = SRB_STATUS_SUCCESS;
        break;
    }
    case MODE_SENSE_RETURN_ALL:
    {
        if (buf_size > 0)
        {
        //buffer size and buffer will be updated in function.
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
UCHAR Scsi_TestUnitReady(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    return SRB_STATUS_INVALID_REQUEST;
}
// ===== End ScsiHandler_CDB6.cpp =====

// ===== Begin SrbStatus_Translator.cpp =====

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
UCHAR NvmeCmdSpecificToSrbStatus(NVME_COMMAND_STATUS &status)
{
    //todo: log the status code
    UNREFERENCED_PARAMETER(status);
    return SRB_STATUS_ERROR;
}
UCHAR NvmeMediaErrorToSrbStatus(NVME_COMMAND_STATUS &status)
{
    //todo: log the status code
    UNREFERENCED_PARAMETER(status);
    return SRB_STATUS_ERROR;
}


#if 0

//
//  Status Code (SC) of NVME_STATUS_TYPE_GENERIC_COMMAND
//
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

//
//  Status Code (SC) of NVME_STATUS_TYPE_COMMAND_SPECIFIC
//
typedef enum {

    NVME_STATUS_COMPLETION_QUEUE_INVALID = 0x00,         // Create I/O Submission Queue
    NVME_STATUS_INVALID_QUEUE_IDENTIFIER = 0x01,         // Create I/O Submission Queue, Create I/O Completion Queue, Delete I/O Completion Queue, Delete I/O Submission Queue
    NVME_STATUS_MAX_QUEUE_SIZE_EXCEEDED = 0x02,         // Create I/O Submission Queue, Create I/O Completion Queue
    NVME_STATUS_ABORT_COMMAND_LIMIT_EXCEEDED = 0x03,         // Abort
    NVME_STATUS_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED = 0x05,         // Asynchronous Event Request
    NVME_STATUS_INVALID_FIRMWARE_SLOT = 0x06,         // Firmware Commit
    NVME_STATUS_INVALID_FIRMWARE_IMAGE = 0x07,         // Firmware Commit
    NVME_STATUS_INVALID_INTERRUPT_VECTOR = 0x08,         // Create I/O Completion Queue
    NVME_STATUS_INVALID_LOG_PAGE = 0x09,         // Get Log Page
    NVME_STATUS_INVALID_FORMAT = 0x0A,         // Format NVM
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_CONVENTIONAL_RESET = 0x0B,         // Firmware Commit
    NVME_STATUS_INVALID_QUEUE_DELETION = 0x0C,         // Delete I/O Completion Queue
    NVME_STATUS_FEATURE_ID_NOT_SAVEABLE = 0x0D,         // Set Features
    NVME_STATUS_FEATURE_NOT_CHANGEABLE = 0x0E,         // Set Features
    NVME_STATUS_FEATURE_NOT_NAMESPACE_SPECIFIC = 0x0F,         // Set Features
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_NVM_SUBSYSTEM_RESET = 0x10,         // Firmware Commit
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_RESET = 0x11,         // Firmware Commit
    NVME_STATUS_FIRMWARE_ACTIVATION_REQUIRES_MAX_TIME_VIOLATION = 0x12,         // Firmware Commit
    NVME_STATUS_FIRMWARE_ACTIVATION_PROHIBITED = 0x13,         // Firmware Commit
    NVME_STATUS_OVERLAPPING_RANGE = 0x14,         // Firmware Commit, Firmware Image Download, Set Features

    NVME_STATUS_NAMESPACE_INSUFFICIENT_CAPACITY = 0x15,         // Namespace Management
    NVME_STATUS_NAMESPACE_IDENTIFIER_UNAVAILABLE = 0x16,         // Namespace Management
    NVME_STATUS_NAMESPACE_ALREADY_ATTACHED = 0x18,         // Namespace Attachment
    NVME_STATUS_NAMESPACE_IS_PRIVATE = 0x19,         // Namespace Attachment
    NVME_STATUS_NAMESPACE_NOT_ATTACHED = 0x1A,         // Namespace Attachment
    NVME_STATUS_NAMESPACE_THIN_PROVISIONING_NOT_SUPPORTED = 0x1B,         // Namespace Management
    NVME_STATUS_CONTROLLER_LIST_INVALID = 0x1C,         // Namespace Attachment

    NVME_STATUS_DEVICE_SELF_TEST_IN_PROGRESS = 0x1D,         // Device Self-test

    NVME_STATUS_BOOT_PARTITION_WRITE_PROHIBITED = 0x1E,         // Firmware Commit

    NVME_STATUS_INVALID_CONTROLLER_IDENTIFIER = 0x1F,         // Virtualization Management
    NVME_STATUS_INVALID_SECONDARY_CONTROLLER_STATE = 0x20,         // Virtualization Management
    NVME_STATUS_INVALID_NUMBER_OF_CONTROLLER_RESOURCES = 0x21,         // Virtualization Management
    NVME_STATUS_INVALID_RESOURCE_IDENTIFIER = 0x22,         // Virtualization Management

    NVME_STATUS_SANITIZE_PROHIBITED_ON_PERSISTENT_MEMORY = 0x23,         // Sanitize

    NVME_STATUS_INVALID_ANA_GROUP_IDENTIFIER = 0x24,         // Namespace Management
    NVME_STATUS_ANA_ATTACH_FAILED = 0x25,         // Namespace Attachment

    NVME_IO_COMMAND_SET_NOT_SUPPORTED = 0x29,         // Namespace Attachment/Management
    NVME_IO_COMMAND_SET_NOT_ENABLED = 0x2A,         // Namespace Attachment
    NVME_IO_COMMAND_SET_COMBINATION_REJECTED = 0x2B,         // Set Features
    NVME_IO_COMMAND_SET_INVALID = 0x2C,         // Identify

    NVME_STATUS_STREAM_RESOURCE_ALLOCATION_FAILED = 0x7F,         // Streams Directive
    NVME_STATUS_ZONE_INVALID_FORMAT = 0x7F,         // Namespace Management

    NVME_STATUS_NVM_CONFLICTING_ATTRIBUTES = 0x80,         // Dataset Management, Read, Write
    NVME_STATUS_NVM_INVALID_PROTECTION_INFORMATION = 0x81,         // Compare, Read, Write, Write Zeroes
    NVME_STATUS_NVM_ATTEMPTED_WRITE_TO_READ_ONLY_RANGE = 0x82,         // Dataset Management, Write, Write Uncorrectable, Write Zeroes
    NVME_STATUS_NVM_COMMAND_SIZE_LIMIT_EXCEEDED = 0x83,         // Dataset Management

    NVME_STATUS_ZONE_BOUNDARY_ERROR = 0xB8,         // Compare, Read, Verify, Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append
    NVME_STATUS_ZONE_FULL = 0xB9,         // Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append
    NVME_STATUS_ZONE_READ_ONLY = 0xBA,         // Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append
    NVME_STATUS_ZONE_OFFLINE = 0xBB,         // Compare, Read, Verify, Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append
    NVME_STATUS_ZONE_INVALID_WRITE = 0xBC,         // Write, Write Uncorrectable, Write Zeroes, Copy
    NVME_STATUS_ZONE_TOO_MANY_ACTIVE = 0xBD,         // Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append, Zone Management Send
    NVME_STATUS_ZONE_TOO_MANY_OPEN = 0xBE,         // Write, Write Uncorrectable, Write Zeroes, Copy, Zone Append, Zone Management Send
    NVME_STATUS_ZONE_INVALID_STATE_TRANSITION = 0xBF,         // Zone Management Send

} NVME_STATUS_COMMAND_SPECIFIC_CODES;

//
//  Status Code (SC) of NVME_STATUS_TYPE_MEDIA_ERROR
//
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

// ===== End SrbStatus_Translator.cpp =====

// ===== Begin Srb_Utils.cpp =====

UCHAR NvmeToSrbStatus(NVME_COMMAND_STATUS& status)
{
//this is most frequently passed condition, so pull it up here.
//It make common route won't consume callstack too deep.
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
void SetScsiSenseBySrbStatus(PSTORAGE_REQUEST_BLOCK srb, UCHAR &status)
{
//don't set ScsiStatus for other SRB_STATUS_xxx .
//Only SRB_STATUS_ERROR need it.
    switch (status)
    {
        case SRB_STATUS_SUCCESS:
            SrbSetScsiStatus(srb, SCSISTAT_GOOD);
            break;
        case SRB_STATUS_BUSY:   //SRB_STATUS_BUSY will check SCSISTAT....
            SrbSetScsiStatus(srb, SCSISTAT_BUSY);
            break;
        case SRB_STATUS_ERROR:
        {
            //If SRB_STATUS_ERROR go with wrong ScsiStatus, storport could treat it as SRB_STATUS_SUCCESS.

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

// ===== End Srb_Utils.cpp =====

// ===== Begin DriverEntry.cpp =====

EXTERN_C_START
sp_DRIVER_INITIALIZE DriverEntry;
ULONG DriverEntry(IN PVOID DrvObj, IN PVOID RegPath)
{
    CDebugCallInOut inout(__FUNCTION__);
    if (IsSupportedOS(10) == FALSE)
        return STOR_STATUS_UNSUPPORTED_VERSION;

    HW_INITIALIZATION_DATA init_data = { 0 };
    ULONG status = 0;

    // Set size of hardware initialization structure.
    init_data.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);

    // Identify required miniport entry point routines.
    init_data.HwInitialize = HwInitialize;
    init_data.HwBuildIo = HwBuildIo;
    init_data.HwStartIo = HwStartIo;
    init_data.HwFindAdapter = HwFindAdapter;        //AddDevice() + IRP_MJ_PNP +  IRP_MN_READ_CONFIG
    init_data.HwResetBus = HwResetBus;
    init_data.HwAdapterControl = HwAdapterControl;
    //init_data.HwUnitControl = HwUnitControl;
    init_data.HwTracingEnabled = HwTracingEnabled;
    init_data.HwCleanupTracing = HwCleanupTracing;

    //IRP_MJ_DEVICE_CONTROL + IOCTL code == IOCTL_MINIPORT_PROCESS_SERVICE_IRP
    //to prevent IOCTL request in DPC_LEVEL, define another IOCTL to make sure it is processed in PASSIVE.
    //so define this IOCTL_MINIPORT_PROCESS_SERVICE_IRP
    init_data.HwProcessServiceRequest = HwProcessServiceRequest;
    //complete NOT FINISHED IRP received in HwProcessServiceRequest. it called when device removed
    init_data.HwCompleteServiceIrp = HwCompleteServiceIrp;

    // Specifiy adapter specific information.
    init_data.AutoRequestSense = TRUE;
    init_data.NeedPhysicalAddresses = TRUE;
    init_data.AdapterInterfaceType = PCIBus;
    init_data.MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    init_data.TaggedQueuing = TRUE;
    init_data.MultipleRequestPerLu = TRUE;
    init_data.NumberOfAccessRanges = 2;
    // Specify support/use SRB Extension for Windows 8 and up
    init_data.SrbTypeFlags = SRB_TYPE_FLAG_STORAGE_REQUEST_BLOCK;
    
    //stornvme uses these features:
    //  STOR_FEATURE_DUMP_INFO
    //  STOR_FEATURE_ADAPTER_CONTROL_PRE_FINDADAPTER
    //  STOR_FEATURE_EXTRA_IO_INFORMATION
    //  STOR_FEATURE_DUMP_RESUME_CAPABLE
    //  STOR_FEATURE_DEVICE_NAME_NO_SUFFIX
    //  STOR_FEATURE_DUMP_POINTERS
    init_data.FeatureSupport = STOR_FEATURE_FULL_PNP_DEVICE_CAPABILITIES /* | STOR_FEATURE_NVME*/;

    /* Set required extension sizes. */
    init_data.DeviceExtensionSize = sizeof(CNvmeDevice);
    init_data.SrbExtensionSize = sizeof(SPCNVME_SRBEXT);

    // Call StorPortInitialize to register with HwInitData
    status = StorPortInitialize(DrvObj, RegPath, &init_data, NULL);

    return status;
}
EXTERN_C_END
// ===== End DriverEntry.cpp =====

// ===== Begin MiniportFunctions.cpp =====

static void FillPortConfiguration(PPORT_CONFIGURATION_INFORMATION portcfg, CNvmeDevice* nvme)
{
//Because MaxTxSize and MaxTxPages should be calculated by nvme->CtrlCap and nvme->CtrlIdent,
//So FillPortConfiguration() should be called AFTER nvme->IdentifyController()
    portcfg->MaximumTransferLength = nvme->MaxTxSize;
    portcfg->NumberOfPhysicalBreaks = nvme->MaxTxPages;
    portcfg->AlignmentMask = FILE_LONG_ALIGNMENT;    //PRP 1 need align DWORD in some case. So set this align is better.
    portcfg->MiniportDumpData = NULL;
    portcfg->InitiatorBusId[0] = 1;
    portcfg->CachesData = FALSE;
    portcfg->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE; //specify bounce buffer type?
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
    portcfg->Dma64BitAddresses = SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED;   //should set this value if MaxNumberOfIO > 1000.
    portcfg->MaxNumberOfIO = NVME_CONST::MAX_IO_PER_LU * NVME_CONST::MAX_LU;
    portcfg->MaxIOsPerLun = NVME_CONST::MAX_IO_PER_LU;

    //this will limit LUN i/o queue and affect HBA Gateway OutstandingMax.
    //stornvme call StorPortSetDeviceQueueDepth() to adjust it dynamically.
    portcfg->InitialLunQueueDepth = NVME_CONST::MAX_IO_PER_LU;

    //Dump is not supported now. Will be supported in future.
    portcfg->RequestedDumpBufferSize = 0;
    portcfg->DumpMode = 0;//DUMP_MODE_CRASH;
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
    //Running at PASSIVE_LEVEL!!!!!!!!!

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

    //Todo: supports multiple controller of NVMe v2.0  
    status = nvme->IdentifyController(NULL, &nvme->CtrlIdent, true);
    if (!NT_SUCCESS(status))
        goto error;
    
    //PCI bus related initialize
    //this should be called AFTER InitController() , because 
    //we need identify controller to know MaxTxSize.
    FillPortConfiguration(port_cfg, nvme);

    return SP_RETURN_FOUND;

error:
//any return code which is not SP_RETURN_FOUND causes driver installation hanging?
//I don't know why so fail this driver later....
    nvme->Teardown();
    //SP_RETURN_NOT_FOUND will cause driver installation hanging...?
    return SP_RETURN_ERROR;
}

_Use_decl_annotations_ BOOLEAN HwInitialize(PVOID devext)
{
//Running at DIRQL
    //in stornvme, it checks PPORT_CONFIGURATION_INFORMATION::DumpMode.
    //If (DumpMode != 0) , stornvme will do all Initialize here....
    //Todo: crack stornvme to know why it can do init here. This is called in DIRQL.

    CDebugCallInOut inout(__FUNCTION__);
    CNvmeDevice* nvme = (CNvmeDevice*)devext;
    NTSTATUS status = nvme->SetPerfOpts();

    if(!NT_SUCCESS(status))
        return FALSE;

    StorPortEnablePassiveInitialization(devext, HwPassiveInitialize);
    return TRUE;
}

_Use_decl_annotations_
BOOLEAN HwPassiveInitialize(PVOID devext)
{
    //Running at PASSIVE_LEVEL
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

    //CreateIoQueues should be called AFTER IdentifyController.
    status = nvme->CreateIoQueues();
    if (!NT_SUCCESS(status))
        return FALSE;

    status = nvme->RegisterIoQueues(NULL);
    if (!NT_SUCCESS(status))
        return FALSE;

    StorPortResume(devext);
    return TRUE;
}

_Use_decl_annotations_
BOOLEAN HwBuildIo(_In_ PVOID devext,_In_ PSCSI_REQUEST_BLOCK srb)
{
//BuildIo() callback is used to perform "resource preparing" and "jobs DON'T NEED lock".
//In this callback, also dispatch some behavior which need be handled very fast.
//some event (e.g. REMOVE_DEVICE and POWER_EVENTS) only fire once and need to be handled quickly.
//We can't dispatch such events to StartIo(), that could waste too much time.
    PSPCNVME_SRBEXT srbext = SPCNVME_SRBEXT::InitSrbExt(devext, (PSTORAGE_REQUEST_BLOCK)srb);
    BOOLEAN need_startio = FALSE;

    switch (srbext->FuncCode())
    {
    case SRB_FUNCTION_ABORT_COMMAND:
    case SRB_FUNCTION_RESET_LOGICAL_UNIT: //handled by HwUnitControl?
    case SRB_FUNCTION_RESET_DEVICE:
        //skip these request currently. I didn't get any idea yet to handle them.
    case SRB_FUNCTION_RESET_BUS:
    //MSDN said : 
    //  it is possible for the HwScsiStartIo routine to be called 
    //  with an SRB in which the Function member is set to SRB_FUNCTION_RESET_BUS 
    //  if a NT-based operating system storage class driver requests this operation. 
    //  The HwScsiStartIo routine can simply call the HwScsiResetBus routine 
    //  to satisfy an incoming bus-reset request.
    //  I don't understand the difference.... 
    //  Current Windows family are already all NT-based system :p
        //SrbSetSrbStatus(srb, SRB_STATUS_INVALID_REQUEST);
		srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
        need_startio = FALSE;
        break;
    //case SRB_FUNCTION_WMI:

    case SRB_FUNCTION_POWER:
        need_startio = BuildIo_SrbPowerHandler(srbext);
        break;
    case SRB_FUNCTION_EXECUTE_SCSI:
        need_startio = BuildIo_ScsiHandler(srbext);
        break;
    case SRB_FUNCTION_IO_CONTROL:
        //should check signature to determine incoming IOCTL
        need_startio = BuildIo_IoctlHandler(srbext);
        break;
    case SRB_FUNCTION_PNP:
        //should handle PNP remove adapter
        need_startio = BuildIo_SrbPnpHandler(srbext);
        break;
	default:
        need_startio = BuildIo_DefaultHandler(srbext);
        break;

    }
    return need_startio;
}

_Use_decl_annotations_
BOOLEAN HwStartIo(PVOID devext, PSCSI_REQUEST_BLOCK srb)
{
    UNREFERENCED_PARAMETER(devext);
    PSPCNVME_SRBEXT srbext = SPCNVME_SRBEXT::GetSrbExt((PSTORAGE_REQUEST_BLOCK)srb);
    UCHAR srb_status = SRB_STATUS_ERROR;

    switch (srbext->FuncCode())
    {
    //case SRB_FUNCTION_RESET_LOGICAL_UNIT:     //dispatched in HwUnitControl
    //case SRB_FUNCTION_RESET_DEVICE:           //dispatched in HwAdapterControl
    //case SRB_FUNCTION_RESET_BUS:              //dispatched in HwResetBus
    //case SRB_FUNCTION_POWER:                  //dispatched in HwBuildIo
    //case SRB_FUNCTION_ABORT_COMMAND:          //should I support abort of async I/O?
    //case SRB_FUNCTION_WMI:                    //TODO: do it later....
    //    srb_status = DefaultCmdHandler(srb, srbext);
    //    break;
    case SRB_FUNCTION_EXECUTE_SCSI:
        srb_status = StartIo_ScsiHandler(srbext);
        break;
    case SRB_FUNCTION_IO_CONTROL:
        srb_status = StartIo_IoctlHandler(srbext);
        break;
    //case SRB_FUNCTION_PNP:
        //pnp handlers
        //scsi handlers
    default:
        srb_status = StartIo_DefaultHandler(srbext);
        break;

    }

    //todo: handle SCSI status for SRB
    if (srb_status != SRB_STATUS_PENDING)
        srbext->CompleteSrb(srb_status);

    //return TRUE indicates that "this driver handled this request, 
    //no matter succeed or fail..."
    return TRUE;
}

_Use_decl_annotations_
BOOLEAN HwResetBus(
    PVOID DeviceExtension,
    ULONG PathId
)
{
    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(PathId);
    //miniport driver is responsible for completing SRBs received by HwStorStartIo for 
    //PathId during this routine and setting their status to SRB_STATUS_BUS_RESET if necessary.
    CNvmeDevice* nvme = (CNvmeDevice*)DeviceExtension;
    DbgBreakPoint();
    nvme->ResetOutstandingCmds();
    return TRUE;
}

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
    //Device "entering" D1 / D2 / D3 from D0
    //**running at DIRQL
        //this is post event of DeviceRemove. 
        //In normal procedure of removing device, we don't have to do anything here.
        //BuildIo SRB_FUNCTION_PNP should handle DEVICE_REMOVE teardown.
        //ScsiStopAdapter is just a power state handler, we have no idea that
        //"is this call from a device remove? or sleep? or hibernation? or power down?"
        status = ScsiAdapterControlSuccess;
        break;
    }
    case ScsiRestartAdapter:
    {
    //Device "entering" D0 state from D1 D2 D3 state
    //This control code do similar things(exclude PerfConfig) as HwInitialize().
    //**running at DIRQL
        status = Handle_RestartAdapter(nvme);
        break;
    }
    case ScsiAdapterSurpriseRemoval:
    {
    //**running < DISPATCH_LEVEL
    //Device is surprise removed.
    //**Will SRB_PNP_xxx still be fired if this control code supported/implemented?
    //***SurpriseRemove don't need unregister queues. Only need to delete queues.
        nvme->Teardown();
        status = ScsiAdapterControlSuccess;
        break;
    }
    //Valid since Win8. If this control enabled, storport will notify 
    //miniport when power plan changed.
    case ScsiPowerSettingNotification:
    {
     //**running at PASSIVE_LEVEL
        STOR_POWER_SETTING_INFO* info = (STOR_POWER_SETTING_INFO*) Parameters;
        UNREFERENCED_PARAMETER(info);
        status = ScsiAdapterControlSuccess;
        break;
    }

    //Valid since Win8. If this control enabled, miniport won't receive
    //SRB_FUNCTION_POWER in BuildIo and no ScsiStopAdapter in AdapterControl
    case ScsiAdapterPower:
    {
     //**running <= DISPATCH_LEVEL
      STOR_ADAPTER_CONTROL_POWER *power = (STOR_ADAPTER_CONTROL_POWER *)Parameters;
      UNREFERENCED_PARAMETER(power);
      status = ScsiAdapterControlSuccess;
      break;
    }

#pragma region === Some explain of un-implemented control codes ===
    //If STOR_FEATURE_ADAPTER_CONTROL_PRE_FINDADAPTER is set in HW_INITIALIZATION_DATA of DriverEntry,
    // storport will fire this control code when handling IRP_MN_FILTER_RESOURCE_REQUIREMENTS.
    // **In this control code, DeviceExtension is STILL NOT initialized because HwFindAdapter not called yet.
    //case ScsiAdapterFilterResourceRequirements:
    //{
    //  //**running < DISPATCH_LEVEL
    //  STOR_FILTER_RESOURCE_REQUIREMENTS* filter = (STOR_FILTER_RESOURCE_REQUIREMENTS*)Parameters;
    //    break;
    //}

    //storport call this control code before  ScsiRestartAdapter.
    // interrupt is NOT connected yet here.
    // If HBA need restore config and resource via StorPortGetBusData() 
    // or StorPortSetBusDataByOffset(), we should implement this control code.
    // ** this means if you need re-touch PCIe resource and reconfig 
    //    runtime config(remap io space...etc), you'll need this control code.
    //case ScsiSetRunningConfig:
    //{
    //     //**running at PASSIVE_LEVEL
    //    //status = HandleScsiSetRunningConfig();
    //    break;
    //}
#pragma endregion

    default:
        status = ScsiAdapterControlUnsuccessful;
    }
    return status;
}

_Use_decl_annotations_
void HwProcessServiceRequest(
    PVOID DeviceExtension,
    PVOID Irp
)
{
    //If there are DeviceIoControl use IOCTL_MINIPORT_PROCESS_SERVICE_IRP, 
    //we should implement this callback.
    
    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(DeviceExtension);
    PIRP irp = (PIRP) Irp;
    //UNREFERENCED_PARAMETER(Irp);
    ////ioctl interface for miniport
    //PSMOKY_EXT devext = (PSMOKY_EXT)DeviceExtension;
    //PIRP irp = (PIRP)Irp;
    //irp->IoStatus.Information = 0;
    //irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    //StorPortCompleteServiceIrp(devext, irp);

    irp->IoStatus.Information = 0;
    irp->IoStatus.Status = STATUS_SUCCESS;
    StorPortCompleteServiceIrp(DeviceExtension, irp);
}

_Use_decl_annotations_
void HwCompleteServiceIrp(PVOID DeviceExtension)
{
    //If HwProcessServiceRequest()is implemented, this HwCompleteServiceIrp 
    // wiill be called before stop to retrieve any new IOCTL_MINIPORT_PROCESS_SERVICE_IRP requests.
    //This callback give us a chance to cleanup requests.

    //example: if IOCTL_MINIPORT_PROCESS_SERVICE_IRP handler send IRP to 
    //          another device and waiting result, we should cancel waiting 
    //          and cleanup that IRP in this callback.

    CDebugCallInOut inout(__FUNCTION__);
    UNREFERENCED_PARAMETER(DeviceExtension);
    //if any async request in HwProcessServiceRequest, 
    //we should complete them here and let them go back asap.
}

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

    //UnitControl is very similar as AdapterControl.
    //First call will query "ScsiQuerySupportedControlTypes", then 
    //miniport need fill corresponding element to report.

    //ScsiUnitStart => a unit is starting up (disk spin up?)
    //ScsiUnitPower => unit power on or off, [Parameters] arg is STOR_UNIT_CONTROL_POWER*
    //ScsiUnitRemove => DeviceRemove post event of Unit
    //ScsiUnitSurpriseRemoval => SurpriseRemoved event of Unit
    
    //UnitControl should handle events of LU:
    //1.Power States
    //2.Device Start
    //3.Device Remove and Surprise Remove
    return ScsiUnitControlSuccess;
}

_Use_decl_annotations_
VOID HwTracingEnabled(
    _In_ PVOID HwDeviceExtension,
    _In_ BOOLEAN Enabled
)
{
    UNREFERENCED_PARAMETER(HwDeviceExtension);
    UNREFERENCED_PARAMETER(Enabled);

    //miniport should write its own ETW log via StorPortEtwEventXXX API (refer to storport.h)
    // So HwTracingEnabled and HwCleanupTracing are used to "turn on" and "turn off" its own ETW logging mechanism.
}

_Use_decl_annotations_
VOID HwCleanupTracing(
    _In_ PVOID  Arg1
)
{
    UNREFERENCED_PARAMETER(Arg1);
}
// ===== End MiniportFunctions.cpp =====

// ===== Begin NvmeDevice.cpp =====

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

    //todo: log error
    //STOR_STATUS_BUSY : already queued this workitem.
    //STOR_STATUS_INVALID_DEVICE_STATE : device is removing.
    //STOR_STATUS_INVALID_IRQL: IRQL > DISPATCH_LEVEL
    StorPortInitializeWorker(nvme, &nvme->RestartWorker);
    stor_status = StorPortQueueWorkItem(DevExt, CNvmeDevice::RestartAdapterWorker, nvme->RestartWorker, NULL);
    ASSERT(stor_status == STOR_STATUS_SUCCESS);
}
void CNvmeDevice::RestartAdapterWorker(
    _In_ PVOID DevExt,
    _In_ PVOID Context,
    _In_ PVOID Worker)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Worker);

    CNvmeDevice* nvme = (CNvmeDevice*)DevExt;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    //ULONG stor_status = STOR_STATUS_SUCCESS;
    if (!nvme->IsWorking())
        return;

    //In InitController, DisableController and EnableController could call 
    //StorPortStallExecution(). It could cause system lag if called in DIRQL.
    //So I move InitController() here...
    status = nvme->InitNvmeStage1();
    ASSERT(NT_SUCCESS(status));

    status = nvme->InitNvmeStage2();
    ASSERT(NT_SUCCESS(status));
    //resume adapter AFTER restart controller done.
    StorPortResume(DevExt);
    StorPortFreeWorker(nvme, &nvme->RestartWorker);
    nvme->RestartWorker = NULL;
}

#pragma region ======== CSpcNvmeDevice inline routines ======== 
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier)
{
    if(barrier)
        MemoryBarrier();
    cc.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->CC.AsUlong);
}
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    csts.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->CSTS.AsUlong);
}
inline void CNvmeDevice::ReadNvmeRegister(NVME_VERSION& ver, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    ver.AsUlong = StorPortReadRegisterUlong(this, &CtrlReg->VS.AsUlong);
}
inline void CNvmeDevice::ReadNvmeRegister(NVME_CONTROLLER_CAPABILITIES& cap, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    cap.AsUlonglong = StorPortReadRegisterUlong64(this, &CtrlReg->CAP.AsUlonglong);
}
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
inline void CNvmeDevice::WriteNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    StorPortWriteRegisterUlong(this, &CtrlReg->CC.AsUlong, cc.AsUlong);
}
inline void CNvmeDevice::WriteNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier)
{
    if (barrier)
        MemoryBarrier();
    StorPortWriteRegisterUlong(this, &CtrlReg->CSTS.AsUlong, csts.AsUlong);
}
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
inline BOOLEAN CNvmeDevice::IsControllerEnabled(bool barrier)
{
    NVME_CONTROLLER_CONFIGURATION cc = {0};
    ReadNvmeRegister(cc, barrier);
    return (TRUE == cc.EN)?TRUE:FALSE;
}
inline BOOLEAN CNvmeDevice::IsControllerReady(bool barrier)
{
    NVME_CONTROLLER_STATUS csts = { 0 };
    ReadNvmeRegister(csts, barrier);
    return (TRUE == csts.RDY && FALSE == csts.CFS) ? TRUE : FALSE;
}
inline void CNvmeDevice::GetAdmQueueDbl(PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL& sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL& cpl)
{
    GetQueueDbl(0, sub, cpl);
}
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
inline void CNvmeDevice::UpdateMaxTxSize()
{
    this->MaxTxSize = (ULONG)((1 << this->CtrlIdent.MDTS) * this->MinPageSize);
    this->MaxTxPages = (ULONG)(this->MaxTxSize / PAGE_SIZE);
}
#if 0
ULONG CNvmeDevice::MinPageSize()
{
    return (ULONG)(1 << (12 + CtrlCap.MPSMIN));
}
ULONG CNvmeDevice::MaxPageSize()
{
    return (ULONG)(1 << (12 + CtrlCap.MPSMAX));
}
ULONG CNvmeDevice::MaxTxSize()
{
    return (ULONG)((1 << this->CtrlIdent.MDTS) * MinPageSize());
}
ULONG CNvmeDevice::MaxTxPages()
{
    return (ULONG)(MaxTxSize() / PAGE_SIZE);
}
ULONG CNvmeDevice::NsCount()
{
    return NamespaceCount;
}
#endif
bool CNvmeDevice::IsWorking() { return (State == NVME_STATE::RUNNING); }
bool CNvmeDevice::IsSetup() { return (State == NVME_STATE::SETUP); }
bool CNvmeDevice::IsTeardown() { return (State == NVME_STATE::TEARDOWN); }
bool CNvmeDevice::IsStop() { return (State == NVME_STATE::STOP); }
#pragma endregion

#pragma region ======== CSpcNvmeDevice ======== 
#if 0
bool CNvmeDevice::GetMsixTable()
{
}
#endif 
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

    //todo: handle NUMA nodes for each queue
    //KeQueryLogicalProcessorRelationship(&ProcNum, RelationNumaNode, &ProcInfo, &ProcInfoSize);
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
void CNvmeDevice::Teardown()
{
    if (!IsWorking())
        return;

    State = NVME_STATE::TEARDOWN;
    DeleteIoQ();
    DeleteAdmQ();
    State = NVME_STATE::STOP;
}
NTSTATUS CNvmeDevice::EnableController()
{
    if (IsControllerReady())
        return STATUS_SUCCESS;

    //if set CC.EN = 1 WHEN CSTS.RDY == 1 and CC.EN == 0, it is undefined behavior.
    //we should wait controller state changing until (CC.EN == 0 and CSTS.RDY == 0).
    bool ok = WaitForCtrlerState(DeviceTimeout, FALSE, FALSE);
    if (!ok)
        return STATUS_INVALID_DEVICE_STATE;

    //before Enable, update these basic information to controller.
    //these fields only can be modified when CC.EN == 0. (plz refer to nvme 1.3 spec)
    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    cc.CSS = NVME_CSS_NVM_COMMAND_SET;
    cc.AMS = NVME_AMS_ROUND_ROBIN;
    cc.SHN = NVME_CC_SHN_NO_NOTIFICATION;
    cc.IOSQES = NVME_CONST::IOSQES;
    cc.IOCQES = NVME_CONST::IOCQES;
    cc.EN = 0;
    WriteNvmeRegister(cc);

    //take a break let controller have enough time to retrieve CC values.
    StorPortStallExecution(StallDelay);

    cc.EN = 1;
    WriteNvmeRegister(cc);

    ok = WaitForCtrlerState(DeviceTimeout, TRUE);

    if(!ok)
        return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::DisableController()
{
    //if (!IsWorking())
    //    return STATUS_INVALID_DEVICE_STATE;

    if(!IsControllerReady())
        return STATUS_SUCCESS;

    //if set CC.EN = 1 WHEN CSTS.RDY == 1 and CC.EN == 0, it is undefined behavior.
    //we should wait controller state changing until (CC.EN == 0 and CSTS.RDY == 0).
    bool ok = WaitForCtrlerState(DeviceTimeout, TRUE, TRUE);
    if (!ok)
        return STATUS_INVALID_DEVICE_STATE;

    //before Enable, update these basic information to controller.
    //these fields only can be modified when CC.EN == 0. (plz refer to nvme 1.3 spec)
    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    ReadNvmeRegister(cc);
    cc.EN = 0;
    WriteNvmeRegister(cc);

    //take a break let controller have enough time to retrieve CC values.
    StorPortStallExecution(StallDelay);
    ok = WaitForCtrlerState(DeviceTimeout, FALSE);

    if (!ok)
        return STATUS_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}

//refet to NVME 1.3 , chapter 3.1
//This function set CC.SHN and wait CSTS.SHST==2.
//If called this function then you want to do anything(e.g. submit cmd), you should do
//DisableController()->EnableController. 
//Note : (In NVMe 1.3 spec) If CC.SHN shutdown progress issued then didn't do restart controler, 
//       all following behavior (e.g. submit new cmd) are UNDEFINED BEHAVIOR.
NTSTATUS CNvmeDevice::ShutdownController()
{
    if (!IsStop() && !IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    State = NVME_STATE::SHUTDOWN;
    NVME_CONTROLLER_CONFIGURATION cc = { 0 };
    //NTSTATUS status = STATUS_SUCCESS;
    //if set CC.EN = 0 WHEN CSTS.RDY == 0 and CC.EN == 1, it is undefined behavior.
    //we should wait controller state changing until (CC.EN == 1 and CSTS.RDY == 1).
    bool ok = WaitForCtrlerState(DeviceTimeout, TRUE, TRUE);
    if (!ok)
        goto ERROR_BSOD;

    ReadNvmeRegister(cc);
    cc.SHN = NVME_CC_SHN_NORMAL_SHUTDOWN;
    WriteNvmeRegister(cc);

    //VMware NVMe 1.0 not guarantee CSTS.SHST will response?
    ok = WaitForCtrlerShst(DeviceTimeout);
    //if (!ok)
    //    goto ERROR_BSOD;

    return DisableController();

ERROR_BSOD:
    NVME_CONTROLLER_STATUS csts = { 0 };
    ReadNvmeRegister(cc);
    ReadNvmeRegister(csts);
    KeBugCheckEx(BUGCHECK_ADAPTER, (ULONG_PTR)this, (ULONG_PTR)cc.AsUlong, (ULONG_PTR)csts.AsUlong, 0);
}
NTSTATUS CNvmeDevice::InitController()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    NTSTATUS status = STATUS_SUCCESS;
    status = DisableController();
    
    //Disable NVMe Contoller will unregister all I/O queues automatically.
    RegisteredIoQ = 0;

    if(!NT_SUCCESS(status))
        return status;
    status = RegisterAdmQ();
    if (!NT_SUCCESS(status))
        return status;
    status = EnableController();
    return status;
}
NTSTATUS CNvmeDevice::InitNvmeStage1()
{
    NTSTATUS status = STATUS_SUCCESS;

    //Todo: supports multiple controller of NVMe v2.0  
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
NTSTATUS CNvmeDevice::InitNvmeStage2()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    //todo: add HostBuffer and AsyncEvent supports
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
NTSTATUS CNvmeDevice::RestartController()
{
//***Running at DIRQL, called by HwAdapterControl
    BOOLEAN ok = FALSE;
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    //stop handling any request BEFORE restart HBA done.
    StorPortPause(this, MAXULONG);

    //[workaround]
    //StorPortQueueWorkItem() only can be called at IRQL <= DISPATCH_LEVEL.
    //And 
    //CNvmeDevice::RegisterIoQueue() should be called at IRQL < DISPATCH_LEVEL.
    //So I have to call DPC to do StorPortQueueWorkItem().
    ok = StorPortIssueDpc(this, &this->RestartDpc, NULL, NULL);
    ASSERT(ok);
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::IdentifyAllNamespaces()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    CAutoPtr<ULONG, NonPagedPool, DEV_POOL_TAG> idlist(new ULONG[NVME_CONST::MAX_NS_COUNT]);
    ULONG ret_count = 0;
    status = IdentifyActiveNamespaceIdList(NULL, idlist, ret_count);
    if(!NT_SUCCESS(status))
        return status;

    //query ns one by one. NS ID is 1 based index
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
NTSTATUS CNvmeDevice::IdentifyFirstNamespace()
{
    NTSTATUS status = IdentifyNamespace(NULL, 1, &this->NsData[0]);
    if(NT_SUCCESS(status))
        NamespaceCount = 1;
    return status;
}
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
NTSTATUS CNvmeDevice::IdentifyController(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA ident, bool poll)
{
    if (!IsWorking())
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
    
    //if(poll == true) means this request comes from StartIo
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

    //Query ID List of all active Namespace
    BuildCmd_IdentSpecifiedNS(my_srbext, data, nsid);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);
    //if(poll == true) means this request comes from StartIo
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
NTSTATUS CNvmeDevice::IdentifyActiveNamespaceIdList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, ULONG& ret_count)
{
//list_count is "how many elemens(not bytes) in nsid_list can store."
//nsid_list is buffer to retrieve nsid returned by this command.
//ret_count is "how many actual nsid(elements, not bytes) returned by this command".
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

    //Query ID List of all active Namespace
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
NTSTATUS CNvmeDevice::SetNumberOfIoQueue(USHORT count)
{
    //this is SET_FEATURE of NVMe Adm Command.
    //The flow is :
    //  1.host tell device "how many i/o queues I want to use".
    //  2.device reply to host "how many queues I can permit you to use".
    //CNvmeDevice::DesiredIoQ should be filled by step2's answer.
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    CAutoPtr<SPCNVME_SRBEXT, NonPagedPool, DEV_POOL_TAG> my_srbext(new SPCNVME_SRBEXT());
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    my_srbext->Init(this, NULL);

    BuildCmd_SetIoQueueCount(my_srbext, count);
    status = SubmitAdmCmd(my_srbext, &my_srbext->NvmeCmd);

    //if(poll == true) means this request comes from StartIo
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
NTSTATUS CNvmeDevice::SetAsyncEvent()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetAsyncEvent() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::SetArbitration()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetArbitration() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::SetSyncHostTime()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetSyncHostTime() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::SetPowerManagement()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, 0, "SetPowerManagement() still not implemented yet!!\n");
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::GetLbaFormat(ULONG nsid, NVME_LBA_FORMAT& format)
{
    //namespace id should be 1 based.
    //** according NVME_COMMAND::CDW0, NSID==0 if not used in command.
    //   So I guess the NSID is 1 based index....
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    UCHAR lba_index = NsData[nsid-1].FLBAS.LbaFormatIndex;
    RtlCopyMemory(&format, &NsData[nsid - 1].LBAF[lba_index], sizeof(NVME_LBA_FORMAT));
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::GetNamespaceBlockSize(ULONG nsid, ULONG& size)
{
    //namespace id should be 1 based.
    //** according NVME_COMMAND::CDW0, NSID==0 if not used in command.
    //   So I guess the NSID is 1 based index....
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    UCHAR lba_index = NsData[nsid - 1].FLBAS.LbaFormatIndex;
    size = (1 << NsData[nsid - 1].LBAF[lba_index].LBADS);
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::GetNamespaceTotalBlocks(ULONG nsid, ULONG64& blocks)
{
    //namespace id should be 1 based.
    //** according NVME_COMMAND::CDW0, NSID==0 if not used in command.
    //   So I guess the NSID is 1 based index....
    if (0 == nsid)
        return STATUS_INVALID_PARAMETER;

    if (0 == NamespaceCount)
        return STATUS_DEVICE_NOT_READY;

    blocks = NsData[nsid - 1].NSZE;
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::SubmitAdmCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd)
{
    if(!IsWorking() || NULL == AdmQueue)
        return STATUS_DEVICE_NOT_READY;
    return AdmQueue->SubmitCmd(srbext, cmd);
}
NTSTATUS CNvmeDevice::SubmitIoCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd)
{
    if (!IsWorking() || NULL == IoQueue)
        return STATUS_DEVICE_NOT_READY;

    ULONG cpu_idx = KeGetCurrentProcessorNumberEx(NULL);
    srbext->IoQueueIndex = (cpu_idx % RegisteredIoQ);
    return IoQueue[srbext->IoQueueIndex]->SubmitCmd(srbext, cmd);
}
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
NTSTATUS CNvmeDevice::SetPerfOpts()
{
    if (!IsWorking())
        return STATUS_INVALID_DEVICE_STATE;

    //initialize perf options
    PERF_CONFIGURATION_DATA set_perf = { 0 };
    PERF_CONFIGURATION_DATA supported = { 0 };
    ULONG stor_status = STOR_STATUS_SUCCESS;
    //Just using STOR_PERF_VERSION_5, STOR_PERF_VERSION_6 is for Win2019 and above...
    supported.Version = STOR_PERF_VERSION_5;
    supported.Size = sizeof(PERF_CONFIGURATION_DATA);
    stor_status = StorPortInitializePerfOpts(this, TRUE, &supported);
    if (STOR_STATUS_SUCCESS != stor_status)
        return FALSE;

    set_perf.Version = STOR_PERF_VERSION_5;
    set_perf.Size = sizeof(PERF_CONFIGURATION_DATA);

    //Allow multiple I/O incoming concurrently. 
    if(0 != (supported.Flags & STOR_PERF_CONCURRENT_CHANNELS))
    {
        set_perf.Flags |= STOR_PERF_CONCURRENT_CHANNELS;
        set_perf.ConcurrentChannels = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    }
    //I don't use SGL... but Win10 don't support this flag
    if (0 != (supported.Flags & STOR_PERF_NO_SGL))
        set_perf.Flags |= STOR_PERF_NO_SGL;
    //IF not set this flag, storport will attempt to fire completion DPC on
    //original cpu which accept this I/O request.
    if (0 != (supported.Flags & STOR_PERF_DPC_REDIRECTION_CURRENT_CPU))
    {
        set_perf.Flags |= STOR_PERF_DPC_REDIRECTION_CURRENT_CPU;
    }

    //spread DPC to all cpu. don't make single cpu too busy.
    if (0 != (supported.Flags & STOR_PERF_DPC_REDIRECTION))
        set_perf.Flags |= STOR_PERF_DPC_REDIRECTION;

    stor_status = StorPortInitializePerfOpts(this, FALSE, &set_perf);
    if(STOR_STATUS_SUCCESS != stor_status)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}
bool CNvmeDevice::IsInValidIoRange(ULONG nsid, ULONG64 offset, ULONG len)
{
    //namespace id should be 1 based.
    //** according NVME_COMMAND::CDW0, NSID==0 if not used in command.
    //   So I guess the NSID is 1 based index....
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
//register IoQueue should register CplQ first, then SubQ.
//They are "QueuePair" .
        BuildCmd_RegIoCplQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if(!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            //when Register I/O Queues, Interrupt are already connected.
            //We just wait for ISR complete commands...
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
            //when Register I/O Queues, Interrupt are already connected.
            //We just wait for ISR complete commands...
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
        //register IoQueue should register CplQ first, then SubQ.
        //They are "Pair" .
        //when UNREGISTER IoQueues, the sequence should be reversed :
        // unregister SubQ first, then CplQ.
        BuildCmd_UnRegIoSubQ(temp, IoQueue[i]);
        status = AdmQueue->SubmitCmd(temp, &temp->NvmeCmd);
        if (!NT_SUCCESS(status))
        {
            status = STATUS_REQUEST_ABORTED;
            goto END;
        }

        do
        {
            //when Register I/O Queues, Interrupt are already connected.
            //We just wait for ISR complete commands...
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
            //when Register I/O Queues, Interrupt are already connected.
            //We just wait for ISR complete commands...
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
    cfg.HistoryDepth = NVME_CONST::MAX_IO_PER_LU;    //HistoryDepth should equal to MaxScsiTag (ScsiTag Depth).
    GetAdmQueueDbl(cfg.SubDbl , cfg.CplDbl);
    AdmQueue = new CNvmeQueue(&cfg);
    if(!AdmQueue->IsInitOK())
        return STATUS_MEMORY_NOT_ALLOCATED;
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::RegisterAdmQ()
{
//AQA register should be only modified when csts.RDY==0(cc.EN == 0)
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
    aqa.ASQS = AdmDepth - 1;    //ASQS and ACQS are zero based index. here we should fill "MAX index" not total count;
    aqa.ACQS = AdmDepth - 1;
    asq.AsUlonglong = (ULONGLONG)subq.QuadPart;
    acq.AsUlonglong = (ULONGLONG)cplq.QuadPart;
    WriteNvmeRegister(aqa, asq, acq);
    return STATUS_SUCCESS;
}
NTSTATUS CNvmeDevice::UnregisterAdmQ()
{
    //AQA register should be only modified when (csts.RDY==0 && cc.EN == 0)
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
NTSTATUS CNvmeDevice::DeleteAdmQ()
{
    if(NULL == AdmQueue)
        return STATUS_MEMORY_NOT_ALLOCATED;

    AdmQueue->Teardown();
    delete AdmQueue;
    AdmQueue = NULL;
    return STATUS_SUCCESS;
}
void CNvmeDevice::ReadCtrlCap()
{
    //CtrlReg->CAP.TO is timeout value in "500 ms" unit.
    //It indicates the timeout worst case of Enabling/Disabling Controller.
    //e.g. CAP.TO==3 means worst case timout to Enable/Disable controller is 1500 milli-second.
    //We should convert it to micro-seconds for StorPortStallExecution() using.
    
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
bool CNvmeDevice::MapCtrlRegisters()
{
    BOOLEAN in_iospace = FALSE;
    STOR_PHYSICAL_ADDRESS bar0 = { 0 };
    INTERFACE_TYPE type = PortCfg->AdapterInterfaceType;
    //I got this mapping method by cracking stornvme.sys.
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
bool CNvmeDevice::GetPciBusData(INTERFACE_TYPE type, ULONG bus, ULONG slot)
{
    ULONG size = sizeof(PciCfg);
    ULONG status = StorPortGetBusData(this, type, bus, slot, &PciCfg, size);

    //refer to MSDN StorPortGetBusData() to check why 2==status is error.
    if (2 == status || status != size)
        return false;

    VendorID = PciCfg.VendorID;
    DeviceID = PciCfg.DeviceID;
    return true;
}
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
bool CNvmeDevice::WaitForCtrlerShst(ULONG time_us)
{
    ULONG elapsed = 0;
    //BOOLEAN is_ready = IsControllerReady();
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
void CNvmeDevice::InitVars()
{
    CtrlReg = NULL;
    PortCfg = NULL;
    Doorbells = NULL;

    VendorID = 0;
    DeviceID = 0;
    State = NVME_STATE::STOP;
    CpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    TotalNumaNodes = 0;     //todo: query total numa nodes.
    RegisteredIoQ = 0;
    AllocatedIoQ = 0;
    DesiredIoQ = NVME_CONST::IO_QUEUE_COUNT;
    DeviceTimeout = 2000 * NVME_CONST::STALL_TIME_US;        //should be updated by CAP, unit in micro-seconds
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
void CNvmeDevice::LoadRegistry()
{
    ULONG size = sizeof(ULONG);
    ULONG ret_size = 0;
    BOOLEAN ok = FALSE;
    UCHAR* buffer = StorPortAllocateRegistryBuffer(this, &size);

    if (buffer == NULL)
        return;

    RtlZeroMemory(buffer, size);
    //ret_size should assign buffer length when calling in,
    //then it will return "how many bytes read from registry".
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
NTSTATUS CNvmeDevice::CreateIoQ()
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    QUEUE_PAIR_CONFIG cfg = { 0 };
    cfg.DevExt = this;
    cfg.Depth = IoDepth;
    cfg.NumaNode = 0;
    cfg.Type = QUEUE_TYPE::IO_QUEUE;
    cfg.HistoryDepth = NVME_CONST::MAX_IO_PER_LU;    //HistoryDepth should equal to MaxScsiTag (ScsiTag Depth).

    for(USHORT i=0; i<DesiredIoQ; i++)
    {
        if(NULL != IoQueue[i])
            continue;
        CNvmeQueue* queue = new CNvmeQueue();
        //Dbl[0] is for AdminQ
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

// ===== End NvmeDevice.cpp =====

// ===== Begin NvmeQueue.cpp =====
static __inline size_t CalcQueueBufferSize(USHORT depth)
{
    size_t page_count = BYTES_TO_PAGES(depth * sizeof(NVME_COMMAND)) +
                    BYTES_TO_PAGES(depth * sizeof(NVME_COMPLETION_ENTRY));
    return page_count * PAGE_SIZE;
}

static __inline ULONG ReadDbl(PVOID devext, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL dbl)
{
//In release build, Read/Write Register use READ_REGISTER_ULONG / WRITE_REGISTER_ULONG
//Macros. They all have FastFence() already so no need to call MemoryBarrier in release build.
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    return StorPortReadRegisterUlong(devext, &dbl->AsUlong);
}
static __inline ULONG ReadDbl(PVOID devext, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL dbl)
{
//In release build, Read/Write Register use READ_REGISTER_ULONG / WRITE_REGISTER_ULONG
//Macros. They all have FastFence() already so no need to call MemoryBarrier in release build.
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    return StorPortReadRegisterUlong(devext, &dbl->AsUlong);
}
static __inline void WriteDbl(PVOID devext, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL dbl, ULONG value)
{
//In release build, Read/Write Register use READ_REGISTER_ULONG / WRITE_REGISTER_ULONG
//Macros. They all have FastFence() already so no need to call MemoryBarrier in release build.
#if !defined(DBG)
    UNREFERENCED_PARAMETER(devext);
#endif
    //Doorbell should write 32bit ULONG.
    //Some controller won't accept the doorbell value if you write only 16bit USHORT value.
    MemoryBarrier();
    StorPortWriteRegisterUlong(devext, &dbl->AsUlong, value);
}
static __inline void WriteDbl(PVOID devext, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL dbl, ULONG value)
{
//In release build, Read/Write Register use READ_REGISTER_ULONG / WRITE_REGISTER_ULONG
//Macros. They all have FastFence() already so no need to call MemoryBarrier in release build.
#if !defined(DBG)
    MemoryBarrier();
    UNREFERENCED_PARAMETER(devext);
#endif
    //Doorbell should write 32bit ULONG.
    //Some controller won't accept the doorbell value if you write only 16bit USHORT value.
    StorPortWriteRegisterUlong(devext, &dbl->AsUlong, value);
}
static __inline bool IsValidQid(ULONG qid)
{
    return (qid != NVME_INVALID_QID);
}
static __inline bool NewCplArrived(PNVME_COMPLETION_ENTRY entry, USHORT current_tag)
{
    if (entry->DW3.Status.P == current_tag)
        return true;
    return false;
}
static __inline void UpdateCplHead(ULONG &cpl_head, USHORT depth)
{
    cpl_head = (cpl_head + 1) % depth;
}
static __inline void UpdateCplHeadAndPhase(ULONG& cpl_head, USHORT& phase, USHORT depth)
{
    UpdateCplHead(cpl_head, depth);
    if (0 == cpl_head)
        phase = !phase;
    //quick calculation, write a boolean table will know why.
    //    phase = !(cpl_head ^ phase);      
}
#pragma region ======== class CNvmeQueue ========

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

CNvmeQueue::CNvmeQueue()
{
    KeInitializeSpinLock(&SubLock);
}
CNvmeQueue::CNvmeQueue(QUEUE_PAIR_CONFIG* config)
    : CNvmeQueue()
{
    Setup(config);
}
CNvmeQueue::~CNvmeQueue()
{
    Teardown();
}
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
    //Todo: StorPortNotification(SetTargetProcessorDpc)
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
        //status = STATUS_INTERNAL_ERROR;
        goto ERROR;
    }

    IsReady = true;
    return STATUS_SUCCESS;
ERROR:
    Teardown();
    return status;
}
void CNvmeQueue::Teardown()
{
    this->IsReady = false;
    DeallocQueueBuffer();
    History.Teardown();
}
NTSTATUS CNvmeQueue::SubmitCmd(SPCNVME_SRBEXT* srbext, PNVME_COMMAND src_cmd)
{
    CSpinLock lock(&SubLock);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG cid = 0;
    if (!this->IsReady)
        return STATUS_DEVICE_NOT_READY;

    //throttle of submittion. If SubTail exceed CplHead, 
    //NVMe device will have fatal error and stopped.
    if(!IsSafeForSubmit())
        return STATUS_DEVICE_BUSY;

    cid = (ULONG)(src_cmd->CDW0.CID & 0xFFFF);
    ASSERT(cid == src_cmd->CDW0.CID);
    status = History.Push(cid, srbext);
    if (STATUS_ALREADY_COMMITTED == status)
    {
        DbgBreakPoint();
        //old cmd is timed out, release it...
        PSPCNVME_SRBEXT old_srbext = NULL;
        History.Pop(cid, old_srbext);
        old_srbext->CompleteSrb(SRB_STATUS_BUSY);

        //after release old cmd, push current cmd again...
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
void CNvmeQueue::CompleteCmd(ULONG max_count)
{
    //DPC is global scope mutex?
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
void CNvmeQueue::GetQueueAddr(PVOID* subq, PVOID* cplq)
{  
    if(subq != NULL)
        *subq = SubQ_VA;

    if (cplq != NULL)
        *cplq = CplQ_VA;
}
void CNvmeQueue::GetQueueAddr(PVOID* subva, PHYSICAL_ADDRESS* subpa, PVOID* cplva, PHYSICAL_ADDRESS* cplpa)
{
    GetQueueAddr(subva, cplva);
    GetQueueAddr(subpa, cplpa);
}
void CNvmeQueue::GetQueueAddr(PHYSICAL_ADDRESS* subq, PHYSICAL_ADDRESS* cplq)
{
    GetSubQAddr(subq);
    GetCplQAddr(cplq);
}
void CNvmeQueue::GetSubQAddr(PHYSICAL_ADDRESS* subq)
{
    subq->QuadPart = SubQ_PA.QuadPart;
}
void CNvmeQueue::GetCplQAddr(PHYSICAL_ADDRESS* cplq)
{
    cplq->QuadPart = CplQ_PA.QuadPart;
}
bool CNvmeQueue::IsSafeForSubmit()
{
    return ((Depth - NVME_CONST::SAFE_SUBMIT_THRESHOLD) > (USHORT)InflightCmds)? true : false;
}
ULONG CNvmeQueue::ReadSubTail()
{
    if (IsValidQid(QueueID) && NULL != SubDbl)
        return ReadDbl(DevExt, SubDbl);
    KdBreakPoint();
    return NVME_CONST::INVALID_DBL_VALUE;
}
void CNvmeQueue::WriteSubTail(ULONG value)
{
    if (IsValidQid(QueueID) && NULL != SubDbl)
        return WriteDbl(DevExt, SubDbl, value);
    KdBreakPoint();
}
ULONG CNvmeQueue::ReadCplHead()
{
    if (IsValidQid(QueueID) && NULL != CplDbl)
        return ReadDbl(DevExt, CplDbl);
    KdBreakPoint();
    return NVME_CONST::INVALID_DBL_VALUE;
}
void CNvmeQueue::WriteCplHead(ULONG value)
{
    if (IsValidQid(QueueID) && NULL != CplDbl)
        return WriteDbl(DevExt, CplDbl, value);
    KdBreakPoint();
}

bool CNvmeQueue::AllocQueueBuffer()
{
    PHYSICAL_ADDRESS low = { 0 }; //I want to allocate it above 4G...
    PHYSICAL_ADDRESS high = { 0 };
    PHYSICAL_ADDRESS align = { 0 };
    
    low.HighPart = 0X000000001;
    high.QuadPart = (LONGLONG)-1;

    //I am too lazy to check if NVMe device request continuous page or not, so.... 
    //Allocate SubQ and CplQ together into a continuous physical memory block.
    ULONG status = StorPortAllocateContiguousMemorySpecifyCacheNode(
        this->DevExt, this->BufferSize,
        low, high, align,
        CNvmeQueue::CacheType, this->NumaNode,
        &this->Buffer);

    //todo: log 
    if(STOR_STATUS_SUCCESS != status)
    {
        KdBreakPoint();
        return false;
    }

    BufferPA = MmGetPhysicalAddress(Buffer);

    return true;
}
bool CNvmeQueue::InitQueueBuffer()
{
    //SubQ and CplQ should be placed on same continuous memory block.
    //1.Calculate total block size. 
    //2.Split total size to SubQ size and CplQ size. 
    //NOTE: both of them should be PAGE_ALIGNED
    this->SubQ_Size = this->Depth * sizeof(NVME_COMMAND);
    this->CplQ_Size = this->Depth * sizeof(NVME_COMPLETION_ENTRY);

    PUCHAR cursor = (PUCHAR) this->Buffer;
    this->SubQ_VA = (PNVME_COMMAND)ROUND_TO_PAGES(cursor);
    cursor += this->SubQ_Size;
    this->CplQ_VA = (PNVME_COMPLETION_ENTRY)ROUND_TO_PAGES(cursor);

    //this->BufferSize = this->SubQ_Size + this->CplQ_Size;
    //Because Align to Page could cause extra waste space in memory.
    //So should check if CplQ exceeds total buffer length...
    if((cursor + this->CplQ_Size) > ((PUCHAR)this->Buffer + this->BufferSize))
        goto ERROR; //todo: log

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
CCmdHistory::CCmdHistory()
{
    //KeInitializeSpinLock(&CmdLock);
}
CCmdHistory::CCmdHistory(class CNvmeQueue* parent, PVOID devext, USHORT depth, ULONG numa_node)
        : CCmdHistory()
{   Setup(parent, devext, depth, numa_node);    }
CCmdHistory::~CCmdHistory()
{
    Teardown();
}
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
void CCmdHistory::Teardown()
{
    //todo: complete all remained SRBEXT

    if(NULL != this->Buffer)
        StorPortFreePool(this->DevExt, this->Buffer);

    this->Buffer = NULL;
}
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
// ===== End NvmeQueue.cpp =====

// ===== Begin StartIo_Handler.cpp =====

UCHAR StartIo_DefaultHandler(PSPCNVME_SRBEXT srbext)
{
    UNREFERENCED_PARAMETER(srbext);
    //SetScsiSenseBySrbStatus(srbext->Srb, SRB_STATUS_INVALID_REQUEST);
    //StorPortNotification(RequestComplete, srbext->DevExt, srbext->Srb);
    return SRB_STATUS_INVALID_REQUEST;
}
UCHAR StartIo_ScsiHandler(PSPCNVME_SRBEXT srbext)
{
    UCHAR opcode = srbext->Cdb()->CDB6GENERIC.OperationCode;
    UCHAR srb_status = SRB_STATUS_ERROR;
    DebugScsiOpCode(opcode);

    switch(opcode)
    {
#if 0
    // 6-byte commands:
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_REZERO_UNIT:
        //case SCSIOP_REWIND:
    case SCSIOP_REQUEST_BLOCK_ADDR:
    case SCSIOP_FORMAT_UNIT:
    case SCSIOP_READ_BLOCK_LIMITS:
    case SCSIOP_REASSIGN_BLOCKS:
        //case SCSIOP_INIT_ELEMENT_STATUS:
    case SCSIOP_SEEK6:
        //case SCSIOP_TRACK_SELECT:
        //case SCSIOP_SLEW_PRINT:
        //case SCSIOP_SET_CAPACITY:
    case SCSIOP_SEEK_BLOCK:
    case SCSIOP_PARTITION:
    case SCSIOP_READ_REVERSE:
    case SCSIOP_WRITE_FILEMARKS:
        //case SCSIOP_FLUSH_BUFFER:
    case SCSIOP_SPACE:
    case SCSIOP_RECOVER_BUF_DATA:
    case SCSIOP_RESERVE_UNIT:
    case SCSIOP_RELEASE_UNIT:
    case SCSIOP_COPY:
    case SCSIOP_ERASE:
    case SCSIOP_START_STOP_UNIT:
        //case SCSIOP_STOP_PRINT:
        //case SCSIOP_LOAD_UNLOAD:
    case SCSIOP_RECEIVE_DIAGNOSTIC:
    case SCSIOP_SEND_DIAGNOSTIC:
    case SCSIOP_MEDIUM_REMOVAL:

    //refer to https://learn.microsoft.com/zh-tw/windows-hardware/test/hlk/testref/1f98eed5-478b-42bc-8c17-ee49a2c63202
    case SCSIOP_REQUEST_SENSE:
        srb_status = Scsi_RequestSense6(srbext);
        break;
#endif
    case SCSIOP_MODE_SELECT:        //cache and other feature options
        srb_status = Scsi_ModeSelect6(srbext);
        break;
    case SCSIOP_READ6:
        //case SCSIOP_RECEIVE:
        srb_status = Scsi_Read6(srbext);
        break;
    case SCSIOP_WRITE6:
        //case SCSIOP_PRINT:
        //case SCSIOP_SEND:
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

        // 10-byte commands
#if 0
    case SCSIOP_READ_FORMATTED_CAPACITY:
    case SCSIOP_SEEK:
        //case SCSIOP_LOCATE:
        //case SCSIOP_POSITION_TO_ELEMENT:
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
        //case SCSIOP_UNMAP:
    case SCSIOP_READ_TOC:
    case SCSIOP_READ_HEADER:
        //case SCSIOP_REPORT_DENSITY_SUPPORT:
    case SCSIOP_PLAY_AUDIO:
    case SCSIOP_GET_CONFIGURATION:
    case SCSIOP_PLAY_AUDIO_MSF:
    case SCSIOP_PLAY_TRACK_INDEX:
        //case SCSIOP_SANITIZE:
    case SCSIOP_PLAY_TRACK_RELATIVE:
    case SCSIOP_GET_EVENT_STATUS:
    case SCSIOP_PAUSE_RESUME:
    case SCSIOP_LOG_SELECT:
    case SCSIOP_LOG_SENSE:
    case SCSIOP_STOP_PLAY_SCAN:
    case SCSIOP_XDWRITE:
    case SCSIOP_XPWRITE:
        //case SCSIOP_READ_DISK_INFORMATION:
        //case SCSIOP_READ_DISC_INFORMATION:
    case SCSIOP_READ_TRACK_INFORMATION:
    case SCSIOP_XDWRITE_READ:
        //case SCSIOP_RESERVE_TRACK_RZONE:
    case SCSIOP_SEND_OPC_INFORMATION:
    case SCSIOP_RESERVE_UNIT10:
        //case SCSIOP_RESERVE_ELEMENT:
    case SCSIOP_RELEASE_UNIT10:
        //case SCSIOP_RELEASE_ELEMENT:
    case SCSIOP_REPAIR_TRACK:
    case SCSIOP_CLOSE_TRACK_SESSION:
    case SCSIOP_READ_BUFFER_CAPACITY:
    case SCSIOP_SEND_CUE_SHEET:
    case SCSIOP_PERSISTENT_RESERVE_IN:
    case SCSIOP_PERSISTENT_RESERVE_OUT:
#endif
    case SCSIOP_MODE_SELECT10:          //cache and other feature options
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

        // 12-byte commands                  
#if 0
    case SCSIOP_BLANK:
        //case SCSIOP_ATA_PASSTHROUGH12:
    case SCSIOP_SEND_EVENT:
        //case SCSIOP_SECURITY_PROTOCOL_IN:
    case SCSIOP_SEND_KEY:
        //case SCSIOP_MAINTENANCE_IN:
    case SCSIOP_REPORT_KEY:
        //case SCSIOP_MAINTENANCE_OUT:
    case SCSIOP_MOVE_MEDIUM:
    case SCSIOP_LOAD_UNLOAD_SLOT:
        //case SCSIOP_EXCHANGE_MEDIUM:
    case SCSIOP_SET_READ_AHEAD:
        //case SCSIOP_MOVE_MEDIUM_ATTACHED:
    case SCSIOP_SERVICE_ACTION_OUT12:
    case SCSIOP_SEND_MESSAGE:
        //case SCSIOP_SERVICE_ACTION_IN12:
    case SCSIOP_GET_PERFORMANCE:
    case SCSIOP_READ_DVD_STRUCTURE:
    case SCSIOP_WRITE_VERIFY12:
    case SCSIOP_SEARCH_DATA_HIGH12:
    case SCSIOP_SEARCH_DATA_EQUAL12:
    case SCSIOP_SEARCH_DATA_LOW12:
    case SCSIOP_SET_LIMITS12:
    case SCSIOP_READ_ELEMENT_STATUS_ATTACHED:
    case SCSIOP_REQUEST_VOL_ELEMENT:
        //case SCSIOP_SECURITY_PROTOCOL_OUT:
    case SCSIOP_SEND_VOLUME_TAG:
        //case SCSIOP_SET_STREAMING:
    case SCSIOP_READ_DEFECT_DATA:
    case SCSIOP_READ_ELEMENT_STATUS:
    case SCSIOP_READ_CD_MSF:
    case SCSIOP_SCAN_CD:
        //case SCSIOP_REDUNDANCY_GROUP_IN:
    case SCSIOP_SET_CD_SPEED:
        //case SCSIOP_REDUNDANCY_GROUP_OUT:
    case SCSIOP_PLAY_CD:
        //case SCSIOP_SPARE_IN:
    case SCSIOP_MECHANISM_STATUS:
        //case SCSIOP_SPARE_OUT:
    case SCSIOP_READ_CD:
        //case SCSIOP_VOLUME_SET_IN:
    case SCSIOP_SEND_DVD_STRUCTURE:
        //case SCSIOP_VOLUME_SET_OUT:
    case SCSIOP_INIT_ELEMENT_RANGE:
#endif
    case SCSIOP_REPORT_LUNS:
        srb_status = Scsi_ReportLuns12(srbext);
        break;
    case SCSIOP_READ12:
        //case SCSIOP_GET_MESSAGE:
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
        // 16-byte commands
#if 0
    case SCSIOP_XDWRITE_EXTENDED16:
        //case SCSIOP_WRITE_FILEMARKS16:
    case SCSIOP_REBUILD16:
        //case SCSIOP_READ_REVERSE16:
    case SCSIOP_REGENERATE16:
    case SCSIOP_EXTENDED_COPY:
        //case SCSIOP_POPULATE_TOKEN:
        //case SCSIOP_WRITE_USING_TOKEN:
    case SCSIOP_RECEIVE_COPY_RESULTS:
        //case SCSIOP_RECEIVE_ROD_TOKEN_INFORMATION:
    case SCSIOP_ATA_PASSTHROUGH16:
    case SCSIOP_ACCESS_CONTROL_IN:
    case SCSIOP_ACCESS_CONTROL_OUT:
    case SCSIOP_COMPARE_AND_WRITE:
    case SCSIOP_READ_ATTRIBUTES:
    case SCSIOP_WRITE_ATTRIBUTES:
    case SCSIOP_WRITE_VERIFY16:
    case SCSIOP_PREFETCH16:
    case SCSIOP_SYNCHRONIZE_CACHE16:
        //case SCSIOP_SPACE16:
    case SCSIOP_LOCK_UNLOCK_CACHE16:
        //case SCSIOP_LOCATE16:
    case SCSIOP_WRITE_SAME16:
        //case SCSIOP_ERASE16:
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
        //case SCSIOP_GET_LBA_STATUS:
        //case SCSIOP_GET_PHYSICAL_ELEMENT_STATUS:
        //case SCSIOP_REMOVE_ELEMENT_AND_TRUNCATE:
        //case SCSIOP_SERVICE_ACTION_IN16:
        srb_status = Scsi_ReadCapacity16(srbext);
        break;

        // 32-byte commands
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
UCHAR StartIo_IoctlHandler(PSPCNVME_SRBEXT srbext)
{

    //SRB_FUNCTION_IO_CONTROL handles serveral kinds (groups) of IOCTL:
    //1. IOCTL_SCSI_MINIPORT_* ioctl codes.
    //2. IOCTL_STORAGE_* ioctl codes
    //3. custom made ioctl codes.
    //4. .....so on.....
    //All of them use SRB_IO_CONTROL as input buffer data. 
    //it is passed-in via Srb->DataBuffer. Because there are many "groups" of 
    //ioctl codes in this handler, so we should identify them by 
    //SrbIoCtrl->Signature field. possible values are:
    //  IOCTL_MINIPORT_SIGNATURE_SCSIDISK           "SCSIDISK"
    //  IOCTL_MINIPORT_SIGNATURE_HYBRDISK           "HYBRDISK"
    //  IOCTL_MINIPORT_SIGNATURE_DSM_NOTIFICATION   "MPDSM   "
    //  IOCTL_MINIPORT_SIGNATURE_DSM_GENERAL        "MPDSMGEN"
    //  IOCTL_MINIPORT_SIGNATURE_FIRMWARE           "FIRMWARE"
    //  IOCTL_MINIPORT_SIGNATURE_QUERY_PROTOCOL     "PROTOCOL"
    //  IOCTL_MINIPORT_SIGNATURE_SET_PROTOCOL       "SETPROTO"
    //  IOCTL_MINIPORT_SIGNATURE_QUERY_TEMPERATURE  "TEMPERAT"
    //  IOCTL_MINIPORT_SIGNATURE_SET_TEMPERATURE_THRESHOLD  "SETTEMPT"
    //  IOCTL_MINIPORT_SIGNATURE_QUERY_PHYSICAL_TOPOLOGY    "TOPOLOGY"
    //  IOCTL_MINIPORT_SIGNATURE_ENDURANCE_INFO     "ENDURINF"
    //** to use custom made ioctl codes, you should also define your own signature for SrbIoCtrl->Signature.
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
// ===== End StartIo_Handler.cpp =====

// ===== Begin BuildIo_Handler.cpp =====

static UCHAR AdapterPnp_QueryCapHandler(PSPCNVME_SRBEXT srbext)
{
    PSTOR_DEVICE_CAPABILITIES_EX cap = (PSTOR_DEVICE_CAPABILITIES_EX)srbext->DataBuf();
    cap->Version = STOR_DEVICE_CAPABILITIES_EX_VERSION_1;
    cap->Size = sizeof(STOR_DEVICE_CAPABILITIES_EX);
    cap->DeviceD1 = 0;
    cap->DeviceD2 = 0;
    cap->LockSupported = 0;
    cap->EjectSupported = 0;
    cap->Removable = 1;         //support RemoveAdapter
    cap->DockDevice = 0;
    cap->UniqueID = 0;
    cap->SilentInstall = 1;     //support silent install on DeviceManager UI
    cap->SurpriseRemovalOK = 1; //support Adapter SurpriseRemove (hot plug)
    cap->NoDisplayInUI = 0;     //should this adapter be shown on DeviceManager UI?
    cap->Address = 0;
    cap->UINumber = 0xFFFFFFFF;
    return SRB_STATUS_SUCCESS;
}

BOOLEAN BuildIo_DefaultHandler(PSPCNVME_SRBEXT srbext)
{
    //srbext->SetStatus(SRB_STATUS_INVALID_REQUEST);
    ////todo: set log 
    //StorPortNotification(RequestComplete, srbext->DevExt, srbext->Srb);
	srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
    return FALSE;
}

BOOLEAN BuildIo_IoctlHandler(PSPCNVME_SRBEXT srbext)
{
    //Handle IOCTL only in StartIo.
    //I don't like to handle IOCTL in DISPATCH_LEVEL...
    UNREFERENCED_PARAMETER(srbext);
    return TRUE;
}

BOOLEAN BuildIo_ScsiHandler(PSPCNVME_SRBEXT srbext)
{
    //todo: set log 
    DebugScsiOpCode(srbext->Cdb()->CDB6GENERIC.OperationCode);
    
    //spcnvme only support 1 disk in current stage.
    //so check path/target/lun here.
    UCHAR path = 0, target = 0, lun = 0;
    SrbGetPathTargetLun(srbext->Srb, &path, &target, &lun);

    if(!(0==path && 0==target && 0==lun))
    {
        srbext->CompleteSrb(SRB_STATUS_INVALID_LUN);
        return FALSE;
    }
    return TRUE;
}

BOOLEAN BuildIo_SrbPowerHandler(PSPCNVME_SRBEXT srbext)
{
	srbext->CompleteSrb(SRB_STATUS_INVALID_REQUEST);
//always return FALSE. This event only handled in BuildIo.
    return FALSE;
}

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

    //All unit control migrated to HwUnitControl callback...
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
        //regular RemoveDevice should shutdown controller first, then delete all queue memory.
            status = srbext->DevExt->ShutdownController();
            if (!NT_SUCCESS(status))
            {
                KdBreakPoint();
                //todo: log
            }
            srbext->DevExt->Teardown();
            srb_status = SRB_STATUS_SUCCESS;
            break;
        case StorSurpriseRemoval:
            //surprise remove doesn't need to shutdown controller.
            //controller is already gone , access controller registers will make BSoD or other problem.
            //srb_status = AdapterPnp_RemoveHandler(srbext);
            srbext->DevExt->Teardown();
            srb_status = SRB_STATUS_SUCCESS;
            break;
    }

END:
    srbext->CompleteSrb(srb_status);
    //always return FALSE. This event only handled in BuildIo.
    return FALSE;
}
// ===== End BuildIo_Handler.cpp =====
