/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side D3D backend library loader.  Any library may be absent;
 * the corresponding top-level overrides return E_FAIL in that case.
 */

#include "npt_library.h"

#include <stdio.h>

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#include <stdio.h>

/* Env-var names and library-name defaults live in npt_library_names.h
 * (shared with the npt_renderer.c D3D12 capset probe). */

#ifdef HAVE_DLFCN_H

static void *
npt_library_open(const char *env_var, const char *default_name)
{
   const char *path = getenv(env_var);
   if (!path)
      path = default_name;

   dlerror(); /* clear */
   void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!handle) {
      npt_log("failed to open %s (%s): %s", env_var, path, dlerror());
   }
   return handle;
}

static void *
npt_library_sym(void *handle, const char *name)
{
   dlerror(); /* clear */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   void *sym = dlsym(handle, name);
#pragma GCC diagnostic pop

   const char *error = dlerror();
   if (error) {
      npt_log("failed to load %s: %s", name, error);
      return NULL;
   }
   return sym;
}

/* <prefix><suffix> from `mod`, logging when it is absent. */
static void *
npt_library_embedder_sym(void *mod, const char *prefix, const char *suffix)
{
   char name[96];
   snprintf(name, sizeof(name), "%s%s", prefix, suffix);
   return npt_library_sym(mod, name);
}

/* The same for a symbol the backend may legitimately lack: absent is not
 * worth a log line. */
static void *
npt_library_embedder_sym_optional(void *mod, const char *prefix,
                                  const char *suffix)
{
   char name[96];
   snprintf(name, sizeof(name), "%s%s", prefix, suffix);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   void *sym = dlsym(mod, name);
#pragma GCC diagnostic pop
   dlerror(); /* clear */
   return sym;
}

/* Every library exporting directx-native embedder symbols reports the
 * DXN_ABI_VERSION it was built with; the descriptors and the embedder API
 * are lockstep, so anything else -- including no answer -- is refused. */
static bool
npt_library_check_abi(void *mod, const char *prefix, const char *what)
{
   PFN_dxn_native_abi_version query =
      ((union { void *p; PFN_dxn_native_abi_version f; }){
         .p = npt_library_embedder_sym(mod, prefix,
                                       DXN_SYM_NATIVE_ABI_VERSION) }).f;
   if (!query) {
      npt_log("%s library has no %s" DXN_SYM_NATIVE_ABI_VERSION
              "; refusing it (this renderer speaks directx-native ABI %u)",
              what, prefix, DXN_ABI_VERSION);
      return false;
   }
   const uint32_t version = query();
   if (version != DXN_ABI_VERSION) {
      npt_log("%s library speaks directx-native ABI %u, this renderer %u; "
              "refusing it", what, version, DXN_ABI_VERSION);
      return false;
   }
   npt_log("%s library: directx-native ABI %u", what, version);
   return true;
}

#ifdef __APPLE__
/* Bind the backend's embedder API from the loaded umbrella.  d3dmetal
 * prefixes its exports dmn_, dxmt prefixes them dxmt_; probing each
 * prefix's native_abi_version (which every backend exports) identifies the
 * backend, which the workaround-flags gate below also needs.  False when
 * the backend's directx-native ABI does not match ours. */
