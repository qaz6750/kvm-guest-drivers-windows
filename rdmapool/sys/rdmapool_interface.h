/*
 * Restricted DMA Pool Driver - Public Interface Header
 *
 * This header defines the shared interface between the rdmapool driver
 * and its clients (e.g., VirtIOLib). It includes device interface GUID,
 * IOCTL codes, and input/output structures.
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

/*
 * NOTE: Include <initguid.h> BEFORE this header in exactly one .c file
 * per binary to generate the GUID definition. Other .c files will get
 * the extern declaration automatically.
 */
#include <ntddk.h>

/* Device interface GUID for the Restricted DMA Pool driver.
 * Clients use this GUID to discover and open the rdmapool device.
 * {A5C36C8E-3742-45A8-8B7D-9F1A2B3C4D5E}
 */
DEFINE_GUID(GUID_DEVINTERFACE_RDMA_POOL,
    0xa5c36c8e, 0x3742, 0x45a8, 0x8b, 0x7d, 0x9f, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e);

#define RDMAPOOL_DEVICE_TYPE FILE_DEVICE_UNKNOWN

/*
 * IOCTL_RDMAPOOL_ALLOCATE
 * Allocates a contiguous DMA buffer from the restricted DMA pool.
 * Input:  RDMAPOOL_ALLOCATE_INPUT  (requested size)
 * Output: RDMAPOOL_ALLOCATE_OUTPUT (VA, PA, actual size)
 */
#define IOCTL_RDMAPOOL_ALLOCATE \
    CTL_CODE(RDMAPOOL_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_RDMAPOOL_FREE
 * Frees a previously allocated DMA buffer.
 * Input:  RDMAPOOL_FREE_INPUT (VA to free)
 * Output: None
 */
#define IOCTL_RDMAPOOL_FREE \
    CTL_CODE(RDMAPOOL_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * IOCTL_RDMAPOOL_QUERY_POOL
 * Queries the DMA pool base address and size information.
 * Input:  None
 * Output: RDMAPOOL_QUERY_POOL_OUTPUT (base VA, base PA, pool size)
 */
#define IOCTL_RDMAPOOL_QUERY_POOL \
    CTL_CODE(RDMAPOOL_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Input structure for IOCTL_RDMAPOOL_ALLOCATE */
typedef struct _RDMAPOOL_ALLOCATE_INPUT {
    SIZE_T RequestedSize;  /* Size in bytes (will be rounded up to page boundary) */
} RDMAPOOL_ALLOCATE_INPUT, *PRDMAPOOL_ALLOCATE_INPUT;

/* Output structure for IOCTL_RDMAPOOL_ALLOCATE */
typedef struct _RDMAPOOL_ALLOCATE_OUTPUT {
    PVOID VirtualAddress;              /* Kernel virtual address of allocated buffer */
    PHYSICAL_ADDRESS PhysicalAddress;  /* Physical address of allocated buffer */
    SIZE_T AllocatedSize;              /* Actual allocated size (page-aligned) */
} RDMAPOOL_ALLOCATE_OUTPUT, *PRDMAPOOL_ALLOCATE_OUTPUT;

/* Input structure for IOCTL_RDMAPOOL_FREE */
typedef struct _RDMAPOOL_FREE_INPUT {
    PVOID VirtualAddress;  /* Kernel VA of buffer to free (must be exact allocation start) */
} RDMAPOOL_FREE_INPUT, *PRDMAPOOL_FREE_INPUT;

/* Output structure for IOCTL_RDMAPOOL_QUERY_POOL */
typedef struct _RDMAPOOL_QUERY_POOL_OUTPUT {
    PVOID BaseVirtualAddress;              /* Pool base kernel VA */
    PHYSICAL_ADDRESS BasePhysicalAddress;  /* Pool base physical address */
    SIZE_T PoolSize;                       /* Total pool size in bytes */
} RDMAPOOL_QUERY_POOL_OUTPUT, *PRDMAPOOL_QUERY_POOL_OUTPUT;
