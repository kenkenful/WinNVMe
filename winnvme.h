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

#define MARKER_TAG          0x23939889
#define BSOD_DELETE_ERROR   0x28825252

#define MSG_BUFFER_SIZE     128
#define DEBUG_PREFIX        "SPC ==>"
#define DBG_FILTER          0x00000888

#define CALLINOUT_TAG       (ULONG)'TUOC'
#define DBGMSG_LEVEL        0x00001000
#define KPRINTF(x)          DbgPrintEx(DPFLTR_IHVDRIVER_ID, DBGMSG_LEVEL, x)

#define CPP_TAG             (ULONG)'MPPC'
void* operator new(size_t size, POOL_TYPE type, ULONG tag = CPP_TAG);
void* operator new[](size_t size, POOL_TYPE type, ULONG tag = CPP_TAG);

void DebugSrbFunctionCode(ULONG code);
void DebugScsiOpCode(UCHAR opcode);
bool IsSupportedOS(ULONG major, ULONG minor = 0);

class CDebugCallInOut
{
private:
    static const int BufSize = 64;
    char* Name = NULL;
public:
    CDebugCallInOut(char* name);
    CDebugCallInOut(const char* name);
    ~CDebugCallInOut();
};

class CSpinLock
{
public:
    CSpinLock(KSPIN_LOCK* lock, bool acquire = true);
    ~CSpinLock();

    void DoAcquire();
    void DoRelease();
protected:
    KSPIN_LOCK* Lock = NULL;
    KIRQL OldIrql = PASSIVE_LEVEL;
    bool IsAcquired = false;
};

class CStorSpinLock
{
public:
    CStorSpinLock(PVOID devext, STOR_SPINLOCK reason, PVOID ctx = NULL);
    ~CStorSpinLock();
    inline void Acquire(STOR_SPINLOCK reason, PVOID ctx = NULL)
    {
        StorPortAcquireSpinLock(this->DevExt, reason, ctx, &this->Handle);
    }
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
    template <typename _Ty, POOL_TYPE _PoolType, ULONG _PoolTag>
    class SpcCppDeleter
    {
    public:
        constexpr SpcCppDeleter() noexcept = default;
        SpcCppDeleter(const SpcCppDeleter<_Ty, _PoolType, _PoolTag>&) noexcept {}

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

        CAutoPtr() noexcept
        {
            this->Ptr = nullptr;
        }
        CAutoPtr(DataType* ptr) noexcept
        {
            this->Ptr = ptr;
        }
        CAutoPtr(PVOID ptr) noexcept
        {
            this->Ptr = (DataType*)ptr;
        }

        virtual ~CAutoPtr() noexcept
        {
            Reset();
        }

        CAutoPtr& operator=(CAutoPtr<_Ty, _PoolType, _PoolTag, _Dx>&& _Right) noexcept
        {
            Reset(_Right.Release());
            return *this;
        }

        operator _Ty* () const
        {
            return this->Ptr;
        }

        _Ty* operator->() const noexcept
        {
            return this->Ptr;
        }

        void Set(_Ty* ptr) noexcept
        {
            this->Ptr = ptr;
        }

        _Ty* Get() const noexcept
        {
            return this->Ptr;
        }

        bool IsNull() const noexcept
        {
            return (NULL == this->Ptr);
        }

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

// ===== Begin PoolTags.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


#define TAG_GENBUF      (ULONG)'FUBG'
#define TAG_SRB         (ULONG)' BRS'
#define TAG_SRBEXT      (ULONG)'EBRS'
#define TAG_VPDPAGE     (ULONG)'PDPV'
#define TAG_PRP2        (ULONG)'2PRP'
#define TAG_LOG_PAGE    (ULONG)'PGOL'
#define TAG_REG_BUFFER  (ULONG)'BGER'
#define TAG_ISR_POLL_CPL (ULONG)'LPCC'
// ===== End PoolTags.h =====

// ===== Begin Constants.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


#define NVME_INVALID_ID     ((ULONG)-1)
#define NVME_INVALID_CID    ((USHORT)-1)        //should align to NVME CID size
#define NVME_INVALID_QID    ((USHORT)-1)

#define MAX_LOGIC_UNIT      1
#define MAX_IO_QUEUE_COUNT  64

//#define MAX_TX_PAGES        32
//#define MAX_TX_SIZE         (PAGE_SIZE * MAX_TX_PAGES)
//#define MAX_CONCURRENT_IO   4096

#define ACCESS_RANGE_COUNT  8

//const char* SpcVendorID = "SPC     ";           //vendor name
//const char* SpcProductID = "SomkingPC NVMe  ";  //model name
//const char* SpcProductRev = "0100";

#pragma region  ======== SCSI and SRB ========
#define SRB_FUNCTION_SPC_INTERNAL   0xFF
#define INVALID_PATH_ID      ((UCHAR)~0)
#define INVALID_TARGET_ID    ((UCHAR)~0)
#define INVALID_LUN_ID       ((UCHAR)~0)
#define INVALID_SRB_QUEUETAG ((ULONG)~0)
#pragma endregion

#pragma region  ======== NVME ========
#define DEFAULT_INT_COALESCE_COUNT  10
#define DEFAULT_INT_COALESCE_TIME   2    //in 100us unit
#pragma endregion

#pragma region  ======== REGISTRY ========
#define REGNAME_ADMQ_DEPTH      (UCHAR*)"AdmQDepth"
#define REGNAME_IOQ_DEPTH       (UCHAR*)"IoQDepth"
#define REGNAME_IOQ_COUNT       (UCHAR*)"IoQCount"
#define REGNAME_COALESCE_TIME   (UCHAR*)"IntCoalescingTime"
#define REGNAME_COALESCE_COUNT  (UCHAR*)"IntCoalescingEntries"


#pragma endregion
// ===== End Constants.h =====

// ===== Begin Inline_Utils.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


FORCEINLINE size_t DivRoundUp(size_t value, size_t align_size)
{
    return (size_t)((value + (align_size-1))/align_size);
}

FORCEINLINE size_t RoundUp(size_t value, size_t align_size)
{
    return (DivRoundUp(value, align_size) * align_size);
}

FORCEINLINE bool IsAddrEqual(PPHYSICAL_ADDRESS a, PPHYSICAL_ADDRESS b)
{
    if (a->QuadPart == b->QuadPart)
        return true;
    return false;
}
FORCEINLINE bool IsAddrEqual(PHYSICAL_ADDRESS& a, PHYSICAL_ADDRESS& b)
{
    return IsAddrEqual(&a, &b);
}
// ===== End Inline_Utils.h =====

// ===== Begin NvmeConstants.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


namespace NVME_CONST{
    static const char* VENDOR_ID = "SPC     ";           //vendor name
    static const char* PRODUCT_ID = "SomkingPC NVMe  ";  //model name
    static const char* PRODUCT_REV = "0100";

