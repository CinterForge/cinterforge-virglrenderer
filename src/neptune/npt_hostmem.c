/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_hostmem.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "c11/threads.h"
#include "venus-protocol/vulkan.h"

static struct {
   void *lib;
   VkInstance instance;
   VkDevice device;
   uint32_t type_index;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
   bool ready;
} hostmem;

static once_flag hostmem_once = ONCE_FLAG_INIT;

/* vkd3d imports the dmabuf into its own VkDevice, so the BO has to come
 * from the GPU that device runs on.  vkd3d is created later and cannot
 * be asked; follow the same DRI_PRIME the device-select layer obeys. */
static VkPhysicalDevice
npt_hostmem_pick_physical_device(VkInstance inst,
                                 PFN_vkGetInstanceProcAddr gipa)
{
   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices =
      (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 =
      (PFN_vkGetPhysicalDeviceProperties2)gipa(inst,
                                              "vkGetPhysicalDeviceProperties2");

   VkPhysicalDevice devs[8];
   uint32_t count = ARRAY_SIZE(devs);
   VkResult vr = EnumeratePhysicalDevices(inst, &count, devs);
   if ((vr != VK_SUCCESS && vr != VK_INCOMPLETE) || !count)
      return VK_NULL_HANDLE;

   unsigned dom, bus, dev, fn;
   const char *prime = getenv("DRI_PRIME");
   const bool want_pci =
      prime && sscanf(prime, "pci-%x_%x_%x_%x", &dom, &bus, &dev, &fn) == 4;

   VkPhysicalDevice discrete = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
      };
      VkPhysicalDeviceProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = want_pci ? &pci : NULL,
      };
      GetPhysicalDeviceProperties2(devs[i], &props);

      if (want_pci && pci.pciDomain == dom && pci.pciBus == bus &&
          pci.pciDevice == dev && pci.pciFunction == fn) {
         npt_log("hostmem: using %s (DRI_PRIME %s)",
                 props.properties.deviceName, prime);
         return devs[i];
      }
      if (!discrete &&
          props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
         discrete = devs[i];
   }

   return discrete ? discrete : devs[0];
}

/* Write-back GTT: the guest maps these pages cached, so a
 * write-combined type would alias two cacheability attributes. */
static bool
npt_hostmem_pick_type(VkInstance inst, PFN_vkGetInstanceProcAddr gipa,
                      VkPhysicalDevice pdev, uint32_t *out_index)
{
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
         inst, "vkGetPhysicalDeviceMemoryProperties");

   VkPhysicalDeviceMemoryProperties props;
   GetPhysicalDeviceMemoryProperties(pdev, &props);

   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
   for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
      if (props.memoryTypes[i].propertyFlags == want) {
         *out_index = i;
         return true;
      }
   }
   return false;
}

static void
npt_hostmem_init_once(void)
{
   static const char *dev_exts[] = {
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
   };

   hostmem.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!hostmem.lib) {
      npt_log("hostmem: dlopen(libvulkan.so.1) failed: %s", dlerror());
      return;
   }

   PFN_vkGetInstanceProcAddr gipa;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(hostmem.lib, "vkGetInstanceProcAddr");
#pragma GCC diagnostic pop
   PFN_vkCreateInstance CreateInstance =
      gipa ? (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance")
           : NULL;
   if (!CreateInstance)
      goto fail_lib;

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pEngineName = "neptune-hostmem",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkResult vr = CreateInstance(&inst_info, NULL, &hostmem.instance);
   if (vr != VK_SUCCESS) {
      npt_log("hostmem: vkCreateInstance failed (%d)", vr);
      goto fail_lib;
   }
   hostmem.DestroyInstance =
      (PFN_vkDestroyInstance)gipa(hostmem.instance, "vkDestroyInstance");

   VkPhysicalDevice pdev =
      npt_hostmem_pick_physical_device(hostmem.instance, gipa);
   if (!pdev ||
       !npt_hostmem_pick_type(hostmem.instance, gipa, pdev, &hostmem.type_index)) {
      npt_log("hostmem: no physical device with a cached host memory type");
      goto fail_instance;
   }

   PFN_vkCreateDevice CreateDevice =
      (PFN_vkCreateDevice)gipa(hostmem.instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(hostmem.instance, "vkGetDeviceProcAddr");

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = ARRAY_SIZE(dev_exts),
      .ppEnabledExtensionNames = dev_exts,
   };
   vr = CreateDevice(pdev, &dev_info, NULL, &hostmem.device);
   if (vr != VK_SUCCESS) {
      npt_log("hostmem: vkCreateDevice failed (%d)", vr);
      goto fail_instance;
   }

   hostmem.AllocateMemory =
      (PFN_vkAllocateMemory)gdpa(hostmem.device, "vkAllocateMemory");
   hostmem.FreeMemory = (PFN_vkFreeMemory)gdpa(hostmem.device, "vkFreeMemory");
   hostmem.GetMemoryFdKHR =
      (PFN_vkGetMemoryFdKHR)gdpa(hostmem.device, "vkGetMemoryFdKHR");
   hostmem.ready = true;
   return;

fail_instance:
   hostmem.DestroyInstance(hostmem.instance, NULL);
   hostmem.instance = VK_NULL_HANDLE;
fail_lib:
   dlclose(hostmem.lib);
   hostmem.lib = NULL;
}

int
npt_hostmem_alloc_dmabuf(uint64_t size)
{
   call_once(&hostmem_once, npt_hostmem_init_once);
   if (!hostmem.ready)
      return -1;

   VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_info,
      .allocationSize = size,
      .memoryTypeIndex = hostmem.type_index,
   };
   VkDeviceMemory mem;
   VkResult vr = hostmem.AllocateMemory(hostmem.device, &alloc_info, NULL, &mem);
   if (vr != VK_SUCCESS) {
      npt_log("hostmem: vkAllocateMemory(%" PRIu64 ") failed (%d)", size, vr);
      return -1;
   }

   const VkMemoryGetFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = mem,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   int fd = -1;
   vr = hostmem.GetMemoryFdKHR(hostmem.device, &fd_info, &fd);
   if (vr != VK_SUCCESS) {
      npt_log("hostmem: vkGetMemoryFdKHR failed (%d)", vr);
      fd = -1;
   }

   /* The dmabuf holds its own reference on the BO. */
   hostmem.FreeMemory(hostmem.device, mem, NULL);
   return fd;
}
