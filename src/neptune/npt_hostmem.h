/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * GPU-importable views of guest SHM blobs: D3D12 heaps and D3D11
 * DYNAMIC buffer slices built on a blob's pages.
 */

#ifndef NPT_HOSTMEM_H
#define NPT_HOSTMEM_H

#include "npt_common.h"

/* Blobs above this get Vulkan-exported dmabuf storage instead of a
 * memfd: udmabuf's default size_limit_mb rejects wrapping larger memfd
 * windows, which would leave a D3D12 heap on the per-Unmap copy path. */
#define NPT_HOSTMEM_DMABUF_MIN_SIZE (64ull << 20)

struct npt_resource;

#ifdef __linux__
/* Allocates `size` bytes of write-back, GPU-importable memory on the
 * GPU the D3D library runs on.  Returns a dmabuf fd or -1. */
int
npt_hostmem_alloc_dmabuf(uint64_t size);

/* A dmabuf of [offset, offset + size) of the SHM blob `res`, for the
 * caller to close; -1 (logged under `who`) when there is none.  A
 * dmabuf-backed blob only has its whole range. */
int
npt_hostmem_blob_dmabuf(const struct npt_resource *res, uint64_t offset,
                        uint64_t size, const char *who);
#else
static inline int
npt_hostmem_alloc_dmabuf(UNUSED uint64_t size)
{
   return -1;
}
#endif

#endif /* NPT_HOSTMEM_H */
