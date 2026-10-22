/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include <dlfcn.h>

#include "vkr_library.h"

void
vkr_library_preload_icd(void)
{
#ifdef ENABLE_VULKAN_PRELOAD
   struct vulkan_library lib = { 0 };

   if (!vkr_library_load(&lib))
      return;

   /* Get vkGetInstanceProcAddr from libvulkan */
   PFN_vkGetInstanceProcAddr get_proc_addr = lib.GetInstanceProcAddr;

   PFN_vkEnumerateInstanceExtensionProperties enumerate_inst_ext_props =
      (PFN_vkEnumerateInstanceExtensionProperties)get_proc_addr(
         VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   if (enumerate_inst_ext_props) {
      /* this makes the Vulkan loader loads ICDs */
      uint32_t unused_count;
      enumerate_inst_ext_props(NULL, &unused_count, NULL);
   }

   vkr_library_unload(&lib);
#endif
}

#if defined(ENABLE_VULKAN_DLOAD)

#ifdef __APPLE__
typedef VkResult(VKAPI_PTR *vkr_PFN_icdNegotiateLoaderICDInterfaceVersion)(
   uint32_t *pSupportedVersion);

/* The Vulkan driver is chosen at runtime: VKR_VULKAN_DRIVER names its dylib (a
 * path, or @rpath/... resolved with the rpaths of the VMM and of this
 * library), MoltenVK by default.  Mesa drivers (KosmicKrisp) export only the
 * loader-ICD interface; they are used without a loader through
 * vk_icdGetInstanceProcAddr, after the interface version negotiation a loader
 * would do.
 */
static const char *
vkr_library_driver_path(void)
{
   const char *path = getenv("VKR_VULKAN_DRIVER");
   return path && *path ? path : "@rpath/libMoltenVK.dylib";
}

static PFN_vkGetInstanceProcAddr
vkr_library_icd_get_proc_addr(void *handle)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   vkr_PFN_icdNegotiateLoaderICDInterfaceVersion negotiate =
      (vkr_PFN_icdNegotiateLoaderICDInterfaceVersion)dlsym(
         handle, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr get_proc_addr =
      (PFN_vkGetInstanceProcAddr)dlsym(handle, "vk_icdGetInstanceProcAddr");
#pragma GCC diagnostic pop
   if (!get_proc_addr)
      return NULL;

   if (negotiate) {
      uint32_t version = 7;
      if (negotiate(&version) != VK_SUCCESS || version < 5) {
         vkr_log("driver ICD interface version negotiation failed (version %u)",
                 version);
         return NULL;
      }
   }
   return get_proc_addr;
}
#endif

bool
vkr_library_load(struct vulkan_library *lib)
{
   if (lib->handle)
      return true;

#ifdef __APPLE__
   const char *path = vkr_library_driver_path();
   lib->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#else
   lib->handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
   if (lib->handle == NULL) {
      vkr_log("failed to open libvulkan: %s", dlerror());
      return false;
   }

   /* Clear any existing error */
   dlerror();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   /* ISO C forbids conversion of object pointer to function pointer type */
   lib->GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)dlsym(lib->handle, "vkGetInstanceProcAddr");
#pragma GCC diagnostic pop

#ifdef __APPLE__
   if (lib->GetInstanceProcAddr == NULL)
      lib->GetInstanceProcAddr = vkr_library_icd_get_proc_addr(lib->handle);
#endif

   if (lib->GetInstanceProcAddr == NULL) {
#ifdef __APPLE__
      vkr_log("%s exports neither vkGetInstanceProcAddr nor vk_icdGetInstanceProcAddr",
              path);
#else
      vkr_log("failed to load vkGetInstanceProcAddr: %s", dlerror());
#endif
      goto fail;
   }

   return true;

fail:
   dlclose(lib->handle);
   lib->handle = NULL;
   return false;
}

void
vkr_library_unload(struct vulkan_library *lib)
{
   if (lib->handle) {
      dlclose(lib->handle);
      lib->GetInstanceProcAddr = NULL;
      lib->handle = NULL;
   }
}

#endif /* ENABLE_VULKAN_DLOAD */

bool
vkr_library_has_portability_enumeration(
   PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext_props)
{
   uint32_t property_count = 0;
   VkExtensionProperties *properties;
   bool has_portability_enumeration = false;

   VkResult ret = enum_inst_ext_props(NULL, &property_count, NULL);
   if (ret != VK_SUCCESS)
      return false;

   properties = calloc(property_count, sizeof(*properties));
   if (!properties)
      return false;

   ret = enum_inst_ext_props(NULL, &property_count, properties);
   if (ret != VK_SUCCESS) {
      free(properties);
      return false;
   }

   for (uint32_t i = 0; i < property_count; i++) {
      if (!strcmp(properties[i].extensionName,
                  VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
         has_portability_enumeration = true;
         break;
      }
   }
   free(properties);
   return has_portability_enumeration;
}
