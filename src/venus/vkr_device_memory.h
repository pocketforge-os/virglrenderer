/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;
struct vkr_resource;

static inline bool
vkr_device_memory_can_use_external_export(bool dma_buf_supported,
                                          bool opaque_supported,
                                          bool no_dma_buf_export,
                                          bool has_gbm_device)
{
   return dma_buf_supported ||
          (opaque_supported && (no_dma_buf_export || !has_gbm_device));
}

static inline bool
vkr_device_memory_should_align_opaque_allocation(bool has_export_info,
                                                 bool no_dma_buf_export)
{
   return !has_export_info || !no_dma_buf_export;
}

static inline bool
vkr_device_memory_should_track_implicit_export(
   VkExternalMemoryHandleTypeFlagBits handle_type)
{
   return handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
}

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

   uint64_t guest_visible_allocation_size;
   uint64_t allocation_size;
   uint32_t memory_type_index;

   bool exported;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

VkResult
vkr_device_memory_validate_opaque_resource(const struct vkr_resource *res,
                                           uint32_t context_id,
                                           vkr_object_id device_id,
                                           uint64_t allocation_size,
                                           uint32_t memory_type_index,
                                           uint64_t *out_driver_allocation_size);

#endif /* VKR_DEVICE_MEMORY_H */