    static const ULONG TX_PAGES = 512; //PRP1 + PRP2 MAX PAGES
    static const ULONG TX_SIZE = PAGE_SIZE * TX_PAGES;
    static const UCHAR SUPPORT_NAMESPACES = 4;
    static const ULONG UNSPECIFIC_NSID = 0;
    static const ULONG DEFAULT_CTRLID = 1;
    static const UCHAR MAX_TARGETS = 1;
    static const UCHAR MAX_LU = 1;
    static const ULONG MAX_IO_PER_LU = 512;
    static const UCHAR IOSQES = 6; //sizeof(NVME_COMMAND)==64 == 2^6, so IOSQES== 6.
    static const UCHAR IOCQES = 4; //sizeof(NVME_COMPLETION_ENTRY)==16 == 2^4, so IOCQES== 4.
    static const UCHAR ADMIN_QUEUE_DEPTH = 64;  //how many entries does the admin queue have?
    
    static const USHORT CPL_INIT_PHASETAG = 1;  //CompletionQueue phase tag init value;
    static const UCHAR IO_QUEUE_COUNT = 16;
    static const USHORT IO_QUEUE_DEPTH = MAX_IO_PER_LU / 2;
    static const ULONG DEFAULT_MAX_TXSIZE = 131072;
    static const USHORT MAX_CID = MAXUSHORT-1;  //0xFFFF is invalid 
    static const ULONG MAX_NS_COUNT = 1024;     //Max NameSpace count. defined in NVMe spec.
    //static const ULONG MAX_CONCURRENT_IO = 1024;
    //static const USHORT SQ_CMD_SIZE = sizeof(NVME_COMMAND);
    //static const USHORT SQ_CMD_SIZE_SHIFT = 6; //sizeof(NVME_COMMAND) is 64 bytes == 2^6
    static const ULONG CPL_ALL_ENTRY = MAXULONG;
    static const ULONG INIT_DBL_VALUE = 0;
    static const ULONG INVALID_DBL_VALUE = (ULONG)MAXUSHORT;
    static const ULONG ISR_HANDLED_CPL = 8;     //how many cpl entries will be handled in each ISR call?
    static const ULONG MAX_INT_COUNT = 64;
    static const ULONG SAFE_SUBMIT_THRESHOLD = 8;
    static const ULONG STALL_TIME_US = 100;  //in micro-seconds
    static const ULONG SLEEP_TIME_US = STALL_TIME_US*5;    //in micro-seconds

    #pragma region ======== SetFeature default values ========
    static const UCHAR INTCOAL_TIME = 2;   //Interrupt Coalescing time threshold in 100us unit.
    static const UCHAR INTCOAL_THRESHOLD = 8;   //Interrupt Coalescing trigger threshold.

    static const UCHAR AB_BURST = 7;        //Arbitration Burst. 111b(0n7) is Unlimit.
    static const UCHAR AB_HPW = 32 - 1;     //High Priority Weight. it is 0-based so need substract 1.
    static const UCHAR AB_MPW = 16 - 1;     //Medium Priority Weight. it is 0-based so need substract 1.
    static const UCHAR AB_LPW = 8 - 1;      //Low Priority Weight. it is 0-based so need substract 1.

