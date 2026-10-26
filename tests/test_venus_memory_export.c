/*
 * Copyright 2026 PocketForge contributors
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "venus/vkr_device.h"
#include "venus/vkr_device_memory.h"
#include "venus/vkr_physical_device.h"

static int backing_fd = -1;
static VkExternalMemoryHandleTypeFlagBits exported_handle_type;

static VkResult
test_get_memory_fd(VkDevice device,
                   const VkMemoryGetFdInfoKHR *info,
                   int *fd)
{
   exported_handle_type = info->handleType;
   *fd = fcntl(backing_fd, F_DUPFD_CLOEXEC, 0);
   return *fd < 0 ? VK_ERROR_TOO_MANY_OBJECTS : VK_SUCCESS;
}

static int
run_export(bool dma_buf, bool opaque, bool cross_device, uint64_t blob_size,
           struct virgl_context_blob *blob)
{
   struct vkr_physical_device physical_device = { 0 };
   struct vkr_device device = {
      .physical_device = &physical_device,
   };
   struct vkr_device_memory memory = {
      .device = &device,
      .property_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      .valid_fd_types =
         (dma_buf ? 1u << VIRGL_RESOURCE_FD_DMABUF : 0) |
         (opaque ? 1u << VIRGL_RESOURCE_FD_OPAQUE : 0),
      .udmabuf_fd = -1,
      .allocation_size = 4096,
      .memory_type_index = 2,
   };
   device.proc_table.GetMemoryFdKHR = test_get_memory_fd;
   device.base.handle.device = (VkDevice)(uintptr_t)1;
   memory.base.handle.device_memory = (VkDeviceMemory)(uintptr_t)2;

   const uint32_t flags =
      cross_device ? VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE : 0;
   return vkr_device_memory_export_blob(&memory, blob_size, flags, blob);
}

static int
test_never_replace_dma_buf(void)
{
   struct virgl_context_blob blob = { 0 };
   if (!run_export(true, true, true, 4096, &blob)) {
      fprintf(stderr, "DMA_BUF export unexpectedly failed\n");
      return 1;
   }

   const bool passed =
      blob.type == VIRGL_RESOURCE_FD_DMABUF &&
      exported_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   close(blob.u.fd);
   if (!passed) {
      fprintf(stderr, "fallback replaced a working DMA_BUF export\n");
      return 1;
   }

   puts("PASS never-replace-dma-buf");
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s never-replace-dma-buf\n", argv[0]);
      return 2;
   }

   FILE *backing = tmpfile();
   if (!backing || ftruncate(fileno(backing), 4096)) {
      perror("create backing file");
      return 2;
   }
   backing_fd = fileno(backing);

   int ret;
   if (!strcmp(argv[1], "never-replace-dma-buf"))
      ret = test_never_replace_dma_buf();
   else
      ret = 2;

   fclose(backing);
   return ret;
}
