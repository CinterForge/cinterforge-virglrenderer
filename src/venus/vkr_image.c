/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_image.h"

#include "vkr_image_gen.h"
#include "vkr_physical_device.h"

/* See vkr_buffer_fix_create_info. */
static void
vkr_image_fix_create_info(struct vkr_device *dev, VkImageCreateInfo *pCreateInfo)
{
   if (!dev->physical_device->is_dma_buf_emulated)
      return;

   VkExternalMemoryImageCreateInfo *ext_create_info =
      vkr_find_struct(pCreateInfo, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   if (ext_create_info)
      ext_create_info->handleTypes &= ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
}

static VkResult
vkr_image_fix_drm_format(struct vkr_device *dev,
                         VkImageCreateInfo *pCreateInfo)
{
   const VkImageDrmFormatModifierExplicitCreateInfoEXT* drm_format_info =
            vkr_find_struct(pCreateInfo, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
   const VkImageDrmFormatModifierListCreateInfoEXT* drm_format_list =
            vkr_find_struct(pCreateInfo, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);

   if (pCreateInfo->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT || (!drm_format_info && !drm_format_list)) {
      return VK_SUCCESS;
   }

   if (drm_format_info && drm_format_info->drmFormatModifier == DRM_FORMAT_MOD_LINEAR) {
      pCreateInfo->tiling = VK_IMAGE_TILING_LINEAR;

      /* An imported linear buffer (a dma-buf from another device or process) comes
       * with its own plane layout, but the host decides the layout of a linear
       * image itself.  Refuse layouts it would not reproduce instead of reading the
       * buffer with the wrong pitch.
       */
      struct vn_device_proc_table *vk = &dev->proc_table;
      if (drm_format_info->drmFormatModifierPlaneCount != 1)
         return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
      if (vk->GetDeviceImageSubresourceLayout) {
         const VkSubresourceLayout *want = &drm_format_info->pPlaneLayouts[0];
         const VkImageSubresource2 subres = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT },
         };
         const VkDeviceImageSubresourceInfo info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO,
            .pCreateInfo = pCreateInfo,
            .pSubresource = &subres,
         };
         VkSubresourceLayout2 have = { .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2 };
         vk->GetDeviceImageSubresourceLayout(dev->base.handle.device, &info, &have);
         if (want->offset != have.subresourceLayout.offset ||
             want->rowPitch != have.subresourceLayout.rowPitch) {
            vkr_log("linear plane layout offset %" PRIu64 " pitch %" PRIu64
                    " differs from the host's offset %" PRIu64 " pitch %" PRIu64,
                    (uint64_t)want->offset, (uint64_t)want->rowPitch,
                    (uint64_t)have.subresourceLayout.offset,
                    (uint64_t)have.subresourceLayout.rowPitch);
            return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
         }
      }
      return VK_SUCCESS;
   }

   for (int i = 0; drm_format_list && i < drm_format_list->drmFormatModifierCount; i++) {
      if (drm_format_list->pDrmFormatModifiers[i] == DRM_FORMAT_MOD_LINEAR) {
         pCreateInfo->tiling = VK_IMAGE_TILING_LINEAR;
         return VK_SUCCESS;
      }
   }

   vkr_log("only DRM_FORMAT_MOD_LINEAR is supported");
   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static VkResult
vkr_image_emulate_drm_format_modifier_properties(UNUSED struct vkr_device *dev,
                                                 UNUSED VkImage image,
                                                 VkImageDrmFormatModifierPropertiesEXT* pProperties)
{
   pProperties->drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
   return VK_SUCCESS;
}

/* Metal asserts (aborting the whole VMM, not just this context) on a texture over its
 * size limits, and MoltenVK passes the extent through.  An image the device limits do
 * not allow is invalid usage; refuse it like a failed creation instead.
 */
