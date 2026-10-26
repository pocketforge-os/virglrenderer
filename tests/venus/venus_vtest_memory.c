/*
 * Copyright 2026 PocketForge contributors
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

struct vk_context {
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkPhysicalDeviceMemoryProperties memory_props;
   PFN_vkGetMemoryFdKHR get_memory_fd;
   PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
};

static void
destroy_context(struct vk_context *ctx)
{
   if (ctx->device)
      vkDestroyDevice(ctx->device, NULL);
   if (ctx->instance)
      vkDestroyInstance(ctx->instance, NULL);
   memset(ctx, 0, sizeof(*ctx));
}

static VkResult
create_context(struct vk_context *ctx)
{
   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "venus-vtest-memory",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };
   VkResult result = vkCreateInstance(&instance_info, NULL, &ctx->instance);
   if (result != VK_SUCCESS)
      return result;

   uint32_t physical_count = 1;
   result = vkEnumeratePhysicalDevices(ctx->instance, &physical_count,
                                       &ctx->physical_device);
   if (result != VK_SUCCESS || physical_count != 1)
      goto fail;

   uint32_t queue_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device,
                                            &queue_count, NULL);
   if (!queue_count) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail;
   }

   const float priority = 0.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *extensions[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = extensions,
   };
   result = vkCreateDevice(ctx->physical_device, &device_info, NULL,
                           &ctx->device);
   if (result != VK_SUCCESS)
      goto fail;

   vkGetPhysicalDeviceMemoryProperties(ctx->physical_device,
                                       &ctx->memory_props);
   ctx->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(
      ctx->device, "vkGetMemoryFdKHR");
   ctx->get_memory_fd_properties =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
         ctx->device, "vkGetMemoryFdPropertiesKHR");
   if (!ctx->get_memory_fd || !ctx->get_memory_fd_properties) {
      result = VK_ERROR_EXTENSION_NOT_PRESENT;
      goto fail;
   }

   return VK_SUCCESS;

fail:
   destroy_context(ctx);
   return result;
}

static uint32_t
choose_memory_type(const struct vk_context *ctx)
{
   for (uint32_t i = 0; i < ctx->memory_props.memoryTypeCount; i++) {
      if (ctx->memory_props.memoryTypes[i].propertyFlags &
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         return i;
   }
   return 0;
}

static VkResult
allocate_exportable(struct vk_context *ctx,
                    VkDeviceSize size,
                    uint32_t memory_type,
                    VkDeviceMemory *memory)
{
   const VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &export_info,
      .allocationSize = size,
      .memoryTypeIndex = memory_type,
   };
   return vkAllocateMemory(ctx->device, &alloc_info, NULL, memory);
}

static VkResult
export_memory(struct vk_context *ctx, VkDeviceMemory memory, int *fd)
{
   const VkMemoryGetFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return ctx->get_memory_fd(ctx->device, &fd_info, fd);
}

static VkResult
import_memory(struct vk_context *ctx,
              int fd,
              VkDeviceSize size,
              uint32_t memory_type,
              VkDeviceMemory *memory)
{
   const VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = fd,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = size,
      .memoryTypeIndex = memory_type,
   };
   return vkAllocateMemory(ctx->device, &alloc_info, NULL, memory);
}

static int
expect_invalid(const char *name, VkResult result)
{
   if (result != VK_ERROR_INVALID_EXTERNAL_HANDLE) {
      fprintf(stderr, "FAIL %s: expected %d, got %d\n", name,
              VK_ERROR_INVALID_EXTERNAL_HANDLE, result);
      return 1;
   }
   printf("PASS %s: VK_ERROR_INVALID_EXTERNAL_HANDLE\n", name);
   return 0;
}

static int
run_case(const char *name)
{
   const VkDeviceSize size = 4096;
   struct vk_context owner = { 0 };
   VkResult result = create_context(&owner);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "FAIL create-context: %d\n", result);
      return 1;
   }

   const uint32_t memory_type = choose_memory_type(&owner);
   VkDeviceMemory memory = VK_NULL_HANDLE;
   result = allocate_exportable(&owner, size, memory_type, &memory);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "FAIL exportable-allocation: %d\n", result);
      destroy_context(&owner);
      return 1;
   }
   printf("PASS exportable-allocation\n");

   int fd = -1;
   result = export_memory(&owner, memory, &fd);
   if (result != VK_SUCCESS || fd < 0) {
      fprintf(stderr, "FAIL export-memory: result=%d fd=%d\n", result, fd);
      vkFreeMemory(owner.device, memory, NULL);
      destroy_context(&owner);
      return 1;
   }
   printf("PASS export-memory\n");

   int ret = 1;
   if (!strcmp(name, "happy")) {
      VkMemoryFdPropertiesKHR props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      };
      result = owner.get_memory_fd_properties(
         owner.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         fd, &props);
      if (result != VK_SUCCESS || !props.memoryTypeBits ||
          !(props.memoryTypeBits & (1u << memory_type))) {
         fprintf(stderr,
                 "FAIL memory-properties: result=%d bits=0x%x type=%u\n",
                 result, props.memoryTypeBits, memory_type);
         goto out;
      }
      printf("PASS memory-properties: bits=0x%x\n", props.memoryTypeBits);

      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, size, memory_type, &imported);
      if (result != VK_SUCCESS) {
         if (import_fd >= 0)
            close(import_fd);
         fprintf(stderr, "FAIL self-reimport: %d\n", result);
         goto out;
      }
      vkFreeMemory(owner.device, imported, NULL);
      printf("PASS self-reimport\n");
      ret = 0;
   } else if (!strcmp(name, "wrong-size")) {
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, size * 2,
                             memory_type, &imported);
      ret = expect_invalid(name, result);
      if (result == VK_SUCCESS)
         vkFreeMemory(owner.device, imported, NULL);
      else if (import_fd >= 0)
         close(import_fd);
   } else if (!strcmp(name, "wrong-offset")) {
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      if (import_fd < 0 || lseek(import_fd, 1, SEEK_SET) != 1) {
         fprintf(stderr, "FAIL wrong-offset setup: %s\n", strerror(errno));
         if (import_fd >= 0)
            close(import_fd);
         goto out;
      }
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, size, memory_type, &imported);
      ret = expect_invalid(name, result);
      if (result == VK_SUCCESS)
         vkFreeMemory(owner.device, imported, NULL);
      else
         close(import_fd);
   } else if (!strcmp(name, "wrong-memory-type")) {
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, size,
                             memory_type + 1, &imported);
      ret = expect_invalid(name, result);
      if (result == VK_SUCCESS)
         vkFreeMemory(owner.device, imported, NULL);
      else if (import_fd >= 0)
         close(import_fd);
   } else if (!strcmp(name, "foreign")) {
      struct vk_context foreign = { 0 };
      result = create_context(&foreign);
      if (result != VK_SUCCESS) {
         fprintf(stderr, "FAIL foreign-context setup: %d\n", result);
         goto out;
      }
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&foreign, import_fd, size,
                             choose_memory_type(&foreign), &imported);
      ret = expect_invalid(name, result);
      if (result == VK_SUCCESS)
         vkFreeMemory(foreign.device, imported, NULL);
      else if (import_fd >= 0)
         close(import_fd);
      destroy_context(&foreign);
   } else {
      fprintf(stderr, "unknown case: %s\n", name);
   }

out:
   close(fd);
   vkFreeMemory(owner.device, memory, NULL);
   destroy_context(&owner);
   return ret;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr,
              "usage: %s happy|foreign|wrong-size|wrong-offset|wrong-memory-type\n",
              argv[0]);
      return 2;
   }
   return run_case(argv[1]);
}