static bool
npt_library_load_embedder_api(struct npt_d3d_library *lib)
{
   void *mod = lib->d3d11_module ? lib->d3d11_module
             : lib->dxgi_module  ? lib->dxgi_module
             :                     lib->d3d12_module;
   if (!mod)
      return true;

   static const struct {
      const char *prefix;
      enum npt_backend_kind kind;
   } candidates[] = {
      { "dmn_",  NPT_BACKEND_D3DMETAL },
      { "dxmt_", NPT_BACKEND_DXMT },
   };

   for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
      const char *prefix = candidates[i].prefix;
      char name[64];
      snprintf(name, sizeof(name), "%s" DXN_SYM_NATIVE_ABI_VERSION, prefix);
      dlerror();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
      void *abi = dlsym(mod, name);
#pragma GCC diagnostic pop
      dlerror(); /* clear */
      if (!abi)
         continue;

      lib->backend = candidates[i].kind;
      if (!npt_library_check_abi(mod, prefix, "backend"))
         return false;

      lib->pfn_event_create =
         ((union { void *p; PFN_dxn_event_create f; }){
            .p = npt_library_embedder_sym(mod, prefix,
                                          DXN_SYM_EVENT_CREATE) }).f;
      lib->pfn_event_close =
         ((union { void *p; PFN_dxn_event_close f; }){
            .p = npt_library_embedder_sym(mod, prefix,
                                          DXN_SYM_EVENT_CLOSE) }).f;
      lib->pfn_event_dup_fd =
         ((union { void *p; PFN_dxn_event_dup_fd f; }){
            .p = npt_library_embedder_sym(mod, prefix,
                                          DXN_SYM_EVENT_DUP_FD) }).f;

      /* The rest is optional: a backend without NT-style handles, D3D12
       * heap import or external DYNAMIC buffers exports nothing for it, and
       * the users fail cleanly. */
      lib->pfn_shared_handle_close =
         ((union { void *p; PFN_dxn_shared_handle_close f; }){
            .p = npt_library_embedder_sym_optional(mod, prefix,
                                          DXN_SYM_SHARED_HANDLE_CLOSE) }).f;
      lib->pfn_darwin_open_existing_heap_from_fd =
         ((union { void *p; PFN_dxn_open_existing_heap_from_fd f; }){
            .p = npt_library_embedder_sym_optional(
               mod, prefix, DXN_SYM_OPEN_EXISTING_HEAP_FROM_FD) }).f;
      lib->pfn_d3d11_buffer_bind_external_fd =
         ((union { void *p; PFN_dxn_d3d11_buffer_bind_external_fd f; }){
            .p = npt_library_embedder_sym_optional(
               mod, prefix, DXN_SYM_D3D11_BUFFER_BIND_EXTERNAL_FD) }).f;
      lib->pfn_d3d11_buffer_external_cookie =
         ((union { void *p; PFN_dxn_d3d11_buffer_external_cookie f; }){
            .p = npt_library_embedder_sym_optional(
               mod, prefix, DXN_SYM_D3D11_BUFFER_EXTERNAL_COOKIE) }).f;
      return true;
   }
   npt_log("backend embedder API (dmn_/dxmt_" DXN_SYM_NATIVE_ABI_VERSION
           ") not found");
   return true;
}
#endif /* __APPLE__ */

#endif /* HAVE_DLFCN_H */