    #pragma endregion
};
// ===== End NvmeConstants.h =====

// ===== Begin NvmeEnums.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


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
//    SRB_CMD = 0x00001000,           //this command use SRB , no wait event. using regular SRB handling
//    WAIT_CMD = 0x00002000,          //this command is internal cmd and waiting for event signal
    SELF_ISSUED = 0x80000000,           //this command issued by SpcNvme.sys myself.
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

//typedef enum _USE_STATE
//{
//    FREE = 0,
//    USED = 1,
//}USE_STATE;

typedef enum _NVME_STATE {
    STOP = 0,
    RUNNING = 1,
    SETUP = 2,
    INIT = 3,       //In InitStage1 and InitStage2
    TEARDOWN = 4,
    RESETCTRL = 5,
    RESETBUS = 6,
    SHUTDOWN = 10,   //CC.SHN==1
}NVME_STATE;

// ===== End NvmeEnums.h =====

// ===== Begin PCIe_Msix.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


#pragma push(1)
#pragma warning(disable:4201)   // nameless struct/union

//MSIX Table data example: (only allocated 3 MSIX interrupt)
//0: kd > dd 0xffffe501eaaea000
//ffffe501`eaaea000  fee0400c 00000000 000049b3 00000000
//ffffe501`eaaea010  fee0800c 00000000 000049b3 00000000
//ffffe501`eaaea020  fee0100c 00000000 00004993 00000000
//ffffe501`eaaea030  fee0400c 00000000 000049b3 00000000
typedef union _MSIX_MSG_ADDR
{
    struct {
        ULONG64 Aligment : 2;
        ULONG64 DestinationMode : 1;
        ULONG64 RedirHint : 1;
        ULONG64 Reserved : 8;
        ULONG64 DestinationID : 8;
        ULONG64 BaseAddr : 12;        //LocalAPIC register phypage, which set in CPU MSR(0x1B)
        ULONG64 Reserved2 : 32;
    };
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
// ===== End PCIe_Msix.h =====

// ===== Begin SrbExt.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


class CNvmeDevice;
struct _SPCNVME_SRBEXT;

typedef VOID SPC_SRBEXT_COMPLETION(struct _SPCNVME_SRBEXT *srbext);
typedef SPC_SRBEXT_COMPLETION* PSPC_SRBEXT_COMPLETION;

typedef struct _SPCNVME_SRBEXT
{
    static _SPCNVME_SRBEXT *GetSrbExt(PSTORAGE_REQUEST_BLOCK srb);
	static _SPCNVME_SRBEXT* InitSrbExt(PVOID devext, PSTORAGE_REQUEST_BLOCK srb);

    CNvmeDevice *DevExt;
    PSTORAGE_REQUEST_BLOCK Srb;
    UCHAR SrbStatus;        //returned SrbStatus for SyncCall of Admin cmd (e.g. IndeitfyController) 
    BOOLEAN InitOK;
    BOOLEAN FreePrp2List;
    NVME_COMMAND NvmeCmd;
    NVME_COMPLETION_ENTRY NvmeCpl;
    PVOID Prp2VA;
    PHYSICAL_ADDRESS Prp2PA;
    PSPC_SRBEXT_COMPLETION CompletionCB;
    //ExtBuf is used to retrieve data by cmd. e.g. LogPage Buffer in GetLogPage().
    //It should be freed in CompletionCB.
    PVOID ExtBuf;      
    
    #pragma region ======== for Debugging ========
    class CNvmeQueue *SubmittedQ;
    ULONG IoQueueIndex;
    PNVME_COMMAND SubmittedCmd;
    ULONG Tag;
    ULONG SubIndex;
    #pragma endregion

    void Init(PVOID devext, STORAGE_REQUEST_BLOCK *srb);
    void CleanUp();
    void CompleteSrb(UCHAR status);
    void CompleteSrb(NVME_COMMAND_STATUS& nvme_status);
    ULONG FuncCode();         //SRB Function Code
    ULONG ScsiQTag();
    PCDB Cdb();
    UCHAR CdbLen();
    UCHAR PathID();           //SCSI Path (bus) ID
    UCHAR TargetID();         //SCSI Device ID
    UCHAR Lun();              //SCSI Logical UNit ID
    PVOID DataBuf();
    ULONG DataBufLen();
    void SetTransferLength(ULONG length);
    void ResetExtBuf(PVOID new_buffer = NULL);
    PSRBEX_DATA_PNP SrbDataPnp();
}SPCNVME_SRBEXT, * PSPCNVME_SRBEXT;

UCHAR NvmeToSrbStatus(NVME_COMMAND_STATUS& status);
UCHAR NvmeGenericToSrbStatus(NVME_COMMAND_STATUS &status);
UCHAR NvmeCmdSpecificToSrbStatus(NVME_COMMAND_STATUS &status);
UCHAR NvmeMediaErrorToSrbStatus(NVME_COMMAND_STATUS &status);

void SetScsiSenseBySrbStatus(PSTORAGE_REQUEST_BLOCK srb, UCHAR &status);
// ===== End SrbExt.h =====

// ===== Begin NvmeQueue.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


typedef struct _QUEUE_PAIR_CONFIG {
    PVOID DevExt = NULL;
    USHORT QID = 0;         //QueueID is zero-based. ID==0 is assigned to AdminQueue constantly
    USHORT Depth = 0;
    USHORT HistoryDepth = 0;
    ULONG NumaNode = MM_ANY_NODE_OK;
    QUEUE_TYPE Type = QUEUE_TYPE::IO_QUEUE;
    PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL SubDbl = NULL;
    PNVME_COMPLETION_QUEUE_HEAD_DOORBELL CplDbl = NULL;
    PVOID PreAllocBuffer = NULL;            //SubQ and CplQ should be continuous memory together
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
    const ULONG BufferTag = (ULONG)'HBRS';

    CCmdHistory();
    CCmdHistory(class CNvmeQueue*parent, PVOID devext, USHORT depth, ULONG numa_node = 0);
    ~CCmdHistory();

    NTSTATUS Setup(class CNvmeQueue*parent, PVOID devext, USHORT depth, ULONG numa_node = 0);
    void Teardown();
    void Reset();
    NTSTATUS Push(ULONG index, PSPCNVME_SRBEXT srbext);
    NTSTATUS Pop(ULONG index, PSPCNVME_SRBEXT &srbext);

private:
    PVOID DevExt = NULL;
    PVOID Buffer = NULL;
    size_t BufferSize = 0;
    PSPCNVME_SRBEXT *History = NULL; //Cast of RawBuffer
    ULONG Depth = 0;                   //how many items in this->History ?
    USHORT QueueID = NVME_INVALID_QID;
    ULONG NumaNode = MM_ANY_NODE_OK;

    //KSPIN_LOCK CmdLock;
    class CNvmeQueue* Parent = NULL;
};

class CNvmeQueue
{
public:
    const static ULONG BufferTag = (ULONG)'QMVN';
    const static MEMORY_CACHING_TYPE CacheType = MEMORY_CACHING_TYPE::MmNonCached;
    static VOID QueueCplDpcRoutine(
        _In_ PSTOR_DPC dpc,
        _In_ PVOID devext,
        _In_opt_ PVOID sysarg1,
        _In_opt_ PVOID sysarg2
    );
    
    CNvmeQueue();
    CNvmeQueue(QUEUE_PAIR_CONFIG* config);
    ~CNvmeQueue();

    NTSTATUS Setup(QUEUE_PAIR_CONFIG* config);
    void Teardown();

    inline bool IsInitOK(){return this->IsReady;}
    
    NTSTATUS SubmitCmd(SPCNVME_SRBEXT* srbext, PNVME_COMMAND src_cmd);
    void CompleteCmd(ULONG max_count = 0);
    void ResetAllCmd();
    void GetQueueAddr(PVOID* subva, PHYSICAL_ADDRESS* subpa, PVOID* cplva, PHYSICAL_ADDRESS* cplpa);
    void GetQueueAddr(PVOID *subq, PVOID* cplq);
    void GetQueueAddr(PHYSICAL_ADDRESS* subq, PHYSICAL_ADDRESS* cplq);
    void GetSubQAddr(PHYSICAL_ADDRESS* subq);
    void GetCplQAddr(PHYSICAL_ADDRESS* cplq);

    STOR_DPC QueueCplDpc;
    PVOID DevExt = NULL;
    USHORT QueueID = NVME_INVALID_QID;  //1-based ID, 0 is reserved for AdminQ
    USHORT Depth = 0;       //how many entries in both SubQ and CplQ?
    ULONG NumaNode = MM_ANY_NODE_OK;
    bool IsReady = false;
    bool UseExtBuffer = false; //Is this Queue use "external allocated buffer" ?
    //In CNvmeQueuePair, it allocates SubQ and CplQ in one large continuous block.
    //QueueBuffer is pointer of this large block.
    //Then divide into 2 blocks for SubQ and CplQ.
    PVOID Buffer = NULL;
    PHYSICAL_ADDRESS BufferPA = {0};
    size_t BufferSize = 0;      //total size of entire queue buffer, BufferSize >= (SubQ_Size + CplQ_Size)
    volatile LONG InflightCmds = 0;
    PNVME_COMMAND SubQ_VA = NULL;       //Virtual address of SubQ Buffer.
    PHYSICAL_ADDRESS SubQ_PA = { 0 }; 
    size_t SubQ_Size = 0;       //total length of SubQ Buffer.

    PNVME_COMPLETION_ENTRY CplQ_VA = NULL;       //Virtual address of CplQ Buffer.
    PHYSICAL_ADDRESS CplQ_PA = { 0 }; 
    size_t CplQ_Size = 0;       //total length of CplQ Buffer.

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
    //bool IsDoingCpl;

    bool IsSafeForSubmit();
    ULONG ReadSubTail();
    void WriteSubTail(ULONG value);
    ULONG ReadCplHead();
    void WriteCplHead(ULONG value);
    bool InitQueueBuffer();    //init contents of this queue
    bool AllocQueueBuffer();    //allocate memory of this queue
    void DeallocQueueBuffer();
};
// ===== End NvmeQueue.h =====

// ===== Begin NvmeDevice.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


typedef struct _DOORBELL_PAIR{
    NVME_SUBMISSION_QUEUE_TAIL_DOORBELL SubTail;
    NVME_COMPLETION_QUEUE_HEAD_DOORBELL CplHead;
}DOORBELL_PAIR, *PDOORBELL_PAIR;

//HW_MESSAGE_SIGNALED_INTERRUPT_ROUTINE NvmeMsixISR;

//Using this class represents the DeviceExtension.
//Because memory allocation in kernel is still C style,
//the constructor and destructor are useless. They won't be called.
//So using Setup() and Teardown() to replace them.
class CNvmeDevice {
public:
    static const ULONG BUGCHECK_BASE = 0x23939889;          //pizzahut....  XD
    static const ULONG BUGCHECK_ADAPTER = BUGCHECK_BASE + 1;            //adapter has some problem. e.g. CSTS.CFS==1
    static const ULONG BUGCHECK_INVALID_STATE = BUGCHECK_BASE + 2;      //if action in invalid controller state, fire this bugcheck
    static const ULONG BUGCHECK_NOT_IMPLEMENTED = BUGCHECK_BASE + 10;
    static const ULONG DEV_POOL_TAG = (ULONG) 'veDN';
    static BOOLEAN NvmeMsixISR(IN PVOID devext, IN ULONG msgid);
    static void RestartAdapterDpc(
            IN PSTOR_DPC  Dpc,
            IN PVOID  DevExt,
            IN PVOID  Arg1,
            IN PVOID  Arg2);
    static void RestartAdapterWorker(
        _In_ PVOID DevExt,
        _In_ PVOID Context,
        _In_ PVOID Worker);
public:
    NTSTATUS Setup(PPORT_CONFIGURATION_INFORMATION pci);
    void Teardown();

    NTSTATUS EnableController();
    NTSTATUS DisableController();
    NTSTATUS ShutdownController();  //set CC.SHN and wait CSTS.SHST==2

    NTSTATUS InitController();
    NTSTATUS InitNvmeStage1();      //InitNvmeStage1() should be called AFTER HwFindAdapte because it need interrupt.
    NTSTATUS InitNvmeStage2();      //InitNvmeStage1() should be called AFTER HwFindAdapte because it need interrupt.
    NTSTATUS RestartController();   //for AdapterControl's ScsiRestartAdaptor

    NTSTATUS RegisterIoQueues(PSPCNVME_SRBEXT srbext);
    NTSTATUS UnregisterIoQueues(PSPCNVME_SRBEXT srbext);

    //NTSTATUS InitIdentifyCtrl();
    NTSTATUS IdentifyAllNamespaces();
    NTSTATUS IdentifyFirstNamespace();
    NTSTATUS CreateIoQueues(bool force = false);    //if(force) => delete exist queue objects and recreate again.

    NTSTATUS IdentifyController(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA ident, bool poll = false);
    NTSTATUS IdentifyNamespace(PSPCNVME_SRBEXT srbext, ULONG nsid, PNVME_IDENTIFY_NAMESPACE_DATA data);
    //nsid_list : variable to store query result. It's size should be PAGE_SIZE.(NVMe max support 1024 NameSpace)
    NTSTATUS IdentifyActiveNamespaceIdList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, ULONG &ret_count);

    NTSTATUS SetNumberOfIoQueue(USHORT count);  //tell NVMe device: I want xx i/o queues. then device reply: I can allow you use xxxx queues.
    NTSTATUS SetInterruptCoalescing();
    NTSTATUS SetAsyncEvent();
    NTSTATUS SetArbitration();
    NTSTATUS SetSyncHostTime();
    NTSTATUS SetPowerManagement();
    NTSTATUS GetLbaFormat(ULONG nsid, NVME_LBA_FORMAT &format);
    NTSTATUS GetNamespaceBlockSize(ULONG nsid, ULONG& size);    //get LBA block size in Bytes
    NTSTATUS GetNamespaceTotalBlocks(ULONG nsid, ULONG64& blocks);    //get LBA total block count of specified namespace.
    NTSTATUS SubmitAdmCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd);
    NTSTATUS SubmitIoCmd(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd);
    void ResetOutstandingCmds();
    NTSTATUS SetPerfOpts();
    bool IsInValidIoRange(ULONG nsid, ULONG64 offset, ULONG len);

