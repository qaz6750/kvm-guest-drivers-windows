/*
 * Restricted DMA Pool Driver - Private Header
 *
 * Copyright (c) 2024
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>

#define RDMAPOOL_POOL_TAG 'PMDR'  /* 'RDMP' reversed */
#define RDMAPOOL_DEVICE_NAME L"\\Device\\RDMAPool"

/* Device context for the rdmapool driver */
typedef struct _RDMAPOOL_DEVICE_CONTEXT {
    /* Pool physical memory region */
    PHYSICAL_ADDRESS PoolBasePA;    /* Physical base address of DMA pool */
    PVOID PoolBaseVA;               /* Kernel VA from MmMapIoSpaceEx */
    SIZE_T PoolSize;                /* Total pool size in bytes */
    ULONG TotalPages;               /* PoolSize / PAGE_SIZE */

    /* Bitmap allocator */
    RTL_BITMAP AllocationBitmap;    /* Tracks which pages are allocated */
    PULONG BitmapBuffer;            /* Backing store for bitmap */
    PULONG AllocationSizes;         /* Per-start-page: number of pages in allocation */

    /* Synchronization */
    WDFSPINLOCK PoolLock;

    /* Configuration source */
    BOOLEAN AcpiEnumerated;         /* TRUE if loaded via ACPI enumeration */
} RDMAPOOL_DEVICE_CONTEXT, *PRDMAPOOL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RDMAPOOL_DEVICE_CONTEXT, RdmaPoolGetDeviceContext)

/* Pool management functions */
NTSTATUS RdmaPoolInitialize(PRDMAPOOL_DEVICE_CONTEXT Context);
VOID RdmaPoolDestroy(PRDMAPOOL_DEVICE_CONTEXT Context);
PVOID RdmaPoolAllocate(PRDMAPOOL_DEVICE_CONTEXT Context, SIZE_T Size,
                       PPHYSICAL_ADDRESS PhysicalAddress, PSIZE_T AllocatedSize);
NTSTATUS RdmaPoolFree(PRDMAPOOL_DEVICE_CONTEXT Context, PVOID VirtualAddress);

/* Configuration reading */
NTSTATUS RdmaPoolReadAcpiConfig(WDFDEVICE Device, PPHYSICAL_ADDRESS Base, PSIZE_T Size);
NTSTATUS RdmaPoolReadRegistryConfig(WDFDEVICE Device, PPHYSICAL_ADDRESS Base, PSIZE_T Size);

/* IOCTL handlers */
VOID RdmaPoolEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                                size_t OutputBufferLength, size_t InputBufferLength,
                                ULONG IoControlCode);

/* PnP callbacks */
NTSTATUS RdmaPoolEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit);
NTSTATUS RdmaPoolEvtDevicePrepareHardware(WDFDEVICE Device,
                                          WDFCMRESLIST ResourcesRaw,
                                          WDFCMRESLIST ResourcesTranslated);
NTSTATUS RdmaPoolEvtDeviceReleaseHardware(WDFDEVICE Device,
                                          WDFCMRESLIST ResourcesTranslated);
NTSTATUS RdmaPoolEvtDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState);
NTSTATUS RdmaPoolEvtDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState);
