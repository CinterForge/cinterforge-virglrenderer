/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_query_pool.h"

#include "vkr_query_pool_gen.h"

/* Venus creates query pools asynchronously: when the driver refuses one, the guest still
 * holds its id and the first command buffer resetting, beginning or copying it is a fatal
 * CS error ("failed to look up object N of type 12") that kills the application's GPU
 * context. Name the refused pool so that report says why.
 */
static void
vkr_dispatch_vkCreateQueryPool(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateQueryPool *args)
{
   if (vkr_query_pool_create_and_add(dispatch->data, args) || !args->pCreateInfo ||
       !args->pQueryPool)
      return;

   const VkQueryPoolCreateInfo *info = args->pCreateInfo;
   vkr_log("vkCreateQueryPool failed on the host (VkResult %d): query pool %" PRIu64
           " (type %d, %u queries, statistics 0x%x, flags 0x%x%s) is unknown, commands using "
           "it are CS errors",
           args->ret,
           vkr_cs_handle_load_id((const void **)args->pQueryPool, VK_OBJECT_TYPE_QUERY_POOL),
           info->queryType, info->queryCount, info->pipelineStatistics, info->flags,
           info->pNext ? ", with pNext" : "");
}

static void
vkr_dispatch_vkDestroyQueryPool(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyQueryPool *args)
{
   vkr_query_pool_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetQueryPoolResults(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkGetQueryPoolResults *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetQueryPoolResults_args_handle(args);
   args->ret = vk->GetQueryPoolResults(args->device, args->queryPool, args->firstQuery,
                                       args->queryCount, args->dataSize, args->pData,
                                       args->stride, args->flags);
}

static void
vkr_dispatch_vkResetQueryPool(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkResetQueryPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetQueryPool_args_handle(args);
   vk->ResetQueryPool(args->device, args->queryPool, args->firstQuery, args->queryCount);
}

void
vkr_context_init_query_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateQueryPool = vkr_dispatch_vkCreateQueryPool;
   dispatch->dispatch_vkDestroyQueryPool = vkr_dispatch_vkDestroyQueryPool;
   dispatch->dispatch_vkGetQueryPoolResults = vkr_dispatch_vkGetQueryPoolResults;
   dispatch->dispatch_vkResetQueryPool = vkr_dispatch_vkResetQueryPool;
}
