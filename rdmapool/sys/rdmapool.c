/*
 * Restricted DMA Pool Driver - Main Implementation
 *
 * This driver manages a restricted DMA memory pool for protected VMs.
 * It maps a reserved physical memory region and provides a page-granularity
 * allocator accessible via IOCTLs for other kernel drivers (e.g., VirtIOLib).
 *
 * The pool base address and size are obtained from:
 *   1. ACPI namespace: \_SB_.RDPA (Address) and \_SB_.RDPS (Size)
 *   2. Registry fallback: HKLM\SYSTEM\CCS\Services\rdmapool\Parameters
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
#include "rdmapool.h"
#include "rdmapool_interface.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, RdmaPoolEvtDeviceAdd)
#pragma alloc_text(PAGE, RdmaPoolEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, RdmaPoolEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, RdmaPoolEvtDeviceD0Entry)
#pragma alloc_text(PAGE, RdmaPoolEvtDeviceD0Exit)
#pragma alloc_text(PAGE, RdmaPoolReadAcpiConfig)
#pragma alloc_text(PAGE, RdmaPoolReadRegistryConfig)
#endif

/* ========================================================================
 * Pool Allocator Implementation
 * ======================================================================== */

NTSTATUS RdmaPoolInitialize(PRDMAPOOL_DEVICE_CONTEXT Context)
{
    ULONG bitmapBytes;
    ULONG allocMapBytes;

    if (!Context->PoolBasePA.QuadPart || !Context->PoolSize) {
        return STATUS_INVALID_PARAMETER;
    }

    Context->TotalPages = (ULONG)(Context->PoolSize / PAGE_SIZE);
    if (Context->TotalPages == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Map the physical DMA pool region to kernel virtual address space */
    Context->PoolBaseVA = MmMapIoSpaceEx(Context->PoolBasePA, Context->PoolSize,
                                         PAGE_READWRITE | PAGE_NOCACHE);
    if (!Context->PoolBaseVA) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Failed to map physical region %I64X size %IX\n",
                   Context->PoolBasePA.QuadPart, Context->PoolSize));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Zero-initialize the pool */
    RtlZeroMemory(Context->PoolBaseVA, Context->PoolSize);

    /* Allocate bitmap buffer (1 bit per page) */
    bitmapBytes = (Context->TotalPages + 31) / 32 * sizeof(ULONG);
    Context->BitmapBuffer = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                    bitmapBytes, RDMAPOOL_POOL_TAG);
    if (!Context->BitmapBuffer) {
        MmUnmapIoSpace(Context->PoolBaseVA, Context->PoolSize);
        Context->PoolBaseVA = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Context->BitmapBuffer, bitmapBytes);
    RtlInitializeBitMap(&Context->AllocationBitmap, Context->BitmapBuffer, Context->TotalPages);

    /* Allocate per-page allocation size tracker */
    allocMapBytes = Context->TotalPages * sizeof(ULONG);
    Context->AllocationSizes = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                       allocMapBytes, RDMAPOOL_POOL_TAG);
    if (!Context->AllocationSizes) {
        ExFreePoolWithTag(Context->BitmapBuffer, RDMAPOOL_POOL_TAG);
        Context->BitmapBuffer = NULL;
        MmUnmapIoSpace(Context->PoolBaseVA, Context->PoolSize);
        Context->PoolBaseVA = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Context->AllocationSizes, allocMapBytes);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: Initialized pool at PA %I64X VA %p size %IX (%u pages)\n",
               Context->PoolBasePA.QuadPart, Context->PoolBaseVA,
               Context->PoolSize, Context->TotalPages));

    return STATUS_SUCCESS;
}

VOID RdmaPoolDestroy(PRDMAPOOL_DEVICE_CONTEXT Context)
{
    if (Context->AllocationSizes) {
        ExFreePoolWithTag(Context->AllocationSizes, RDMAPOOL_POOL_TAG);
        Context->AllocationSizes = NULL;
    }
    if (Context->BitmapBuffer) {
        ExFreePoolWithTag(Context->BitmapBuffer, RDMAPOOL_POOL_TAG);
        Context->BitmapBuffer = NULL;
    }
    if (Context->PoolBaseVA) {
        MmUnmapIoSpace(Context->PoolBaseVA, Context->PoolSize);
        Context->PoolBaseVA = NULL;
    }
}