    bool IsWorking();
    bool IsSetup();
    bool IsTeardown();
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

    ULONG DeviceTimeout;        //should be updated by CAP, unit in micro-seconds
    ULONG StallDelay;

    ACCESS_RANGE AccessRanges[ACCESS_RANGE_COUNT];         //AccessRange from miniport HwFindAdapter.
    ULONG AccessRangeCount;
    ULONG Bar0Size;
    UCHAR MaxNamespaces;
    USHORT IoDepth;
    USHORT AdmDepth;
    ULONG TotalNumaNodes;
    ULONG NamespaceCount;       //how many namespace active in current device?
    
    UCHAR CoalescingThreshold;  //how many interrupt should be coalesced into one interrupt?
    UCHAR CoalescingTime;       //how long(time) should interrupts be coalesced?

    USHORT  VendorID;
    USHORT  DeviceID;
    ULONG   CpuCount;

    NVME_VERSION                        NvmeVer;
    NVME_CONTROLLER_CAPABILITIES        CtrlCap;
    NVME_IDENTIFY_CONTROLLER_DATA       CtrlIdent;
    NVME_IDENTIFY_NAMESPACE_DATA        NsData[NVME_CONST::SUPPORT_NAMESPACES];
    
    //these 2 DPC and WorkItem are used for HwAdapterControl::ScsiRestartAdapter event.
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

