/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_COMMAND_BUFFER_H
#define VKR_COMMAND_BUFFER_H

#include "vkr_common.h"

#include "vkr_context.h"

struct vkr_command_pool {
   struct vkr_object base;

   struct list_head command_buffers;
};
VKR_DEFINE_OBJECT_CAST(command_pool, VK_OBJECT_TYPE_COMMAND_POOL, VkCommandPool)

/* Pipeline bind points tracked for placeholder pipelines. */
enum vkr_bind_point {
   VKR_BIND_POINT_GRAPHICS,
   VKR_BIND_POINT_COMPUTE,
   VKR_BIND_POINT_RAY_TRACING,
   VKR_BIND_POINT_COUNT,
};

struct vkr_command_buffer {
   struct vkr_object base;

   struct vkr_device *device;

   /* A placeholder pipeline (vkr_pipeline_is_placeholder) is bound at the bind
    * point: its draws/dispatches/ray traces are not recorded.  Cleared by
    * vkBeginCommandBuffer and by binding a real pipeline.
    */
   bool placeholder_bound[VKR_BIND_POINT_COUNT];
};
VKR_DEFINE_OBJECT_CAST(command_buffer, VK_OBJECT_TYPE_COMMAND_BUFFER, VkCommandBuffer)

void
vkr_context_init_command_pool_dispatch(struct vkr_context *ctx);

void
vkr_context_init_command_buffer_dispatch(struct vkr_context *ctx);

static inline void
vkr_command_pool_release(struct vkr_context *ctx, struct vkr_command_pool *pool)
{
   vkr_context_remove_objects(ctx, &pool->command_buffers);
}

#endif /* VKR_COMMAND_BUFFER_H */