bool
npt_library_init(struct npt_d3d_library *lib)
{
   memset(lib, 0, sizeof(*lib));

#ifdef HAVE_DLFCN_H
#ifndef __APPLE__
   /* Headless operation requires the host D3D11/DXGI library's
    * headless WSI backend. */
   setenv("DXVK_WSI_DRIVER", "Headless", 0);
#endif

   lib->d3d11_module = npt_library_open("NPT_D3D11_LIBRARY_PATH",
                                         NPT_D3D11_LIBRARY_DEFAULT);
   if (lib->d3d11_module) {
      lib->pfn_D3D11CreateDevice =
         ((union { void *p; PFN_D3D11CreateDevice f; }){
            .p = npt_library_sym(lib->d3d11_module, "D3D11CreateDevice")
         }).f;
      if (!lib->pfn_D3D11CreateDevice) {
         npt_log("D3D11 library loaded but D3D11CreateDevice not found");
         dlclose(lib->d3d11_module);
         lib->d3d11_module = NULL;
      } else {
         /* Optional. */
         lib->pfn_D3D11On12CreateDevice =
            ((union { void *p; PFN_D3D11On12CreateDevice f; }){
               .p = npt_library_sym(lib->d3d11_module,
                                    "D3D11On12CreateDevice")
            }).f;
#ifndef __APPLE__
         /* dxvk's embedder API.  Optional: without it the guest's
          * DYNAMIC buffers stay on the copying map ring. */
         if (!npt_library_check_abi(lib->d3d11_module, "dxvk_", "D3D11")) {
            dlclose(lib->d3d11_module);
            lib->d3d11_module = NULL;
            lib->pfn_D3D11CreateDevice = NULL;
            lib->pfn_D3D11On12CreateDevice = NULL;
         } else {
            lib->pfn_d3d11_buffer_bind_external_fd =
               ((union { void *p; PFN_dxn_d3d11_buffer_bind_external_fd f; }){
                  .p = npt_library_embedder_sym_optional(
                     lib->d3d11_module, "dxvk_",
                     DXN_SYM_D3D11_BUFFER_BIND_EXTERNAL_FD) }).f;
            lib->pfn_d3d11_buffer_external_cookie =
               ((union { void *p; PFN_dxn_d3d11_buffer_external_cookie f; }){
                  .p = npt_library_embedder_sym_optional(
                     lib->d3d11_module, "dxvk_",
                     DXN_SYM_D3D11_BUFFER_EXTERNAL_COOKIE) }).f;
         }
#endif
      }
   }

   lib->dxgi_module = npt_library_open("NPT_DXGI_LIBRARY_PATH",
                                        NPT_DXGI_LIBRARY_DEFAULT);
   if (lib->dxgi_module) {
      lib->pfn_CreateDXGIFactory1 =
         ((union { void *p; PFN_CreateDXGIFactory1 f; }){
            .p = npt_library_sym(lib->dxgi_module, "CreateDXGIFactory1")
         }).f;
      if (!lib->pfn_CreateDXGIFactory1) {
         npt_log("DXGI library loaded but CreateDXGIFactory1 not found");
         dlclose(lib->dxgi_module);
         lib->dxgi_module = NULL;
      }
   }

   /* Persistently-mapped UPLOAD/READBACK heaps hand the guest's SHM
    * pages straight to the app, so vkd3d falling back to a private
    * allocation on a failed host import would silently make the GPU read
    * pages the guest never writes.  require_host_import turns that into a
    * loud failure the guest answers by degrading to its sync-copy path.
    * Must be set before the module loads: the config parses once. */
   {
      const char *cfg = getenv("VKD3D_CONFIG");
      if (!cfg || !strstr(cfg, "require_host_import")) {
         if (cfg && cfg[0]) {
            char merged[1024];
            int n = snprintf(merged, sizeof(merged),
                             "%s,require_host_import", cfg);
            if (n > 0 && (size_t)n < sizeof(merged))
               setenv("VKD3D_CONFIG", merged, 1);
            else
               npt_log("VKD3D_CONFIG too long; require_host_import NOT added");
         } else {
            setenv("VKD3D_CONFIG", "require_host_import", 1);
         }
      }
   }

   lib->d3d12_module = npt_library_open("NPT_D3D12_LIBRARY_PATH",
                                         NPT_D3D12_LIBRARY_DEFAULT);
   if (lib->d3d12_module) {
      lib->pfn_D3D12CreateDevice =
         ((union { void *p; PFN_D3D12CreateDevice f; }){
            .p = npt_library_sym(lib->d3d12_module, "D3D12CreateDevice")
         }).f;
      if (!lib->pfn_D3D12CreateDevice) {
         npt_log("D3D12 library loaded but D3D12CreateDevice not found");
         dlclose(lib->d3d12_module);
         lib->d3d12_module = NULL;
      } else {
         /* Optional; the toplevel overrides fail cleanly when absent. */
         lib->pfn_D3D12SerializeRootSignature =
            ((union { void *p; PFN_npt_lib_D3D12SerializeRootSignature f; }){
               .p = npt_library_sym(lib->d3d12_module,
                                    "D3D12SerializeRootSignature")
            }).f;
         lib->pfn_D3D12SerializeVersionedRootSignature =
            ((union { void *p;
                      PFN_npt_lib_D3D12SerializeVersionedRootSignature f; }){
               .p = npt_library_sym(lib->d3d12_module,
                                    "D3D12SerializeVersionedRootSignature")
            }).f;
#ifndef __APPLE__
         /* vkd3d-proton's embedder API.  Optional: NULL makes
          * CREATE_HEAP_FROM_SHMEM fail cleanly and the guest stays on the
          * sync-map path. */
         if (!npt_library_check_abi(lib->d3d12_module, "vkd3d_", "D3D12")) {
            dlclose(lib->d3d12_module);
            lib->d3d12_module = NULL;
            lib->pfn_D3D12CreateDevice = NULL;
            lib->pfn_D3D12SerializeRootSignature = NULL;
            lib->pfn_D3D12SerializeVersionedRootSignature = NULL;
         } else {
            lib->pfn_vkd3d_open_existing_heap_from_dmabuf =
               ((union { void *p; PFN_dxn_open_existing_heap_from_dmabuf f; }){
                  .p = npt_library_embedder_sym_optional(
                     lib->d3d12_module, "vkd3d_",
                     DXN_SYM_OPEN_EXISTING_HEAP_FROM_DMABUF) }).f;
         }
#endif
      }
   }

   if (lib->d3d11_module) {
      const char *p = getenv("NPT_D3D11_LIBRARY_PATH");
      npt_log("loaded D3D11 library: %s", p ? p : NPT_D3D11_LIBRARY_DEFAULT);
   }
   if (lib->dxgi_module) {
      const char *p = getenv("NPT_DXGI_LIBRARY_PATH");
      npt_log("loaded DXGI library: %s", p ? p : NPT_DXGI_LIBRARY_DEFAULT);
   }
   if (lib->d3d12_module) {
      const char *p = getenv("NPT_D3D12_LIBRARY_PATH");
      npt_log("loaded D3D12 library: %s", p ? p : NPT_D3D12_LIBRARY_DEFAULT);
   }

#ifdef __APPLE__
   if (!npt_library_load_embedder_api(lib)) {
      npt_library_fini(lib);
      return false;
   }

   /* D3DMetal's DXBC->AIR/DXIL shader converter has several defects the guest
    * driver (Triton) patches around. Advertise exactly those patches -- each a
    * specific ISGN/OSGN edit -- so the guest applies them only against
    * D3DMetal; DXMT reports none. */
   if (lib->backend == NPT_BACKEND_D3DMETAL && lib->d3d11_module)
      lib->workaround_flags =
         NPT_WA_WIDEN_SCALAR_VS_INPUT_MASK |
         NPT_WA_TYPE_VS_INPUT_FROM_VERTEX_FORMAT |
         NPT_WA_LINEARIZE_NOPERSPECTIVE_PS_INPUT |
         NPT_WA_SYNTHESIZE_IO_SIGNATURE_FROM_SHDR;
#endif

   /* Debug/test override: NPT_WA_FLAGS forces the workaround-flags set (hex or
    * decimal), e.g. NPT_WA_FLAGS=0 disables every host workaround so the raw
    * backend behavior (and the gates) can be exercised. */
   {
      const char *ov = getenv("NPT_WA_FLAGS");
      if (ov) {
         lib->workaround_flags = (uint32_t)strtoul(ov, NULL, 0);
         npt_log("NPT_WA_FLAGS override -> workaround_flags=0x%08x",
                 lib->workaround_flags);
      }
   }
#else
   npt_log("D3D library loading: dlopen not available");
#endif

   return true;
}

void
npt_library_fini(struct npt_d3d_library *lib)
{
#ifdef HAVE_DLFCN_H
   if (lib->d3d11_module)
      dlclose(lib->d3d11_module);
   if (lib->dxgi_module)
      dlclose(lib->dxgi_module);
   if (lib->d3d12_module)
      dlclose(lib->d3d12_module);
#endif

   memset(lib, 0, sizeof(*lib));
}
