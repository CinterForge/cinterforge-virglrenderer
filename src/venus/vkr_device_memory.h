/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;
/* POSIX shm imported as host memory, backing memory the guest can map */
struct vkr_shm {
   int fd;
   void *ptr;
   size_t size;

   /* in vkr_device::shm_release.pending or vkr_shm_release::shms */
   struct list_head head;
};

/* shm whose VkDeviceMemory has been freed, unmapped once the GPU work
 * submitted before the free has completed on every queue of the device
 */
struct vkr_shm_release {
   struct vkr_device *device;
   /* queues whose release marker has not completed yet, under
    * vkr_device::shm_release.mutex
    */
   uint32_t pending_queues;
   struct list_head shms;
};

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;

   bool might_export;

   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   /* udmabuf backing non-external mappable memory */
   int udmabuf_fd;

   /* shm imported as host memory when the host cannot export an fd */
   struct vkr_shm *shm;

   uint64_t allocation_size;
   uint32_t memory_type_index;

   bool exported;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

void
vkr_device_flush_shm_releases(struct vkr_device *dev, bool force);

void
vkr_shm_release_complete_queue(struct vkr_shm_release *release);

void
vkr_device_fini_shm_releases(struct vkr_device *dev);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

#endif /* VKR_DEVICE_MEMORY_H */