    //Following are huge data.
    //for more convenient windbg debugging, I put them on tail of class data.
    PCI_COMMON_CONFIG                   PciCfg;

    void InitVars();
    void LoadRegistry();

    NTSTATUS CreateAdmQ();
    NTSTATUS RegisterAdmQ();
    NTSTATUS UnregisterAdmQ();
    NTSTATUS DeleteAdmQ();

    NTSTATUS CreateIoQ();   //create all IO queue
    NTSTATUS DeleteIoQ();   //delete all IO queue

    void ReadCtrlCap();      //load capability and informations AFTER register address mapped.
    bool MapCtrlRegisters();
    //bool GetMsixTable();
    bool GetPciBusData(INTERFACE_TYPE type, ULONG bus, ULONG slot);

    bool WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy);
    bool WaitForCtrlerState(ULONG time_us, BOOLEAN csts_rdy, BOOLEAN cc_en);
    bool WaitForCtrlerShst(ULONG time_us);

    void ReadNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier = true);
    void ReadNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier = true);
    void ReadNvmeRegister(NVME_VERSION &ver, bool barrier = true);
    void ReadNvmeRegister(NVME_CONTROLLER_CAPABILITIES &cap, bool barrier = true);
    void ReadNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES& aqa,
                        NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS& asq,
                        NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS& acq,
                        bool barrier = true);

    void WriteNvmeRegister(NVME_CONTROLLER_CONFIGURATION& cc, bool barrier = true);
    void WriteNvmeRegister(NVME_CONTROLLER_STATUS& csts, bool barrier = true);
    void WriteNvmeRegister(NVME_ADMIN_QUEUE_ATTRIBUTES &aqa, 
                        NVME_ADMIN_SUBMISSION_QUEUE_BASE_ADDRESS &asq, 
                        NVME_ADMIN_COMPLETION_QUEUE_BASE_ADDRESS &acq, 
                        bool barrier = true);
    
    void GetAdmQueueDbl(PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL &sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL &cpl);
    void GetQueueDbl(ULONG qid, PNVME_SUBMISSION_QUEUE_TAIL_DOORBELL& sub, PNVME_COMPLETION_QUEUE_HEAD_DOORBELL& cpl);

    BOOLEAN IsControllerEnabled(bool barrier = true);
    BOOLEAN IsControllerReady(bool barrier = true);
    void UpdateMaxTxSize();
    //void DoQueueCompletion(CNvmeQueue* queue);
};

