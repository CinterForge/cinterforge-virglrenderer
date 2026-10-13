/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * GPU-exportable host memory for guest SHM blobs that back D3D12 heaps.
 */

#ifndef NPT_HOSTMEM_H
#define NPT_HOSTMEM_H

#include "npt_common.h"

/* Blobs above this get Vulkan-exported dmabuf storage instead of a
 * memfd: udmabuf's default size_limit_mb rejects wrapping larger memfd
 * windows, which would leave a D3D12 heap on the per-Unmap copy path. */
#define NPT_HOSTMEM_DMABUF_MIN_SIZE (64ull << 20)

#ifdef __linux__
/* Allocates `size` bytes of write-back, GPU-importable memory on the
 * GPU the D3D library runs on.  Returns a dmabuf fd or -1. */
int
npt_hostmem_alloc_dmabuf(uint64_t size);
#else
static inline int
npt_hostmem_alloc_dmabuf(UNUSED uint64_t size)
{
   return -1;
}
#endif

#endif /* NPT_HOSTMEM_H */