static bool
vkr_image_within_limits(const struct vkr_device *dev, const VkImageCreateInfo *info)
{
   const VkPhysicalDeviceLimits *limits = &dev->physical_device->properties.limits;
   const VkExtent3D *e = &info->extent;
   uint32_t max_dim;
   switch (info->imageType) {
   case VK_IMAGE_TYPE_1D:
      max_dim = limits->maxImageDimension1D;
      break;
   case VK_IMAGE_TYPE_2D:
      max_dim = (info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
                   ? MIN2(limits->maxImageDimension2D, limits->maxImageDimensionCube)
                   : limits->maxImageDimension2D;
      break;
   default:
      max_dim = limits->maxImageDimension3D;
      break;
   }

   const uint32_t largest = MAX3(e->width, e->height, e->depth);
   const bool ok = e->width && e->height && e->depth && largest <= max_dim &&
                   info->arrayLayers && info->arrayLayers <= limits->maxImageArrayLayers &&
                   info->mipLevels && info->mipLevels <= util_logbase2(largest) + 1;
   if (!ok) {
      vkr_log("image %ux%ux%u (type %d, %u layers, %u levels) exceeds the device limits; "
              "not created",
              e->width, e->height, e->depth, info->imageType, info->arrayLayers,
              info->mipLevels);
   }
   return ok;
}

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);

   vkr_image_fix_create_info(dev, (VkImageCreateInfo *)args->pCreateInfo);

   if (!vkr_image_within_limits(dev, args->pCreateInfo)) {
      args->ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return;
   }

   if (!dev->physical_device->EXT_image_drm_format_modifier) {
      args->ret = vkr_image_fix_drm_format(dev, (VkImageCreateInfo *)args->pCreateInfo);
      if (args->ret != VK_SUCCESS) {
         return;
      }
   }

   /* XXX If VkExternalMemoryImageCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the image, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * The discussions in vkr_dispatch_vkCreateBuffer are applicable to both
    * buffers and images.  Additionally, drivers usually use
    * VkExternalMemoryImageCreateInfo to pick a well-defined image layout for
    * interoperability with foreign queues.  However, a well-defined layout
    * might not exist for some images.  When it does, it might still require a
    * dedicated allocation or might have a degraded performance.
    *
    * On the other hand, binding an external memory to an image created
    * without VkExternalMemoryImageCreateInfo usually works.  Yes, it will
    * explode if the external memory is accessed by foreign queues due to the
    * lack of a well-defined image layout.  But we never end up in that
    * situation because the app does not consider the memory external.
    */

   vkr_image_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
   vkr_image_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vk->GetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vk->GetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vk->GetImageSparseMemoryRequirements(args->device, args->image,
                                        args->pSparseMemoryRequirementCount,
                                        args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vk->GetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                         args->pSparseMemoryRequirementCount,
                                         args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkBindImageMemory_args_handle(args);
   args->ret =
      vk->BindImageMemory(args->device, args->image, args->memory, args->memoryOffset);
}

static void
vkr_dispatch_vkBindImageMemory2(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkBindImageMemory2_args_handle(args);
   args->ret = vk->BindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vk->GetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                                 args->pLayout);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout2_args_handle(args);
   vk->GetImageSubresourceLayout2(args->device, args->image, args->pSubresource,
                                  args->pLayout);
}

static void
vkr_dispatch_vkGetDeviceImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vkr_image_fix_create_info(dev, (VkImageCreateInfo *)args->pInfo->pCreateInfo);

   vn_replace_vkGetDeviceImageSubresourceLayout_args_handle(args);
   vk->GetDeviceImageSubresourceLayout(args->device, args->pInfo, args->pLayout);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);

   if (dev->physical_device->EXT_image_drm_format_modifier) {
      args->ret = vk->GetImageDrmFormatModifierPropertiesEXT(args->device, args->image,
                                                            args->pProperties);
   } else {
      args->ret = vkr_image_emulate_drm_format_modifier_properties(dev, args->image,
                                                                   args->pProperties);
   }
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
   vkr_image_view_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
   vkr_image_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   vkr_sampler_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   vkr_sampler_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vkr_image_fix_create_info(dev, (VkImageCreateInfo *)args->pInfo->pCreateInfo);

   vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(args);
   vk->GetDeviceImageMemoryRequirements(args->device, args->pInfo,
                                        args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vkr_image_fix_create_info(dev, (VkImageCreateInfo *)args->pInfo->pCreateInfo);

   vn_replace_vkGetDeviceImageSparseMemoryRequirements_args_handle(args);
   vk->GetDeviceImageSparseMemoryRequirements(args->device, args->pInfo,
                                              args->pSparseMemoryRequirementCount,
                                              args->pSparseMemoryRequirements);
}

void
vkr_context_init_image_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;
   dispatch->dispatch_vkGetImageSubresourceLayout2 =
      vkr_dispatch_vkGetImageSubresourceLayout2;
   dispatch->dispatch_vkGetDeviceImageSubresourceLayout =
      vkr_dispatch_vkGetDeviceImageSubresourceLayout;

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;
   dispatch->dispatch_vkGetDeviceImageMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageMemoryRequirements;
   dispatch->dispatch_vkGetDeviceImageSparseMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements;
}

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;
}

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;
}

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;
}