// ===== End NvmeDevice.h =====

// ===== Begin MiniportFunctions.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


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


SCSI_ADAPTER_CONTROL_STATUS Handle_QuerySupportedControlTypes(
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list);
SCSI_ADAPTER_CONTROL_STATUS Handle_RestartAdapter(CNvmeDevice* devext);
// ===== End MiniportFunctions.h =====

// ===== Begin NvmePrpBuilder.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


bool BuildPrp(PSPCNVME_SRBEXT srbext, PNVME_COMMAND cmd, PVOID buffer, size_t buf_size);
// ===== End NvmePrpBuilder.h =====

// ===== Begin NvmeCmdBuilder.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


void BuiildCmd_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG blocks, bool is_write);
void BuildCmd_IdentCtrler(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_CONTROLLER_DATA data);
void BuildCmd_IdentActiveNsidList(PSPCNVME_SRBEXT srbext, PVOID nsid_list, size_t list_size);
void BuildCmd_IdentSpecifiedNS(PSPCNVME_SRBEXT srbext, PNVME_IDENTIFY_NAMESPACE_DATA data, ULONG nsid);
void BuildCmd_IdentAllNSList(PSPCNVME_SRBEXT srbext, PVOID ns_buf, size_t buf_size);
void BuildCmd_SetIoQueueCount(PSPCNVME_SRBEXT srbext, USHORT count);

void BuildCmd_RegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
void BuildCmd_RegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
void BuildCmd_UnRegIoSubQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);
void BuildCmd_UnRegIoCplQ(PSPCNVME_SRBEXT srbext, CNvmeQueue* queue);

void BuildCmd_InterruptCoalescing(PSPCNVME_SRBEXT srbext, UCHAR threshold, UCHAR interval);
void BuildCmd_SetArbitration(PSPCNVME_SRBEXT srbext);

void BuildCmd_GetFirmwareSlotsInfo(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info);
void BuildCmd_GetFirmwareSlotsInfoV1(PSPCNVME_SRBEXT srbext, PNVME_FIRMWARE_SLOT_INFO_LOG info);

