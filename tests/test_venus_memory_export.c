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
#include "virglrenderer.h"

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
      .guest_visible_allocation_size = 4096,
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

static int
test_preserve_gbm_dma_buf_path(void)
{
   if (vkr_device_memory_can_use_external_export(false, true, false, true)) {
      fprintf(stderr, "opaque fallback replaced the GBM dma-buf path\n");
      return 1;
   }

   if (!vkr_device_memory_can_use_external_export(false, true, false, false)) {
      fprintf(stderr, "opaque fallback unavailable without a GBM device\n");
      return 1;
   }

   if (!vkr_device_memory_should_align_opaque_allocation(false, true) ||
       !vkr_device_memory_should_align_opaque_allocation(true, false) ||
       vkr_device_memory_should_align_opaque_allocation(true, true)) {
      fprintf(stderr, "opaque allocation alignment scope changed\n");
      return 1;
   }

   puts("PASS preserve-gbm-dma-buf-path");
   return 0;
}

static int
test_track_implicit_opaque_only(void)
{
   if (vkr_device_memory_should_track_implicit_export(
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
       !vkr_device_memory_should_track_implicit_export(
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
      fprintf(stderr, "implicit export tracking scope mismatch\n");
      return 1;
   }

   puts("PASS track-implicit-opaque-only");
   return 0;
}

static int
test_get_info_fd_visibility(bool opaque)
{
   int pipe_fds[2];
   if (pipe(pipe_fds)) {
      perror("create pipe");
      return 2;
   }

   struct virgl_renderer_callbacks callbacks = {
      .version = 1,
   };
   uint32_t cookie = 0;
   if (virgl_renderer_init(&cookie, VIRGL_RENDERER_NO_VIRGL, &callbacks)) {
      fprintf(stderr, "failed to initialize renderer\n");
      close(pipe_fds[0]);
      close(pipe_fds[1]);
      return 2;
   }

   const uint32_t fd_type = opaque ? VIRGL_RENDERER_BLOB_FD_TYPE_OPAQUE
                                   : VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
   const struct virgl_renderer_resource_import_blob_args import_args = {
      .res_handle = 1,
      .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
      .fd_type = fd_type,
      .fd = pipe_fds[0],
      .size = 4096,
   };
   if (virgl_renderer_resource_import_blob(&import_args)) {
      fprintf(stderr, "failed to create fd-backed resource\n");
      close(pipe_fds[0]);
      close(pipe_fds[1]);
      virgl_renderer_cleanup(&cookie);
      return 2;
   }

   struct virgl_renderer_resource_info info = { 0 };
   struct virgl_renderer_resource_info_ext info_ext = {
      .version = VIRGL_RENDERER_RESOURCE_INFO_EXT_VERSION,
   };
   const int expected_fd = opaque ? -1 : pipe_fds[0];
   const int info_ret = virgl_renderer_resource_get_info(1, &info);
   const int info_ext_ret = virgl_renderer_resource_get_info_ext(1, &info_ext);
   const bool passed = !info_ret && !info_ext_ret && info.fd == expected_fd &&
                       info_ext.base.fd == expected_fd;

   close(pipe_fds[1]);
   virgl_renderer_resource_unref(1);
   virgl_renderer_cleanup(&cookie);

   if (!passed) {
      fprintf(stderr,
              "get_info fd visibility mismatch: type=%d expected=%d info=%d "
              "info_ext=%d info_ret=%d info_ext_ret=%d\n",
              fd_type, expected_fd, info.fd, info_ext.base.fd, info_ret,
              info_ext_ret);
      return 1;
   }

   printf("PASS %s-get-info-fd\n", opaque ? "hide-opaque" : "preserve-dma-buf");
   return 0;
}

static int
test_opaque_import_validation(void)
{
   struct vkr_resource resource = {
      .fd_type = VIRGL_RESOURCE_FD_OPAQUE,
      .u.fd = backing_fd,
      .opaque_export = {
         .valid = true,
         .context_id = 1,
         .device_id = 2,
         .guest_visible_allocation_size = 4352,
         .driver_allocation_size = 8192,
         .memory_type_index = 3,
         .fd_offset = 0,
      },
   };

   if (lseek(backing_fd, 0, SEEK_SET) != 0)
      return 2;

   uint64_t driver_allocation_size = 0;
   if (vkr_device_memory_validate_opaque_resource(
          &resource, 1, 2, 4352, 3, &driver_allocation_size) != VK_SUCCESS ||
       driver_allocation_size != 8192 ||
       vkr_device_memory_validate_opaque_resource(
          &resource, 9, 2, 4352, 3, NULL) !=
          VK_ERROR_INVALID_EXTERNAL_HANDLE ||
       vkr_device_memory_validate_opaque_resource(
          &resource, 1, 9, 4352, 3, NULL) !=
          VK_ERROR_INVALID_EXTERNAL_HANDLE ||
       vkr_device_memory_validate_opaque_resource(
          &resource, 1, 2, 8192, 3, NULL) !=
          VK_ERROR_INVALID_EXTERNAL_HANDLE ||
       vkr_device_memory_validate_opaque_resource(
          &resource, 1, 2, 4352, 4, NULL) !=
          VK_ERROR_INVALID_EXTERNAL_HANDLE) {
      fprintf(stderr, "opaque resource metadata validation mismatch\n");
      return 1;
   }

   if (lseek(backing_fd, 1, SEEK_SET) != 1 ||
       vkr_device_memory_validate_opaque_resource(
          &resource, 1, 2, 4352, 3, NULL) !=
          VK_ERROR_INVALID_EXTERNAL_HANDLE) {
      fprintf(stderr, "opaque resource offset validation mismatch\n");
      return 1;
   }

   puts("PASS opaque-import-validation");
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr,
              "usage: %s never-replace-dma-buf|preserve-gbm-dma-buf-path|"
              "track-implicit-opaque-only|"
              "hide-opaque-get-info|preserve-dma-buf-get-info|"
              "opaque-import-validation\n",
              argv[0]);
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
   else if (!strcmp(argv[1], "preserve-gbm-dma-buf-path"))
      ret = test_preserve_gbm_dma_buf_path();
   else if (!strcmp(argv[1], "track-implicit-opaque-only"))
      ret = test_track_implicit_opaque_only();
   else if (!strcmp(argv[1], "hide-opaque-get-info"))
      ret = test_get_info_fd_visibility(true);
   else if (!strcmp(argv[1], "preserve-dma-buf-get-info"))
      ret = test_get_info_fd_visibility(false);
   else if (!strcmp(argv[1], "opaque-import-validation"))
      ret = test_opaque_import_validation();
   else
      ret = 2;

   fclose(backing);
   return ret;
}
