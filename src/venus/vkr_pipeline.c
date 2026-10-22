/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_pipeline.h"

#include "vkr_pipeline_gen.h"

static void
vkr_dispatch_vkCreateShaderModule(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCreateShaderModule *args)
{
   struct vkr_context *ctx = dispatch->data;

   /* Reject invalid codeSize.
    *
    * VkShaderModuleCreateInfo is unique in the Vulkan API (as of 2023-08-22).
    * Except in rare cases, (see the `altlen` attribute in vk.xml), for each
    * typed non-void array in Vulkan, the api specifies the array length as the
    * count of array elements. But VkShaderModuleCreateInfo has a typed array
    * (uint32_t *pCode) whose length (codeSize) is specified in bytes, not as
    * a count of uint32_t.
    *
    * Also, the Vulkan 1.3.261 spec seems confused about the size of `pCode`.
    * The spec says "codeSize is the size, in bytes, of the code pointed to by
    * pCode", and then later says "pCode must be a valid pointer to an array of
    * codeSize/4 uint32_t values".
    *
    * (FWIW, VkShaderCreateInfoEXT learned from this mistake and declared the
    * array to be typeless, `void *pCode`).
    *
    * The venus encoder/decoder believes the array size is `4 * (codeSize / 4)`
    * because the vk.xml says so. For example, if codeSize is 259, then venus
    * encodes/decodes only 256 bytes. But the native driver may try to read all
    * 259 bytes, leading to out-of-bound access. Prevent the oob access by
    * validating codeSize here.
    */
   if (args->pCreateInfo->codeSize % 4 != 0) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vkr_shader_module_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyShaderModule(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkDestroyShaderModule *args)
{
   vkr_shader_module_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineLayout(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreatePipelineLayout *args)
{
   vkr_pipeline_layout_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyPipelineLayout(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyPipelineLayout *args)
{
   vkr_pipeline_layout_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineCache(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCreatePipelineCache *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);

   struct vkr_pipeline_cache *obj = vkr_context_alloc_object(
      ctx, sizeof(*obj), VK_OBJECT_TYPE_PIPELINE_CACHE, args->pPipelineCache);
   if (!obj) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }

   /* Venus creates pipeline caches asynchronously, so a failure here kills the
    * context at the next use of the cache.  Initial data is only a hint, and
    * the driver may reject it (MoltenVK fails the cache when cached MSL does
    * not compile): create the cache empty instead.
    */
   if (vkr_pipeline_cache_create_driver_handle(ctx, args, obj) != VK_SUCCESS &&
       args->pCreateInfo->initialDataSize) {
      vkr_log("pipeline cache %" PRIu64 ": initial data (%zu bytes) rejected by the "
              "driver (VkResult %d), creating it empty",
              obj->base.id, args->pCreateInfo->initialDataSize, args->ret);

      VkPipelineCacheCreateInfo empty_info = *args->pCreateInfo;
      empty_info.initialDataSize = 0;
      empty_info.pInitialData = NULL;
      /* args->device is the driver handle now */
      args->ret = dev->proc_table.CreatePipelineCache(args->device, &empty_info, NULL,
                                                      &obj->base.handle.pipeline_cache);
   }
   if (args->ret != VK_SUCCESS) {
      free(obj);
      return;
   }

   vkr_device_add_object(ctx, dev, &obj->base);
}

static void
vkr_dispatch_vkDestroyPipelineCache(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkDestroyPipelineCache *args)
{
   vkr_pipeline_cache_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetPipelineCacheData(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkGetPipelineCacheData *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetPipelineCacheData_args_handle(args);
   args->ret = vk->GetPipelineCacheData(args->device, args->pipelineCache,
                                        args->pDataSize, args->pData);
}

static void
vkr_dispatch_vkMergePipelineCaches(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkMergePipelineCaches *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkMergePipelineCaches_args_handle(args);
   args->ret = vk->MergePipelineCaches(args->device, args->dstCache, args->srcCacheCount,
                                       args->pSrcCaches);
}

/* Flags with which the guest driver waits for the result of a pipeline
 * creation.  Otherwise Venus creates pipelines asynchronously: the guest
 * already uses the handles before the host has created anything.
 */
#define VKR_PIPELINE_CREATE_GUEST_WAITS                                              \
   (VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |                     \
    VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT)

static bool
vkr_pipeline_guest_waits(const void *pnext, VkPipelineCreateFlags flags)
{
   const VkPipelineCreateFlags2CreateInfo *flags2 =
      vkr_find_struct(pnext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
   return (flags2 ? flags2->flags : flags) & VKR_PIPELINE_CREATE_GUEST_WAITS;
}

/* Add the vkr_pipeline for one created pipeline, whatever the result of the
 * creation as a whole (per pipeline, a failure is a VK_NULL_HANDLE).
 *
 * A pipeline that failed on the host is still added, as a placeholder with a
 * VK_NULL_HANDLE driver handle, unless the guest waits for the result: an
 * asynchronously created pipeline is in use in the guest already, and a
 * lookup failure on its id would kill the whole context.  Binding the
 * placeholder is not passed to the driver: draws and dispatches are skipped
 * while it is bound (vkr_command_buffer.c), and destroying it frees just the
 * object.
 *
 * The reply, if any, reports the failure with VK_NULL_HANDLE.  A guest that
 * waits only on a secondary ring (no flag says so) drops its pipeline then,
 * and the placeholder lives until the device is destroyed.
 */
static void
vkr_pipeline_add(struct vkr_context *ctx,
                 struct vkr_device *dev,
                 struct vkr_pipeline *obj,
                 VkPipeline handle,
                 VkPipeline *args_pipeline,
                 bool guest_waits)
{
   obj->base.handle.pipeline = handle;
   if (handle == VK_NULL_HANDLE) {
      *args_pipeline = VK_NULL_HANDLE;
      if (guest_waits) {
         free(obj);
         return;
      }
      vkr_log("pipeline %" PRIu64 " creation failed on host; draws using it will be "
              "skipped",
              obj->base.id);
   }

   vkr_device_add_object(ctx, dev, &obj->base);
}

/* The generated vkr_*_pipeline_create_array drops every vkr_pipeline when the
 * creation as a whole fails, although the driver still creates the pipelines
 * it can, and vkr_pipeline_add_array drops the failed ones.  These add each
 * pipeline with vkr_pipeline_add instead.
 */
#define VKR_PIPELINE_CREATE(ctx, args, type)                                         \
   do {                                                                              \
      struct vkr_device *_dev = vkr_device_from_handle((args)->device);              \
      struct object_array _arr;                                                      \
      if (vkr_##type##_pipeline_init_array((ctx), (args), &_arr) != VK_SUCCESS)      \
         break;                                                                      \
      /* failed pipelines must be VK_NULL_HANDLE; do not trust the driver */        \
      memset(_arr.handle_storage, 0, sizeof(VkPipeline) * _arr.count);               \
      vkr_##type##_pipeline_create_driver_handles((ctx), (args), &_arr);             \
      for (uint32_t _i = 0; _i < _arr.count; _i++) {                                 \
         const bool _waits = vkr_pipeline_guest_waits((args)->pCreateInfos[_i].pNext, \
                                                      (args)->pCreateInfos[_i].flags); \
         vkr_pipeline_add((ctx), _dev, _arr.objects[_i],                             \
                          ((VkPipeline *)_arr.handle_storage)[_i],                   \
                          &(args)->pPipelines[_i], _waits);                          \
      }                                                                              \
      _arr.objects_stolen = true;                                                    \
      object_array_fini(&_arr);                                                      \
   } while (0)

static void
vkr_dispatch_vkCreateGraphicsPipelines(struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCreateGraphicsPipelines *args)
{
   TRACE_FUNC();
   VKR_PIPELINE_CREATE(dispatch->data, args, graphics);
}

static void
vkr_dispatch_vkCreateComputePipelines(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCreateComputePipelines *args)
{
   TRACE_FUNC();
   VKR_PIPELINE_CREATE(dispatch->data, args, compute);
}

static void
vkr_dispatch_vkDestroyPipeline(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyPipeline *args)
{
   if (vkr_pipeline_is_placeholder(args->pipeline)) {
      vkr_device_remove_object(dispatch->data, vkr_device_from_handle(args->device),
                               &vkr_pipeline_from_handle(args->pipeline)->base);
      return;
   }

   vkr_pipeline_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateRayTracingPipelinesKHR(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateRayTracingPipelinesKHR *args)
{
   TRACE_FUNC();
   VKR_PIPELINE_CREATE(dispatch->data, args, ray_tracing);
}

static void
vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   if (vkr_pipeline_is_placeholder(args->pipeline)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   vn_replace_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingCaptureReplayShaderGroupHandlesKHR(
      args->device, args->pipeline, args->firstGroup, args->groupCount, args->dataSize,
      args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   if (vkr_pipeline_is_placeholder(args->pipeline)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   vn_replace_vkGetRayTracingShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupHandlesKHR(args->device, args->pipeline,
                                                      args->firstGroup, args->groupCount,
                                                      args->dataSize, args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupStackSizeKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   if (vkr_pipeline_is_placeholder(args->pipeline)) {
      args->ret = 0;
      return;
   }

   vn_replace_vkGetRayTracingShaderGroupStackSizeKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupStackSizeKHR(args->device, args->pipeline,
                                                        args->group, args->groupShader);
}

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateShaderModule = vkr_dispatch_vkCreateShaderModule;
   dispatch->dispatch_vkDestroyShaderModule = vkr_dispatch_vkDestroyShaderModule;
}

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineLayout = vkr_dispatch_vkCreatePipelineLayout;
   dispatch->dispatch_vkDestroyPipelineLayout = vkr_dispatch_vkDestroyPipelineLayout;
}

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineCache = vkr_dispatch_vkCreatePipelineCache;
   dispatch->dispatch_vkDestroyPipelineCache = vkr_dispatch_vkDestroyPipelineCache;
   dispatch->dispatch_vkGetPipelineCacheData = vkr_dispatch_vkGetPipelineCacheData;
   dispatch->dispatch_vkMergePipelineCaches = vkr_dispatch_vkMergePipelineCaches;
}

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateGraphicsPipelines = vkr_dispatch_vkCreateGraphicsPipelines;
   dispatch->dispatch_vkCreateComputePipelines = vkr_dispatch_vkCreateComputePipelines;
   dispatch->dispatch_vkDestroyPipeline = vkr_dispatch_vkDestroyPipeline;

   /* VK_KHR_ray_tracing_pipeline */
   dispatch->dispatch_vkCreateRayTracingPipelinesKHR =
      vkr_dispatch_vkCreateRayTracingPipelinesKHR;
   dispatch->dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupStackSizeKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR;
}