void BuildCmd_AdminSecuritySend(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb);
void BuildCmd_AdminSecurityRecv(PSPCNVME_SRBEXT srbext, ULONG nsid, PCDB cdb);

// ===== End NvmeCmdBuilder.h =====

// ===== Begin BuildIo_Handlers.h =====

BOOLEAN BuildIo_DefaultHandler(PSPCNVME_SRBEXT srbext);
BOOLEAN BuildIo_IoctlHandler(PSPCNVME_SRBEXT srbext);
BOOLEAN BuildIo_ScsiHandler(PSPCNVME_SRBEXT srbext);
BOOLEAN BuildIo_SrbPowerHandler(PSPCNVME_SRBEXT srbext);
BOOLEAN BuildIo_SrbPnpHandler(PSPCNVME_SRBEXT srbext);

// ===== End BuildIo_Handlers.h =====

// ===== Begin StartIo_Handler.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


UCHAR StartIo_DefaultHandler(PSPCNVME_SRBEXT srbext);
UCHAR StartIo_ScsiHandler(PSPCNVME_SRBEXT srbext);
UCHAR StartIo_IoctlHandler(PSPCNVME_SRBEXT srbext);
// ===== End StartIo_Handler.h =====

// ===== Begin Scsi_Utils.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


UCHAR Scsi_ReadWrite(PSPCNVME_SRBEXT srbext, ULONG64 offset, ULONG len, bool is_write);
UCHAR NvmeStatus2SrbStatus(NVME_COMMAND_STATUS* status);

