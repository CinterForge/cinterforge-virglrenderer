/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PIPELINE_H
#define VKR_PIPELINE_H

#include "vkr_common.h"

struct vkr_shader_module {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(shader_module, VK_OBJECT_TYPE_SHADER_MODULE, VkShaderModule)

struct vkr_pipeline_layout {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(pipeline_layout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, VkPipelineLayout)

struct vkr_pipeline_cache {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(pipeline_cache, VK_OBJECT_TYPE_PIPELINE_CACHE, VkPipelineCache)

struct vkr_pipeline {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(pipeline, VK_OBJECT_TYPE_PIPELINE, VkPipeline)

/* A pipeline the driver failed to create (see vkr_pipeline_add): the object
 * exists for the guest, its driver handle is VK_NULL_HANDLE.
 */
static inline bool
vkr_pipeline_is_placeholder(VkPipeline pipeline)
{
   const struct vkr_pipeline *obj = vkr_pipeline_from_handle(pipeline);
   return obj && obj->base.handle.pipeline == VK_NULL_HANDLE;
}

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx);

#endif /* VKR_PIPELINE_H */