PVOID RdmaPoolAllocate(PRDMAPOOL_DEVICE_CONTEXT Context, SIZE_T Size,
                       PPHYSICAL_ADDRESS PhysicalAddress, PSIZE_T AllocatedSize)
{
    ULONG numPages;
    ULONG startPage;
    SIZE_T offset;
    PVOID va;

    if (!Size || !Context->PoolBaseVA) {
        return NULL;
    }

    /* Round up to page boundary */
    numPages = (ULONG)((Size + PAGE_SIZE - 1) / PAGE_SIZE);

    WdfSpinLockAcquire(Context->PoolLock);

    /* Find contiguous free pages */
    startPage = RtlFindClearBits(&Context->AllocationBitmap, numPages, 0);
    if (startPage == 0xFFFFFFFF || startPage + numPages > Context->TotalPages) {
        WdfSpinLockRelease(Context->PoolLock);
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Allocation failed, no %u contiguous pages available\n", numPages));
        return NULL;
    }

    /* Mark pages as allocated */
    RtlSetBits(&Context->AllocationBitmap, startPage, numPages);
    Context->AllocationSizes[startPage] = numPages;

    WdfSpinLockRelease(Context->PoolLock);

    /* Compute addresses */
    offset = (SIZE_T)startPage * PAGE_SIZE;
    va = (PUCHAR)Context->PoolBaseVA + offset;
    PhysicalAddress->QuadPart = Context->PoolBasePA.QuadPart + offset;
    *AllocatedSize = (SIZE_T)numPages * PAGE_SIZE;

    /* Zero the allocated region */
    RtlZeroMemory(va, *AllocatedSize);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_TRACE_LEVEL,
               "RDMAPool: Allocated %u pages at VA %p PA %I64X\n",
               numPages, va, PhysicalAddress->QuadPart));

    return va;
}

NTSTATUS RdmaPoolFree(PRDMAPOOL_DEVICE_CONTEXT Context, PVOID VirtualAddress)
{
    ULONG_PTR vaAddr = (ULONG_PTR)VirtualAddress;
    ULONG_PTR poolStart = (ULONG_PTR)Context->PoolBaseVA;
    ULONG_PTR poolEnd = poolStart + Context->PoolSize;
    ULONG pageIndex;
    ULONG numPages;

    if (!Context->PoolBaseVA) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Validate VA is in pool range */
    if (vaAddr < poolStart || vaAddr >= poolEnd) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Free failed, VA %p not in pool range\n", VirtualAddress));
        return STATUS_INVALID_PARAMETER;
    }

    /* Validate page alignment */
    if ((vaAddr - poolStart) % PAGE_SIZE != 0) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Free failed, VA %p not page-aligned\n", VirtualAddress));
        return STATUS_INVALID_PARAMETER;
    }

    pageIndex = (ULONG)((vaAddr - poolStart) / PAGE_SIZE);

    WdfSpinLockAcquire(Context->PoolLock);

    /* Validate this is the start of an allocation */
    numPages = Context->AllocationSizes[pageIndex];
    if (numPages == 0) {
        WdfSpinLockRelease(Context->PoolLock);
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Free failed, page %u is not an allocation start\n", pageIndex));
        return STATUS_INVALID_PARAMETER;
    }

    /* Clear allocation tracking */
    RtlClearBits(&Context->AllocationBitmap, pageIndex, numPages);
    Context->AllocationSizes[pageIndex] = 0;

    WdfSpinLockRelease(Context->PoolLock);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_TRACE_LEVEL,
               "RDMAPool: Freed %u pages at VA %p (page %u)\n",
               numPages, VirtualAddress, pageIndex));

    return STATUS_SUCCESS;
}

/* ========================================================================
 * Configuration Reading (ACPI and Registry)
 * ======================================================================== */

/*
 * Read DMA pool configuration from ACPI.
 * When the driver is ACPI-enumerated, evaluate RDPA and RDPS methods
 * on the device's ACPI namespace.
 */