UCHAR Scsi_RequestSense6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Read6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Write6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Inquiry6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Verify6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ModeSelect6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ModeSense6(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_TestUnitReady(PSPCNVME_SRBEXT srbext);

UCHAR Scsi_Read10(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Write10(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ReadCapacity10(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Verify10(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ModeSelect10(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ModeSense10(PSPCNVME_SRBEXT srbext);

UCHAR Scsi_ReportLuns12(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Read12(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Write12(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Verify12(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_SecurityProtocolIn(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_SecurityProtocolOut(PSPCNVME_SRBEXT srbext);

UCHAR Scsi_Read16(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Write16(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_Verify16(PSPCNVME_SRBEXT srbext);
UCHAR Scsi_ReadCapacity16(PSPCNVME_SRBEXT srbext);
// ===== End Scsi_Utils.h =====

// ===== Begin ScsiHandler_InlineUtils.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


inline ULONG CopyToCdbBuffer(PUCHAR& buffer, ULONG& buf_size, PVOID page, ULONG page_size, ULONG &ret_size)
{
    ULONG copied_size = 0;
    copied_size = page_size;
    if (copied_size > buf_size)
        copied_size = buf_size;

    RtlCopyMemory(buffer, page, copied_size);
    buf_size -= copied_size;  //calculate "how many bytes left for my pending copy"?
    buffer += copied_size;
    ret_size += copied_size;
    return copied_size;
}
inline void FillParamHeader(PMODE_PARAMETER_HEADER header)
{
//In a complete page return buffer, the header->ModeDataLength should be
// => sizeof(followed page data) + sizeof(header) - sizeof(header->ModeDataLength).
//Here are set header->ModeDataLength to sizeof(MODE_PARAMETER_HEADER) - sizeof(header->ModeDataLength).
//In following procedure we will set it to header->ModeDataLength += page_size;
    header->ModeDataLength = sizeof(MODE_PARAMETER_HEADER) - sizeof(header->ModeDataLength);
    header->MediumType = DIRECT_ACCESS_DEVICE;
    header->DeviceSpecificParameter = 0;
    header->BlockDescriptorLength = 0;  //I don't want reply BlockDescriptor  :p
}
inline void FillParamHeader10(PMODE_PARAMETER_HEADER10 header)
{
    USHORT data_size = sizeof(MODE_PARAMETER_HEADER10);
    USHORT mode_data_size = data_size - sizeof(header->ModeDataLength);
    REVERSE_BYTES_2(header->ModeDataLength, &mode_data_size);
    header->MediumType = DIRECT_ACCESS_DEVICE;
    header->DeviceSpecificParameter = 0;
    //I don't want reply BlockDescriptor, so dont set BlockDescriptorLength field  :p
}
inline void FillModePage_Caching(CNvmeDevice* devext, PMODE_CACHING_PAGE page)
{
    page->PageCode = MODE_PAGE_CACHING;
    page->PageLength = (UCHAR)(sizeof(MODE_CACHING_PAGE) - 2); //sizeof(MODE_CACHING_PAGE) - sizeof(page->PageCode) - sizeof(page->PageLength)
    page->ReadDisableCache = !devext->ReadCacheEnabled;
    page->WriteCacheEnable = devext->WriteCacheEnabled;
    page->PageSavable = TRUE;
}
inline void FillModePage_InfoException(PMODE_INFO_EXCEPTIONS page)
{
    page->PageCode = MODE_PAGE_FAULT_REPORTING;
    page->PageLength = (UCHAR)(sizeof(MODE_INFO_EXCEPTIONS) - 2); //sizeof(MODE_INFO_EXCEPTIONS) - sizeof(page->PageCode) - sizeof(page->PageLength)
    page->ReportMethod = 5;  //Generate no sense
}
inline void FillModePage_Control(PMODE_CONTROL_PAGE page)
{
    //all fields of MODE_CONTROL_PAGE refer to Seagate SCSI reference "Control Mode page (table 302)" 
    page->PageCode = MODE_PAGE_CONTROL;
    page->PageLength = (UCHAR)(sizeof(MODE_CONTROL_PAGE) - 2); //sizeof(MODE_CONTROL_PAGE) - sizeof(page->PageCode) - sizeof(page->PageLength)
    page->QueueAlgorithmModifier = 0;
}
inline ULONG ReplyModePageCaching(CNvmeDevice* devext, PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_CACHING_PAGE page = { 0 };
    ULONG page_size = sizeof(MODE_CACHING_PAGE);
    FillModePage_Caching(devext, &page);

    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}
inline ULONG ReplyModePageControl(PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_CONTROL_PAGE page = { 0 };
    ULONG page_size = sizeof(MODE_CONTROL_PAGE);
    FillModePage_Control(&page);
    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}
inline ULONG ReplyModePageInfoExceptionCtrl(PUCHAR& buffer, ULONG& buf_size, ULONG& ret_size)
{
    MODE_INFO_EXCEPTIONS page = { 0 };
    ULONG page_size = sizeof(MODE_INFO_EXCEPTIONS);
    FillModePage_InfoException(&page);
    return CopyToCdbBuffer(buffer, buf_size, &page, page_size, ret_size);
}

#pragma region ======== Parse SCSI ReadWrite length and offset ======== 
//Note: In SCSI, all read/write request are in "BLOCKS", not in bytes.
inline void ParseReadWriteOffsetAndLen(CDB::_CDB6READWRITE &rw, ULONG64 &offset, ULONG &len)
{
    offset = (rw.LogicalBlockMsb1 << 16) | (rw.LogicalBlockMsb0 << 8) | rw.LogicalBlockLsb;
    len = rw.TransferBlocks;
    if(0 == len)
        len = 256;
}
inline void ParseReadWriteOffsetAndLen(CDB::_CDB10& rw, ULONG64& offset, ULONG &len)
{
    offset = 0;
    len = 0;
    REVERSE_BYTES_4(&offset, &rw.LogicalBlockByte0);
    REVERSE_BYTES_2(&len, &rw.TransferBlocksMsb);
}
inline void ParseReadWriteOffsetAndLen(CDB::_CDB12& rw, ULONG64& offset, ULONG &len)
{
    offset = 0;
    len = 0;
    REVERSE_BYTES_4(&offset, &rw.LogicalBlock);
    REVERSE_BYTES_4(&len, &rw.TransferLength);
}
inline void ParseReadWriteOffsetAndLen(CDB::_CDB16& rw, ULONG64& offset, ULONG &len)
{
    REVERSE_BYTES_8(&offset, &rw.LogicalBlock);
    REVERSE_BYTES_4(&len, &rw.TransferLength);
}
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
// ===== End ScsiHandler_InlineUtils.h =====

// ===== Begin IoctlScsiMiniport_Handlers.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


UCHAR IoctlScsiMiniport_Firmware(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl);
// ===== End IoctlScsiMiniport_Handlers.h =====

// ===== Begin Ioctl_FirmwareFunctions.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================


UCHAR Firmware_GetInfo(PSPCNVME_SRBEXT srbext);
UCHAR Firmware_DownloadToAdapter(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request);
UCHAR Firmware_ActivateSlot(PSPCNVME_SRBEXT srbext, PSRB_IO_CONTROL ioctl, PFIRMWARE_REQUEST_BLOCK request);
// ===== End Ioctl_FirmwareFunctions.h =====

// ===== Begin LicenseTemplate.h =====
// ================================================================
// SpcNvme : OpenSource NVMe Driver for Windows 8+
// Author : Roy Wang(SmokingPC).
// Licensed by MIT License.
// 
// Copyright (C) 2022, Roy Wang (SmokingPC)
// https://github.com/smokingpc/
// 
// NVMe Spec: https://nvmexpress.org/specifications/
// Contact Me : smokingpc@gmail.com
// ================================================================
// Permission is hereby granted, free of charge, to any person obtaining a 
// copy of this softwareand associated documentation files(the "Software"), 
// to deal in the Software without restriction, including without limitation 
// the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and /or sell copies of the Software, and to permit persons to whom the 
// Software is furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in 
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
// ================================================================
// [Additional Statement]
// This Driver is implemented by NVMe Spec 1.3 and Windows Storport Miniport Driver.
// You can copy, modify, redistribute the source code. 
// 
// There is only one requirement to use this source code:
// PLEASE DO NOT remove or modify the "original author" of this codes.
// Keep "original author" declaration unmodified.
// 
// Enjoy it.
// ================================================================

// ===== End LicenseTemplate.h =====
