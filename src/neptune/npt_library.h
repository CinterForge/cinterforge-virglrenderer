/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_LIBRARY_H
#define NPT_LIBRARY_H

#include "npt_common.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

#include "npt_library_names.h"

#include <directx-native/dxn_embedder.h>

/*
 * Loads the host D3D backend libraries.  Path lookup: NPT_*_LIBRARY_PATH
 * env var if set, otherwise the built-in default name.
 */

/* Host backend workaround flag bits, host side.  Derived at backend-library
 * load from the loaded backend's needs and written once -- ORed with
 * NPT_WA_FLAGS_PRESENT so the guest can tell the word was written -- into the
 * ring blob at npt_cmd_create_ring.workaround_offset.  Each bit names a
 * shader/cap patch the guest applies ONLY when the bit is set; a backend that
 * needs none leaves them all clear.  These compensate for a specific host
 * backend's defects, not general correctness.  The guest defines the same wire
 * values in its own npt_workaround.h; the two are matched by review, not by a
 * shared header. */
#define NPT_WA_FLAGS_PRESENT                     (1u << 31) /* host wrote the word */
#define NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK        (1u << 0)  /* scalar .x IA VS input -> .xy */
#define NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT  (1u << 1)  /* VS ISGN comp type from bound format */
#define NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT  (1u << 2)  /* dcl_input_ps noperspective -> linear */
#define NPT_WA_SYNTHESIZE_IO_SIGNATURE_FROM_SHDR (1u << 3)  /* empty GS ISGN/OSGN from SHDR DCLs */

/* Which darwin backend the loaded umbrella turned out to be, detected in
 * npt_library_init from the embedder event-API symbol prefix. */
enum npt_backend_kind {
   NPT_BACKEND_UNKNOWN = 0,
   NPT_BACKEND_D3DMETAL,   /* Apple D3DMetal via d3dmetal-native (dmn_*) */
   NPT_BACKEND_DXMT,       /* DXMT native D3D11-on-Metal (dxmt_*) */
};

/* Real library-ABI signatures for the root-signature serializers.  The
 * generated PFN_D3D12Serialize* typedefs describe the Neptune WIRE shape
 * (byte-array outputs; the guest can't consume a host-side ID3DBlob), so
 * the host keeps its own typedefs matching the actual exports.
 * NPT_STDMETHODCALLTYPE is required: the darwin backend exports these
 * MS-ABI, same as D3D12CreateDevice, so without it every argument lands
 * in the wrong register and the serializer rejects any desc. */
typedef HRESULT (NPT_STDMETHODCALLTYPE *PFN_npt_lib_D3D12SerializeRootSignature)(
   const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
   D3D_ROOT_SIGNATURE_VERSION Version,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob);
typedef HRESULT (NPT_STDMETHODCALLTYPE *PFN_npt_lib_D3D12SerializeVersionedRootSignature)(
   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *pRootSignature,
   ID3DBlob **ppBlob,
   ID3DBlob **ppErrorBlob);

/* The embedder API every backend exports beside the D3D entry points --
 * events, shared-handle close, heap import, external DYNAMIC buffers -- is
 * directx-native's (dxn_embedder.h): the PFN_dxn_* types below are its, and a
 * backend whose <prefix>native_abi_version differs from DXN_ABI_VERSION is
 * refused at load. */

struct npt_d3d_library {
   void *d3d11_module;
   void *dxgi_module;
   void *d3d12_module;

   PFN_D3D11CreateDevice pfn_D3D11CreateDevice;
   PFN_D3D11On12CreateDevice pfn_D3D11On12CreateDevice;

   PFN_CreateDXGIFactory1 pfn_CreateDXGIFactory1;

   PFN_D3D12CreateDevice pfn_D3D12CreateDevice;

   /* Darwin embedder event API, dlsym'd from the backend umbrella
    * (d3dmetal exports dmn_event_*, dxmt exports dxmt_event_*).  NULL off
    * darwin or when the backend did not load; npt_event.c signals through
    * these. */
   PFN_dxn_event_create pfn_event_create;
   PFN_dxn_event_close pfn_event_close;
   PFN_dxn_event_dup_fd pfn_event_dup_fd;
   /* Darwin: CloseHandle analog for the NT-style handles the backend's
    * CreateSharedHandle family vends (each owns its own fd).  NULL on a
    * backend without one (dxmt has no NT-style handles). */
   PFN_dxn_shared_handle_close pfn_shared_handle_close;
   /* Optional, darwin: same contract as the vkd3d dmabuf twin below.
    * NULL => CREATE_HEAP_FROM_SHMEM fails cleanly (sync-map fallback). */
   PFN_dxn_open_existing_heap_from_fd pfn_darwin_open_existing_heap_from_fd;
   /* DXMT or dxvk; NULL => RESOURCE_BIND_D3D11_BUFFER_SHMEM is
    * E_NOTIMPL. */
   PFN_dxn_d3d11_buffer_bind_external_fd pfn_d3d11_buffer_bind_external_fd;
   PFN_dxn_d3d11_buffer_external_cookie pfn_d3d11_buffer_external_cookie;
   enum npt_backend_kind backend;

   /* NPT_WA_* bits describing the workarounds the loaded backend needs, set in
    * npt_library_init. Reported to the guest per-context (via the ring blob) so
    * Triton gates host-backend-specific shader/cap patches on them. */
   uint32_t workaround_flags;

   /* Optional: resolved from the d3d12 module (the vkd3d-proton loader
    * exports them alongside D3D12CreateDevice). */
   PFN_npt_lib_D3D12SerializeRootSignature pfn_D3D12SerializeRootSignature;
   PFN_npt_lib_D3D12SerializeVersionedRootSignature pfn_D3D12SerializeVersionedRootSignature;
   /* Optional: NULL when the d3d12 library predates the Neptune
    * dmabuf-heap export; CREATE_HEAP_FROM_SHMEM then fails cleanly and
    * the guest stays on the sync-map path. */
   PFN_dxn_open_existing_heap_from_dmabuf pfn_vkd3d_open_existing_heap_from_dmabuf;
};

bool
npt_library_init(struct npt_d3d_library *lib);

void
npt_library_fini(struct npt_d3d_library *lib);

#endif /* NPT_LIBRARY_H */