NTSTATUS RdmaPoolReadAcpiConfig(WDFDEVICE Device, PPHYSICAL_ADDRESS Base, PSIZE_T Size)
{
    NTSTATUS status;
    ACPI_EVAL_INPUT_BUFFER inputBuffer;
    UCHAR outputBuf[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + sizeof(ACPI_METHOD_ARGUMENT)];
    PACPI_EVAL_OUTPUT_BUFFER outputBuffer = (PACPI_EVAL_OUTPUT_BUFFER)outputBuf;
    WDF_MEMORY_DESCRIPTOR inputDesc, outputDesc;
    ULONG_PTR bytesReturned = 0;

    PAGED_CODE();

    /* Evaluate RDPA (DMA pool address) */
    RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));
    inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(inputBuffer.MethodName, "RDPA", 4);

    RtlZeroMemory(outputBuf, sizeof(outputBuf));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDesc, &inputBuffer, sizeof(inputBuffer));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDesc, outputBuf, sizeof(outputBuf));

    status = WdfIoTargetSendIoctlSynchronously(
        WdfDeviceGetIoTarget(Device),
        NULL,
        IOCTL_ACPI_EVAL_METHOD,
        &inputDesc,
        &outputDesc,
        NULL,
        &bytesReturned);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: ACPI RDPA evaluation failed: 0x%X\n", status));
        return status;
    }

    if (outputBuffer->Count < 1 ||
        outputBuffer->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: ACPI RDPA returned invalid data\n"));
        return STATUS_ACPI_INVALID_DATA;
    }

    Base->QuadPart = (LONGLONG)outputBuffer->Argument[0].Argument;

    /* Evaluate RDPS (DMA pool size) */
    RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));
    inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(inputBuffer.MethodName, "RDPS", 4);

    RtlZeroMemory(outputBuf, sizeof(outputBuf));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDesc, &inputBuffer, sizeof(inputBuffer));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDesc, outputBuf, sizeof(outputBuf));

    status = WdfIoTargetSendIoctlSynchronously(
        WdfDeviceGetIoTarget(Device),
        NULL,
        IOCTL_ACPI_EVAL_METHOD,
        &inputDesc,
        &outputDesc,
        NULL,
        &bytesReturned);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: ACPI RDPS evaluation failed: 0x%X\n", status));
        return status;
    }

    if (outputBuffer->Count < 1 ||
        outputBuffer->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: ACPI RDPS returned invalid data\n"));
        return STATUS_ACPI_INVALID_DATA;
    }

    *Size = (SIZE_T)outputBuffer->Argument[0].Argument;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: ACPI config: Base=%I64X Size=%IX\n",
               Base->QuadPart, *Size));

    return STATUS_SUCCESS;
}

/*
 * Read DMA pool configuration from registry.
 * Registry path: HKLM\SYSTEM\CurrentControlSet\Services\rdmapool\Parameters
 *   DmaPoolBase  REG_QWORD  Physical base address
 *   DmaPoolSize  REG_DWORD  Pool size in bytes
 */
NTSTATUS RdmaPoolReadRegistryConfig(WDFDEVICE Device, PPHYSICAL_ADDRESS Base, PSIZE_T Size)
{
    NTSTATUS status;
    WDFKEY paramsKey = NULL;
    ULONG dmaPoolSize = 0;
    ULONG64 dmaPoolBase = 0;
    DECLARE_CONST_UNICODE_STRING(baseValueName, L"DmaPoolBase");
    DECLARE_CONST_UNICODE_STRING(sizeValueName, L"DmaPoolSize");

    PAGED_CODE();

    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DRIVER,
                                     KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &paramsKey);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: Failed to open registry key: 0x%X\n", status));
        return status;
    }

    /* Read DmaPoolBase (QWORD) */
    status = WdfRegistryQueryValue(paramsKey, &baseValueName, sizeof(dmaPoolBase),
                                  &dmaPoolBase, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        /* Try as DWORD for 32-bit addresses */
        ULONG baseAsUlong = 0;
        status = WdfRegistryQueryULong(paramsKey, &baseValueName, &baseAsUlong);
        if (NT_SUCCESS(status)) {
            dmaPoolBase = baseAsUlong;
        }
    }

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: Failed to read DmaPoolBase: 0x%X\n", status));
        WdfRegistryClose(paramsKey);
        return status;
    }

    /* Read DmaPoolSize (DWORD) */
    status = WdfRegistryQueryULong(paramsKey, &sizeValueName, &dmaPoolSize);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: Failed to read DmaPoolSize: 0x%X\n", status));
        WdfRegistryClose(paramsKey);
        return status;
    }

    WdfRegistryClose(paramsKey);

    Base->QuadPart = (LONGLONG)dmaPoolBase;
    *Size = (SIZE_T)dmaPoolSize;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: Registry config: Base=%I64X Size=%IX\n",
               Base->QuadPart, *Size));

    return STATUS_SUCCESS;
}

/* ========================================================================
 * IOCTL Handlers
 * ======================================================================== */

