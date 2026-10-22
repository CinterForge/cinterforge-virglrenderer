/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device_memory.h"

#include <math.h>

#include "vn_protocol_renderer_transport.h"

#include "vkr_device_memory_gen.h"
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util/anon_file.h"
#include "vkr_device.h"
#include "vkr_physical_device.h"
#include "vkr_queue.h"

static bool
vkr_get_fd_info_from_resource_info(const struct vkr_resource *res,
                                   const VkImportMemoryResourceInfoMESA *res_info,
                                   VkImportMemoryFdInfoKHR *out)
{
   VkExternalMemoryHandleTypeFlagBits handle_type;
   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      break;
   default:
      return false;
   }

   int fd = os_dupfd_cloexec(res->u.fd);
   if (fd < 0)
      return false;

   *out = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = res_info->pNext,
      .fd = fd,
      .handleType = handle_type,
   };
   return true;
}

#if defined(HAVE_LINUX_UDMABUF_H) && defined(HAVE_MEMFD_CREATE)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                             const VkMemoryAllocateInfo *alloc_info,
                                             int *out_udmabuf_fd,
                                             VkImportMemoryFdInfoKHR *out_fd_info)
{
   int memfd = -1;
   int udmabuf_fd = -1;
   int fd = -1;

   memfd = memfd_create("vkr-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (memfd < 0) {
      vkr_log("memfd_create failed (%s)", strerror(errno));
      goto fail;
   }

   const size_t size = align(alloc_info->allocationSize, getpagesize());
   int ret = ftruncate(memfd, size);
   if (ret) {
      vkr_log("ftruncate failed (%s)", strerror(errno));
      goto fail;
   }

   ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
   if (ret) {
      vkr_log("fcntl F_ADD_SEALS failed (%s)", strerror(errno));
      goto fail;
   }

   const struct udmabuf_create create = {
      .memfd = memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = size,
   };
   udmabuf_fd = ioctl(physical_dev->udmabuf_dev_fd, UDMABUF_CREATE, &create);
   if (udmabuf_fd < 0) {
      vkr_log("ioctl UDMABUF_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   fd = os_dupfd_cloexec(udmabuf_fd);
   if (fd < 0) {
      vkr_log("os_dupfd_cloexec failed (%s)", strerror(errno));
      goto fail;
   }

   close(memfd);

   *out_udmabuf_fd = udmabuf_fd;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return VK_SUCCESS;

fail:
   if (udmabuf_fd >= 0)
      close(udmabuf_fd);
   if (memfd >= 0)
      close(memfd);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#else  /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

static inline VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(
   UNUSED struct vkr_physical_device *physical_dev,
   UNUSED const VkMemoryAllocateInfo *alloc_info,
   UNUSED int *out_udmabuf_fd,
   UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("udmabuf_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>

#define GBM_BO_USE_SW_READ_RARELY (1 << 10)
#define GBM_BO_USE_SW_WRITE_RARELY (1 << 12)

static inline int
vkr_gbm_bo_get_fd(void *gbm_bo)
{
   assert(gbm_bo);

   /* gbm_bo_get_fd returns negative error code on failure */
   return gbm_bo_get_fd(gbm_bo);
}

static inline void
vkr_gbm_bo_destroy(void *gbm_bo)
{
   gbm_bo_destroy(gbm_bo);
}

static VkResult
vkr_gbm_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                         const VkMemoryAllocateInfo *alloc_info,
                                         void **out_gbm_bo,
                                         VkImportMemoryFdInfoKHR *out_fd_info)
{
   const uint32_t flags =
      GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_RARELY | GBM_BO_USE_SW_WRITE_RARELY;
   struct gbm_bo *gbm_bo;
   int fd = -1;

   assert(physical_dev->gbm_device);

   /*
    * Reject here for simplicity. Letting VkPhysicalDeviceVulkan11Properties return
    * min(maxMemoryAllocationSize, UINT32_MAX) will affect unmappable scenarios.
    */
   if (alloc_info->allocationSize > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Page alignment is used on all implementations we support. */
   const uint32_t alloc_size = align(alloc_info->allocationSize, getpagesize());
#ifdef MINIGBM
   const uint32_t format = GBM_FORMAT_R8;
   const uint32_t width = alloc_size;
   const uint32_t height = 1;
#else
   /* Mesa gbm has texture size limitations, so we can't rely on R8 here. Instead, we
    * allocate a large enough linear rgba8 buffer.
    */
   const uint32_t format = GBM_FORMAT_ABGR8888;
   const uint8_t pixel_bytes = 4;
   const uint32_t width =
      (uint32_t)ceil(sqrt((alloc_size + pixel_bytes - 1) / pixel_bytes));
   const uint32_t height = width;
#endif /* MINIGBM */

   gbm_bo = gbm_bo_create(physical_dev->gbm_device, width, height, format, flags);
   if (!gbm_bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   fd = vkr_gbm_bo_get_fd(gbm_bo);
   if (fd < 0) {
      vkr_gbm_bo_destroy(gbm_bo);
      return fd == -EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *out_gbm_bo = (void *)gbm_bo;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return VK_SUCCESS;
}

#else

static inline int
vkr_gbm_bo_get_fd(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
   return -1;
}

static inline void
vkr_gbm_bo_destroy(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
}

static inline VkResult
vkr_gbm_get_fd_info_from_allocation_info(UNUSED struct vkr_physical_device *physical_dev,
                                         UNUSED const VkMemoryAllocateInfo *alloc_info,
                                         UNUSED void **out_gbm_bo,
                                         UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("minigbm_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* ENABLE_GBM_ALLOCATION */

static VkResult
vkr_shm_alloc(uint64_t size, struct vkr_shm **out_shm)
{
   const size_t page_size = getpagesize();
   const size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

   struct vkr_shm *shm = calloc(1, sizeof(*shm));
   if (!shm)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   shm->fd = os_create_anonymous_file(aligned_size, "vkr-host-mem");
   if (shm->fd < 0) {
      const int err = errno;
      vkr_log_errno(err, "creating a %zu-byte shm for host memory", aligned_size);
      free(shm);
      return vkr_result_from_errno(err);
   }

   shm->ptr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
   if (shm->ptr == MAP_FAILED) {
      vkr_log_errno(errno, "mapping a %zu-byte shm for host memory", aligned_size);
      close(shm->fd);
      free(shm);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   shm->size = aligned_size;
   *out_shm = shm;
   return VK_SUCCESS;
}

/* Map the shm behind a guest-imported resource so it can be imported into the
 * driver as host memory.  The mapping and fd are our own, so the memory stays
 * valid if the resource is destroyed first, and exporting the memory again
 * hands out the same shm.
 */
static struct vkr_shm *
vkr_shm_import(const struct vkr_physical_device *physical_dev,
               const struct vkr_resource *res,
               uint64_t size)
{
   if (res->shm_fd < 0) {
      vkr_log("shm resource %u has no fd to import", res->res_id);
      return NULL;
   }

   const uint64_t alignment =
      MAX2((uint64_t)getpagesize(), physical_dev->min_imported_host_pointer_alignment);
   const uint64_t aligned_size = (size + alignment - 1) / alignment * alignment;
   struct stat st;
   if (fstat(res->shm_fd, &st) || st.st_size < 0 || (uint64_t)st.st_size < aligned_size) {
      vkr_log("shm resource %u (%lld bytes) is too small to import %" PRIu64
              " bytes with alignment %" PRIu64,
              res->res_id, (long long)st.st_size, size, alignment);
      return NULL;
   }

   struct vkr_shm *shm = calloc(1, sizeof(*shm));
   if (!shm)
      return NULL;

   shm->fd = os_dupfd_cloexec(res->shm_fd);
   if (shm->fd < 0) {
      vkr_log_errno(errno, "duplicating the fd of shm resource %u", res->res_id);
      free(shm);
      return NULL;
   }

   shm->ptr =
      mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
   if (shm->ptr == MAP_FAILED) {
      vkr_log("failed to map shm resource %u (%s)", res->res_id, strerror(errno));
      close(shm->fd);
      free(shm);
      return NULL;
   }
   shm->size = aligned_size;

   if ((uintptr_t)shm->ptr % alignment) {
      vkr_log("shm resource %u mapped at %p, not aligned to %" PRIu64, res->res_id,
              shm->ptr, alignment);
      munmap(shm->ptr, shm->size);
      close(shm->fd);
      free(shm);
      return NULL;
   }

   return shm;
}

static void
vkr_shm_free(struct vkr_shm *shm)
{
   if (!shm)
      return;

   munmap(shm->ptr, shm->size);
   close(shm->fd);
   free(shm);
}

static VkResult
vkr_get_host_pointer_memory_type_bits(struct vkr_device *dev,
                                      void *ptr,
                                      uint32_t *out_bits)
{
   VkMemoryHostPointerPropertiesEXT host_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
   };
   const VkResult ret = dev->GetMemoryHostPointerPropertiesEXT(
      dev->base.handle.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      ptr, &host_props);
   *out_bits = host_props.memoryTypeBits;
   return ret;
}

static const char *
vkr_device_memory_import_kind(const VkMemoryAllocateInfo *alloc_info)
{
   for (const VkBaseInStructure *s = alloc_info->pNext; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA)
         return "resource";
      if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR)
         return "fd";
      if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT)
         return "host pointer";
   }
   return "none";
}

static void
vkr_device_memory_allocate(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkAllocateMemory *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_physical_device *physical_dev = dev->physical_device;

   VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)args->pAllocateInfo;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   if (unlikely(mem_type_index >= physical_dev->memory_properties.memoryTypeCount)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   VkExportMemoryAllocateInfo *export_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
   uint32_t valid_fd_types = 0;
   struct vkr_shm *shm = NULL;
   VkImportMemoryHostPointerInfoEXT local_host_import;

   /* translate VkImportMemoryResourceInfoMESA into VkImportMemoryFdInfoKHR, or
    * into VkImportMemoryHostPointerInfoEXT for shm resources, in place
    */
   VkImportMemoryFdInfoKHR local_import_info = { .fd = -1 };
   VkImportMemoryResourceInfoMESA *res_info = NULL;
   void *prev_of_res_info = vkr_find_prev_struct(
      alloc_info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   if (prev_of_res_info) {
      res_info = (VkImportMemoryResourceInfoMESA *)vkr_pnext_get_next(prev_of_res_info);
      struct vkr_resource *res = vkr_context_get_resource(ctx, res_info->resourceId);
      if (!res) {
         vkr_log("failed to import resource: invalid res_id %u", res_info->resourceId);
         vkr_context_set_fatal(ctx);
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      if (res->fd_type == VIRGL_RESOURCE_FD_SHM && physical_dev->is_dma_buf_emulated) {
         /* What the guest sees as a dma_buf is shm here (memory exported by
          * this or another context, or created by the VMM).  The driver
          * cannot import an fd, so map the shm and import it as host memory.
          */
         shm = vkr_shm_import(physical_dev, res, alloc_info->allocationSize);
         if (!shm) {
            args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            return;
         }

         uint32_t type_bits;
         args->ret = vkr_get_host_pointer_memory_type_bits(dev, shm->ptr, &type_bits);
         if (args->ret == VK_SUCCESS && !type_bits) {
            vkr_log("shm resource %u cannot be imported as host memory",
                    res_info->resourceId);
            args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         }
         if (args->ret != VK_SUCCESS) {
            vkr_shm_free(shm);
            return;
         }
         if (!(type_bits & (1u << mem_type_index))) {
            /* Only the driver memory type changes; the guest keeps seeing the
             * memory type it asked for.
             */
            alloc_info->memoryTypeIndex = ffs(type_bits) - 1;
            vkr_log("shm resource %u imported with memory type %u instead of %u",
                    res_info->resourceId, alloc_info->memoryTypeIndex, mem_type_index);
         }

         local_host_import = (VkImportMemoryHostPointerInfoEXT){
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
            .pNext = res_info->pNext,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            .pHostPointer = shm->ptr,
         };
         vkr_pnext_set_next(prev_of_res_info, &local_host_import);
         alloc_info->allocationSize = shm->size;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;

         if (export_info) {
            /* the memory is shm-backed; the guest's dma_buf export is emulated */
            void *prev_of_export_info = vkr_find_prev_struct(
               alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
            vkr_pnext_set_next(prev_of_export_info, export_info->pNext);
            export_info = NULL;
         }
      } else {
         if (!vkr_get_fd_info_from_resource_info(res, res_info, &local_import_info)) {
            args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            return;
         }

         vkr_pnext_set_next(prev_of_res_info, &local_import_info);
      }
   }

   /* track if driver has requested export allocation */
   const bool might_export = export_info && export_info->handleTypes;

   /* XXX Force dma_buf/opaque fd export or gbm bo import until a new extension that
    * supports direct export from host visible memory
    *
    * Most VkImage and VkBuffer are non-external while most VkDeviceMemory are external
    * if allocated with a host visible memory type. We still violate the spec by binding
    * external memory to non-external image or buffer, which needs spec changes with a
    * new extension.
    *
    * Skip forcing external if a valid VkImportMemoryResourceInfoMESA is provided, since
    * the mapping will be directly set up from the existing virgl resource.
    */
   const uint32_t property_flags =
      physical_dev->memory_properties.memoryTypes[mem_type_index].propertyFlags;
   int udmabuf_fd = -1;
   void *gbm_bo = NULL;
   VkExportMemoryAllocateInfo local_export_info;

   /* Importing host memory is the only way this host can hand memory out as an
    * fd, so it has to cover everything the guest may want an fd for: host
    * visible memory, which is always made exportable below, and any allocation
    * the guest itself asked to export.  The latter is only ever a dma_buf
    * request -- dma_buf is emulated on top of VK_EXT_external_memory_host --
    * and is otherwise left to fall through to a host that supports no fd handle
    * type at all.
    */
   const bool force_shm_import = physical_dev->is_dma_buf_emulated && !res_info &&
                                 ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
                                  might_export);

   if (force_shm_import) {
      if (export_info) {
         /* Strip export info since valid_fd_types can only be shm here.  The
          * guest asks for a dma_buf export when dma_buf is emulated, which
          * the host can neither honour nor be shown.
          */
         VkBaseInStructure *prev_of_export_info = vkr_find_prev_struct(
            alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

         prev_of_export_info->pNext = export_info->pNext;
         export_info = NULL;
      }

      /* Allocate shm and import it as host memory. */
      args->ret = vkr_shm_alloc(alloc_info->allocationSize, &shm);
      if (args->ret != VK_SUCCESS)
         return;

      uint32_t type_bits;
      args->ret = vkr_get_host_pointer_memory_type_bits(dev, shm->ptr, &type_bits);
      if (args->ret == VK_SUCCESS && !(type_bits & (1u << mem_type_index))) {
         /* Some types cannot hold host memory: lazily allocated ones (Metal memoryless),
          * and every device-local-only type with a MoltenVK lacking PR #2834.  A type
          * with the same properties (apart from lazy allocation) that can is just as
          * good.  Only the driver memory type changes; the guest keeps seeing the memory
          * type it asked for.
          */
         const VkMemoryPropertyFlags want =
            property_flags & ~VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
         const VkPhysicalDeviceMemoryProperties *props = &physical_dev->memory_properties;
         uint32_t substitute = UINT32_MAX;
         for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) &&
                (props->memoryTypes[i].propertyFlags & want) == want) {
               substitute = i;
               break;
            }
         }
         if (substitute != UINT32_MAX) {
            alloc_info->memoryTypeIndex = substitute;
            vkr_log("host memory import is unsupported for memory type %u; allocated "
                    "with memory type %u",
                    mem_type_index, substitute);
         } else {
            vkr_log("host memory import is unsupported for memory type %u",
                    mem_type_index);
            args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         }
      }
      if (args->ret != VK_SUCCESS) {
         vkr_shm_free(shm);
         return;
      }

      local_host_import = (VkImportMemoryHostPointerInfoEXT){
         .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
         .pNext = alloc_info->pNext,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
         .pHostPointer = shm->ptr,
      };
      alloc_info->pNext = &local_host_import;
      alloc_info->allocationSize = shm->size;
      valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;
   } else if ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !res_info) {
      /* An implementation can support dma_buf import along with opaque fd export/import.
       * If the client driver is using external memory and requesting dma_buf, without
       * dma_buf fd export support, we must use gbm bo import path instead of forcing
       * opaque fd export. e.g. the client driver uses external memory for wsi image.
       */
      const bool no_dma_buf_export =
         !export_info ||
         !(export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      const bool force_gbm_import = !!physical_dev->gbm_device;
      const bool force_udmabuf_import = physical_dev->udmabuf_dev_fd >= 0;
      if (!(force_gbm_import || force_udmabuf_import) &&
          (physical_dev->is_dma_buf_fd_export_supported ||
           (physical_dev->is_opaque_fd_export_supported && no_dma_buf_export))) {
         const VkExternalMemoryHandleTypeFlagBits handle_type =
            physical_dev->is_dma_buf_fd_export_supported
               ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         if (export_info) {
            export_info->handleTypes |= handle_type;
         } else {
            local_export_info = (const VkExportMemoryAllocateInfo){
               .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
               .pNext = alloc_info->pNext,
               .handleTypes = handle_type,
            };
            export_info = &local_export_info;
            alloc_info->pNext = &local_export_info;

            if (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
               /* Guest virtgpu kernel aligns up blob mem size to the page boundary. No
                * matter dma-buf or opaque fd export allocation, the actual allocation
                * in most cases would follow the same padding. For dma-buf, we are able
                * to allocate and validate via lseek below. For opaque fd, Vulkan spec
                * requires to use the same size with allocation time for the mapper,
                * either vkr_allocator or vulkano, to import and populate the mapping.
                * Accessible mapping given by vkMapMemory is normally limited to just the
                * alloc size. When KVM register the mappings to guest pci bar, that
                * involves an invalid chunk towards the end of the page boundary. As a
                * result, any guest side accelerated instructions that rely on the
                * paddings can end up with Illegal instruction error. The most common
                * trigger is via memcpy'ing valid range to the guest side mapped buffer
                * memory, and then, e.g. __memcpy_avx_unaligned_erms  can hit the error.
                */
               alloc_info->allocationSize =
                  align(alloc_info->allocationSize, getpagesize());
            }
         }
      } else if (physical_dev->EXT_external_memory_dma_buf) {
         /* Allocate dma_buf externally and force to import. */
         if (export_info) {
            /* Strip export info since valid_fd_types can only be dma_buf here. */
            void *prev_of_export_info = vkr_find_prev_struct(
               alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

            vkr_pnext_set_next(prev_of_export_info, export_info->pNext);
            export_info = NULL;
         }

         if (force_udmabuf_import) {
            /* To be noted, there are 2 limits for udmabuf:
             * - list_limit: udmabuf_create_list->count limit. Default is 1024.
             * - size_limit_mb: Max size of a dmabuf, in megabytes. Default is 64.
             */
            args->ret = vkr_udmabuf_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &udmabuf_fd, &local_import_info);
         } else {
            args->ret = vkr_gbm_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &gbm_bo, &local_import_info);
         }
         if (args->ret != VK_SUCCESS)
            return;

         alloc_info->pNext = &local_import_info;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      }
   }

   if (export_info) {
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_OPAQUE;
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_DMABUF;
   }

   struct vkr_device_memory *mem = vkr_device_memory_create_and_add(ctx, args);
   if (!mem) {
      if (local_import_info.fd >= 0)
         close(local_import_info.fd);
      if (gbm_bo)
         vkr_gbm_bo_destroy(gbm_bo);
      vkr_shm_free(shm);
      return;
   }

   mem->device = dev;
   mem->might_export = might_export;
   mem->property_flags = property_flags;
   mem->valid_fd_types = valid_fd_types;
   mem->udmabuf_fd = udmabuf_fd;
   mem->gbm_bo = gbm_bo;
   mem->shm = shm;
   mem->allocation_size = alloc_info->allocationSize;
   mem->memory_type_index = mem_type_index;
}

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;

   /* captured before the allocate info is rewritten */
   const VkMemoryAllocateInfo *alloc_info = args->pAllocateInfo;
   const VkDeviceSize size = alloc_info->allocationSize;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   const char *import_kind = vkr_device_memory_import_kind(alloc_info);
   const VkImportMemoryResourceInfoMESA *res_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   const uint32_t res_id = res_info ? res_info->resourceId : 0;
   const VkExportMemoryAllocateInfo *export_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
   const VkExternalMemoryHandleTypeFlags export_types =
      export_info ? export_info->handleTypes : 0;

   args->ret = VK_SUCCESS;
   vkr_device_memory_allocate(dispatch, args);

   if (args->ret != VK_SUCCESS) {
      const struct vkr_resource *res = res_id ? vkr_context_get_resource(ctx, res_id) : NULL;
      /* Venus submits vkAllocateMemory without waiting for the reply (Mesa's
       * vn_device_memory_alloc_simple; only VN_PERF=no_async_mem_alloc waits): the guest
       * already handed the memory to the application, and nothing in the protocol tells
       * it the allocation failed.  Binding, mapping or exporting it references an object
       * that does not exist, which is fatal; a stand-in object would let the GPU access
       * memory that is not there.  Only freeing it is tolerated (vkr_dispatch_command).
       */
      vkr_log("vkAllocateMemory failed: VkResult %d, size %" PRIu64
              ", memory type %u, import %s (res %u fd_type %d), export types 0x%x; memory "
              "%" PRIu64 " does not exist and the guest, which does not wait for the "
              "result, loses its context when it uses it",
              args->ret, (uint64_t)size, mem_type_index, import_kind, res_id,
              res ? (int)res->fd_type : -1, export_types,
              (uint64_t)vkr_cs_handle_load_id((const void **)args->pMemory,
                                              VK_OBJECT_TYPE_DEVICE_MEMORY));
   }
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
   if (!mem)
      return;

   /* The driver may reference the shm pages until the VkDeviceMemory is
    * freed, so free the driver handle before releasing the backing.
    */
   vkr_device_memory_destroy_driver_handle(ctx, args);
   vkr_device_memory_release(mem);
   vkr_device_remove_object(ctx, dev, &mem->base);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vk->GetDeviceMemoryCommitment(args->device, args->memory,
                                 args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_resource *res = vkr_context_get_resource(ctx, args->resourceId);
   if (!res) {
      vkr_log("failed to query resource props: invalid res_id %u", args->resourceId);
      vkr_context_set_fatal(ctx);
      return;
   }

   if (res->fd_type == VIRGL_RESOURCE_FD_SHM && dev->physical_device->is_dma_buf_emulated) {
      /* imported as host memory by vkAllocateMemory; res->u.data is a page
       * aligned mapping of the same shm
       */
      uint32_t type_bits;
      args->ret = vkr_get_host_pointer_memory_type_bits(dev, res->u.data, &type_bits);
      if (args->ret != VK_SUCCESS)
         return;

      args->pMemoryResourceProperties->memoryTypeBits = type_bits;

      VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
         vkr_find_struct(args->pMemoryResourceProperties->pNext,
                         VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
      if (alloc_size_props)
         alloc_size_props->allocationSize = res->size;
      return;
   }

   if (res->fd_type != VIRGL_RESOURCE_FD_DMABUF) {
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   static const VkExternalMemoryHandleTypeFlagBits handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   VkMemoryFdPropertiesKHR mem_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);
   args->ret =
      vk->GetMemoryFdPropertiesKHR(args->device, handle_type, res->u.fd, &mem_fd_props);
   if (args->ret != VK_SUCCESS)
      return;

   args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;

   VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
      vkr_find_struct(args->pMemoryResourceProperties->pNext,
                      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = res->size;
}

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges = NULL;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges = NULL;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;
}

/* release batching: one marker per queue per batch */
#define VKR_SHM_RELEASE_BATCH_NS (2 * 1000 * 1000)
#define VKR_SHM_RELEASE_BATCH_SIZE ((size_t)64 << 20)

static int64_t
vkr_shm_release_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void
vkr_shm_free_list(struct list_head *shms)
{
   list_for_each_entry_safe (struct vkr_shm, shm, shms, head)
      vkr_shm_free(shm);
   list_inithead(shms);
}

/* Hand the pending shm to the queues: each gets a marker after what it has
 * been submitted so far, and the shm is unmapped when all markers completed.
 * Unless forced, pending shm is batched for VKR_SHM_RELEASE_BATCH_NS or up to
 * VKR_SHM_RELEASE_BATCH_SIZE.  Called after frees and submissions.
 */
void
vkr_device_flush_shm_releases(struct vkr_device *dev, bool force)
{
   mtx_lock(&dev->shm_release.mutex);
   if (list_is_empty(&dev->shm_release.pending)) {
      mtx_unlock(&dev->shm_release.mutex);
      return;
   }
   const int64_t now = vkr_shm_release_now_ns();
   if (!force && now - dev->shm_release.last_flush_ns < VKR_SHM_RELEASE_BATCH_NS &&
       dev->shm_release.pending_size < VKR_SHM_RELEASE_BATCH_SIZE) {
      mtx_unlock(&dev->shm_release.mutex);
      return;
   }

   struct vkr_shm_release *release = malloc(sizeof(*release));
   if (!release) {
      mtx_unlock(&dev->shm_release.mutex);
      return;
   }
   release->device = dev;
   list_inithead(&release->shms);
   list_splicetail(&dev->shm_release.pending, &release->shms);
   list_inithead(&dev->shm_release.pending);
   dev->shm_release.pending_size = 0;
   dev->shm_release.last_flush_ns = now;

   /* one reference for this function, released below */
   release->pending_queues = 1;
   list_for_each_entry (struct vkr_queue, queue, &dev->queues, base.track_head)
      release->pending_queues++;
   mtx_unlock(&dev->shm_release.mutex);

   /* The markers are submitted outside the lock.  Queues are only removed at
    * device destruction, which does not race with frees or submissions.
    */
   list_for_each_entry (struct vkr_queue, queue, &dev->queues, base.track_head) {
      if (!vkr_queue_submit_shm_release(queue, release))
         vkr_shm_release_complete_queue(release);
   }
   vkr_shm_release_complete_queue(release);
}

void
vkr_shm_release_complete_queue(struct vkr_shm_release *release)
{
   struct vkr_device *dev = release->device;

   mtx_lock(&dev->shm_release.mutex);
   const bool done = !--release->pending_queues;
   mtx_unlock(&dev->shm_release.mutex);

   if (done) {
      vkr_shm_free_list(&release->shms);
      free(release);
   }
}

/* The device is idle and about to be destroyed: nothing can use the shm
 * anymore.  Markers still queued are retired with the queues.
 */
void
vkr_device_fini_shm_releases(struct vkr_device *dev)
{
   mtx_lock(&dev->shm_release.mutex);
   dev->shm_release.immediate = true;
   vkr_shm_free_list(&dev->shm_release.pending);
   dev->shm_release.pending_size = 0;
   mtx_unlock(&dev->shm_release.mutex);
}

void
vkr_device_memory_release(struct vkr_device_memory *mem)
{
   if (mem->shm) {
      struct vkr_device *dev = mem->device;

      mtx_lock(&dev->shm_release.mutex);
      const bool immediate = dev->shm_release.immediate;
      if (!immediate) {
         list_addtail(&mem->shm->head, &dev->shm_release.pending);
         dev->shm_release.pending_size += mem->shm->size;
      }
      mtx_unlock(&dev->shm_release.mutex);

      if (immediate)
         vkr_shm_free(mem->shm);
      else
         vkr_device_flush_shm_releases(dev, false);
      mem->shm = NULL;
   }
   if (mem->gbm_bo)
      vkr_gbm_bo_destroy(mem->gbm_bo);
   if (mem->udmabuf_fd >= 0)
      close(mem->udmabuf_fd);
}

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob)
{
   TRACE_FUNC();

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported) {
      vkr_log("mem has been exported");
      return false;
   }

   uint32_t map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (!visible) {
         vkr_log("mem cannot support mappable blob");
         return false;
      }

      /* XXX guessed */
      map_info = (coherent && cached) ? VIRGL_RENDERER_MAP_CACHE_CACHED
                                      : VIRGL_RENDERER_MAP_CACHE_WC;
   }

   if (mem->shm) {
      const int fd = os_dupfd_cloexec(mem->shm->fd);
      if (fd < 0) {
         vkr_log_errno(errno, "exporting memory %" PRIu64 " as a blob (dup of its shm fd)",
                       (uint64_t)mem->base.id);
         return false;
      }
      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = fd,
         .map_info = map_info,
      };
      return true;
   }

   const bool can_export_dma_buf = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF);
   const bool can_export_opaque = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE);
   enum virgl_resource_fd_type fd_type;
   VkExternalMemoryHandleTypeFlagBits handle_type;
   struct virgl_resource_vulkan_info vulkan_info;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!can_export_dma_buf) {
         vkr_log("mem cannot export to dma_buf for cross device blob sharing");
         return false;
      }
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_dma_buf) {
      /* prefer dmabuf for easier mapping? */
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_opaque) {
      /* prefer opaque for performance? */
      fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

      STATIC_ASSERT(sizeof(vulkan_info.device_uuid) == VK_UUID_SIZE);
      STATIC_ASSERT(sizeof(vulkan_info.driver_uuid) == VK_UUID_SIZE);

      const VkPhysicalDeviceIDProperties *id_props =
         &mem->device->physical_device->id_properties;
      memcpy(vulkan_info.device_uuid, id_props->deviceUUID, VK_UUID_SIZE);
      memcpy(vulkan_info.driver_uuid, id_props->driverUUID, VK_UUID_SIZE);

      vulkan_info.allocation_size = mem->allocation_size;
      vulkan_info.memory_type_index = mem->memory_type_index;
   } else {
      vkr_log("mem is not exportable");
      return false;
   }

   int fd;
   if (mem->udmabuf_fd >= 0) {
      fd = os_dupfd_cloexec(mem->udmabuf_fd);
      if (fd < 0) {
         vkr_log("mem udmabuf fd dup failed (%s)", strerror(errno));
         return false;
      }
   } else if (mem->gbm_bo) {
      assert(handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      assert(can_export_dma_buf && !can_export_opaque);

      fd = vkr_gbm_bo_get_fd(mem->gbm_bo);
      if (fd < 0) {
         vkr_log("mem gbm bo export failed (ret %d)", fd);
         return false;
      }
   } else {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      const VkMemoryGetFdInfoKHR fd_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = mem->base.handle.device_memory,
         .handleType = handle_type,
      };
      VkResult ret = vk->GetMemoryFdKHR(mem->device->base.handle.device, &fd_info, &fd);
      if (ret != VK_SUCCESS) {
         vkr_log("mem fd export failed (vk ret %d)", ret);
         return false;
      }
   }

   if (fd_type == VIRGL_RESOURCE_FD_DMABUF) {
      const off_t dma_buf_size = lseek(fd, 0, SEEK_END);
      if (dma_buf_size < 0 || (uint64_t)dma_buf_size < blob_size) {
         vkr_log("mem dma_buf_size %lld < blob_size %" PRIu64, (long long)dma_buf_size,
                 blob_size);
         close(fd);
         return false;
      }
   }

   mem->exported = true;

   *out_blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   return true;
}