VOID RdmaPoolEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                                size_t OutputBufferLength, size_t InputBufferLength,
                                ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PRDMAPOOL_DEVICE_CONTEXT context = RdmaPoolGetDeviceContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytesReturned = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_RDMAPOOL_ALLOCATE: {
        PRDMAPOOL_ALLOCATE_INPUT input = NULL;
        PRDMAPOOL_ALLOCATE_OUTPUT output = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        output->VirtualAddress = RdmaPoolAllocate(context, input->RequestedSize,
                                                  &output->PhysicalAddress,
                                                  &output->AllocatedSize);
        if (output->VirtualAddress) {
            status = STATUS_SUCCESS;
            bytesReturned = sizeof(*output);
        } else {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        break;
    }

    case IOCTL_RDMAPOOL_FREE: {
        PRDMAPOOL_FREE_INPUT input = NULL;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*input), (PVOID *)&input, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        status = RdmaPoolFree(context, input->VirtualAddress);
        break;
    }

    case IOCTL_RDMAPOOL_QUERY_POOL: {
        PRDMAPOOL_QUERY_POOL_OUTPUT output = NULL;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
        if (!NT_SUCCESS(status)) {
            break;
        }

        output->BaseVirtualAddress = context->PoolBaseVA;
        output->BasePhysicalAddress = context->PoolBasePA;
        output->PoolSize = context->PoolSize;
        status = STATUS_SUCCESS;
        bytesReturned = sizeof(*output);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

/* ========================================================================
 * PnP / Power Callbacks
 * ======================================================================== */

NTSTATUS RdmaPoolEvtDevicePrepareHardware(WDFDEVICE Device,
                                          WDFCMRESLIST ResourcesRaw,
                                          WDFCMRESLIST ResourcesTranslated)
{
    PRDMAPOOL_DEVICE_CONTEXT context = RdmaPoolGetDeviceContext(Device);
    NTSTATUS status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: PrepareHardware\n"));

    /* Try ACPI first (works when ACPI-enumerated) */
    status = RdmaPoolReadAcpiConfig(Device, &context->PoolBasePA, &context->PoolSize);
    if (NT_SUCCESS(status)) {
        context->AcpiEnumerated = TRUE;
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "RDMAPool: Using ACPI configuration\n"));
    } else {
        /* Fall back to registry */
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
                   "RDMAPool: ACPI not available, trying registry\n"));
        status = RdmaPoolReadRegistryConfig(Device, &context->PoolBasePA, &context->PoolSize);
        if (!NT_SUCCESS(status)) {
            KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                       "RDMAPool: No valid configuration found\n"));
            return status;
        }
    }

    /* Validate configuration */
    if (!context->PoolBasePA.QuadPart || !context->PoolSize) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Invalid pool config: Base=%I64X Size=%IX\n",
                   context->PoolBasePA.QuadPart, context->PoolSize));
        return STATUS_INVALID_PARAMETER;
    }

    if (context->PoolSize % PAGE_SIZE != 0) {
        /* Round down to page boundary */
        context->PoolSize = (context->PoolSize / PAGE_SIZE) * PAGE_SIZE;
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
                   "RDMAPool: Pool size rounded down to %IX\n", context->PoolSize));
    }

    /* Initialize the pool allocator */
    status = RdmaPoolInitialize(context);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: Pool initialization failed: 0x%X\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS RdmaPoolEvtDeviceReleaseHardware(WDFDEVICE Device,
                                          WDFCMRESLIST ResourcesTranslated)
{
    PRDMAPOOL_DEVICE_CONTEXT context = RdmaPoolGetDeviceContext(Device);

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: ReleaseHardware\n"));

    RdmaPoolDestroy(context);

    return STATUS_SUCCESS;
}

NTSTATUS RdmaPoolEvtDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "RDMAPool: D0Entry\n"));
    return STATUS_SUCCESS;
}

NTSTATUS RdmaPoolEvtDeviceD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "RDMAPool: D0Exit\n"));
    return STATUS_SUCCESS;
}

/* ========================================================================
 * Device Add and Driver Entry
 * ======================================================================== */

NTSTATUS RdmaPoolEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PRDMAPOOL_DEVICE_CONTEXT context;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "RDMAPool: DeviceAdd\n"));

    /* Setup PnP/Power callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = RdmaPoolEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = RdmaPoolEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = RdmaPoolEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = RdmaPoolEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    /* Create device with context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RDMAPOOL_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: WdfDeviceCreate failed: 0x%X\n", status));
        return status;
    }

    context = RdmaPoolGetDeviceContext(device);
    RtlZeroMemory(context, sizeof(*context));

    /* Create spinlock for pool synchronization */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->PoolLock);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: WdfSpinLockCreate failed: 0x%X\n", status));
        return status;
    }

    /* Create I/O queue for IOCTL handling */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = RdmaPoolEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: WdfIoQueueCreate failed: 0x%X\n", status));
        return status;
    }

    /* Register device interface so clients can find us */
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_RDMA_POOL, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: WdfDeviceCreateDeviceInterface failed: 0x%X\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: Device created successfully\n"));

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL,
               "RDMAPool: DriverEntry\n"));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, RdmaPoolEvtDeviceAdd);
    config.DriverPoolTag = RDMAPOOL_POOL_TAG;

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "RDMAPool: WdfDriverCreate failed: 0x%X\n", status));
    }

    return status;
}
