/*
 * Copyright 2026 PocketForge contributors
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
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
   VkQueue queue;
   uint32_t queue_family_index;
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
   ctx->queue_family_index = queue_info.queueFamilyIndex;
   vkGetDeviceQueue(ctx->device, ctx->queue_family_index, 0, &ctx->queue);

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

static bool
find_memory_type(const struct vk_context *ctx,
                 uint32_t type_bits,
                 VkMemoryPropertyFlags properties,
                 uint32_t *memory_type)
{
   for (uint32_t i = 0; i < ctx->memory_props.memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (ctx->memory_props.memoryTypes[i].propertyFlags & properties) ==
             properties) {
         *memory_type = i;
         return true;
      }
   }
   return false;
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
allocate_implicit(struct vk_context *ctx,
                  VkDeviceSize size,
                  uint32_t memory_type,
                  VkDeviceMemory *memory)
{
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
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
expect_invalid(const char *name, VkResult result, bool async_alloc)
{
   if (async_alloc) {
      if (result != VK_SUCCESS) {
         fprintf(stderr, "FAIL %s async submission: expected %d, got %d\n",
                 name, VK_SUCCESS, result);
         return 1;
      }
      printf("PASS %s: async allocation queued for host validation\n", name);
      fflush(stdout);
      return 0;
   }

   if (result != VK_ERROR_INVALID_EXTERNAL_HANDLE) {
      fprintf(stderr, "FAIL %s: expected %d, got %d\n", name,
              VK_ERROR_INVALID_EXTERNAL_HANDLE, result);
      return 1;
   }
   printf("PASS %s: VK_ERROR_INVALID_EXTERNAL_HANDLE\n", name);
   return 0;
}

/*
 * An asynchronous vkAllocateMemory can return before the renderer rejects the
 * import.  Make the next host-visible operation use the rejected memory, then
 * require the context to fail before a copy can complete.
 */
static int
use_rejected_memory(struct vk_context *ctx,
                    const char *name,
                    VkDeviceMemory imported)
{
   const VkDeviceSize size = 64;
   const uint8_t sentinel = 0xa5;
   const VkExternalMemoryBufferCreateInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkBufferCreateInfo source_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &external_info,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   const VkBufferCreateInfo readback_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer source = VK_NULL_HANDLE;
   VkBuffer readback = VK_NULL_HANDLE;
   VkDeviceMemory readback_memory = VK_NULL_HANDLE;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkResult result = vkCreateBuffer(ctx->device, &source_info, NULL, &source);
   if (result != VK_SUCCESS)
      goto setup_fail;
   result = vkCreateBuffer(ctx->device, &readback_info, NULL, &readback);
   if (result != VK_SUCCESS)
      goto setup_fail;

   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(ctx->device, readback, &requirements);
   uint32_t readback_type;
   if (!find_memory_type(ctx, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &readback_type)) {
      fprintf(stderr, "FAIL %s async-use setup: no coherent readback type\n",
              name);
      goto fail;
   }
   const VkMemoryAllocateInfo allocation_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = readback_type,
   };
   result = vkAllocateMemory(ctx->device, &allocation_info, NULL,
                             &readback_memory);
   if (result != VK_SUCCESS)
      goto setup_fail;
   result = vkBindBufferMemory(ctx->device, readback, readback_memory, 0);
   if (result != VK_SUCCESS)
      goto setup_fail;

   void *mapping = NULL;
   result = vkMapMemory(ctx->device, readback_memory, 0, size, 0, &mapping);
   if (result != VK_SUCCESS)
      goto setup_fail;
   memset(mapping, sentinel, size);
   vkUnmapMemory(ctx->device, readback_memory);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family_index,
   };
   result = vkCreateCommandPool(ctx->device, &pool_info, NULL, &command_pool);
   if (result != VK_SUCCESS)
      goto setup_fail;
   const VkCommandBufferAllocateInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   result = vkAllocateCommandBuffers(ctx->device, &command_info,
                                     &command_buffer);
   if (result != VK_SUCCESS)
      goto setup_fail;

   printf("PASS %s: attempting async use of rejected import\n", name);
   fflush(stdout);
   result = vkBindBufferMemory(ctx->device, source, imported, 0);
   if (result != VK_SUCCESS)
      goto use_failed;

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result != VK_SUCCESS)
      goto use_failed;
   const VkBufferCopy copy = { .size = size };
   vkCmdCopyBuffer(command_buffer, source, readback, 1, &copy);
   result = vkEndCommandBuffer(command_buffer);
   if (result != VK_SUCCESS)
      goto use_failed;
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   result = vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
   if (result != VK_SUCCESS)
      goto use_failed;
   result = vkQueueWaitIdle(ctx->queue);
   if (result != VK_SUCCESS)
      goto use_failed;

   result = vkMapMemory(ctx->device, readback_memory, 0, size, 0, &mapping);
   bool unchanged = result == VK_SUCCESS;
   if (result == VK_SUCCESS) {
      const uint8_t *bytes = mapping;
      for (VkDeviceSize i = 0; i < size; i++)
         unchanged &= bytes[i] == sentinel;
      vkUnmapMemory(ctx->device, readback_memory);
   }
   fprintf(stderr,
           "FAIL %s async-use reached a successful wait; "
           "sentinel_unchanged=%d\n",
           name, unchanged);
   return 1;

use_failed:
   if (result == VK_ERROR_DEVICE_LOST) {
      printf("PASS %s: async use returned VK_ERROR_DEVICE_LOST\n", name);
      fflush(stdout);
      return 0;
   }
   fprintf(stderr, "FAIL %s async-use: unexpected result %d\n", name,
           result);
   return 1;

setup_fail:
   fprintf(stderr, "FAIL %s async-use setup: %d\n", name, result);
fail:
   if (command_pool)
      vkDestroyCommandPool(ctx->device, command_pool, NULL);
   if (readback_memory)
      vkFreeMemory(ctx->device, readback_memory, NULL);
   if (readback)
      vkDestroyBuffer(ctx->device, readback, NULL);
   if (source)
      vkDestroyBuffer(ctx->device, source, NULL);
   return 1;
}

static int
run_case(const char *name)
{
   const char *alloc_mode = getenv("VN_ALLOC_MODE");
   const bool async_alloc = alloc_mode && !strcmp(alloc_mode, "async");
   const bool implicit = !strcmp(name, "implicit-unaligned");
   const bool unaligned = implicit || !strcmp(name, "happy-unaligned") ||
                          !strcmp(name, "wrong-size-unaligned");
   const bool success_case = !strcmp(name, "happy") ||
                             !strcmp(name, "happy-unaligned") || implicit;
   const VkDeviceSize size = unaligned ? 4096 + 256 : 4096;
   struct vk_context owner = { 0 };
   VkResult result = create_context(&owner);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "FAIL create-context: %d\n", result);
      return 1;
   }

   const uint32_t memory_type = choose_memory_type(&owner);
   VkDeviceMemory memory = VK_NULL_HANDLE;
   result = implicit ? allocate_implicit(&owner, size, memory_type, &memory)
                     : allocate_exportable(&owner, size, memory_type, &memory);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "FAIL %sallocation: %d\n",
              implicit ? "implicit-" : "exportable-", result);
      destroy_context(&owner);
      return 1;
   }
   printf("PASS %sallocation: size=%" PRIu64 "\n",
          implicit ? "implicit-" : "exportable-", (uint64_t)size);

   if (implicit) {
      void *mapping = NULL;
      result = vkMapMemory(owner.device, memory, 0, size, 0, &mapping);
      if (result != VK_SUCCESS) {
         fprintf(stderr, "FAIL implicit-map: %d\n", result);
         vkFreeMemory(owner.device, memory, NULL);
         destroy_context(&owner);
         return 1;
      }
      vkUnmapMemory(owner.device, memory);
      printf("PASS implicit-map\n");
   }

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
   if (success_case) {
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
   } else if (!strcmp(name, "wrong-size") ||
              !strcmp(name, "wrong-size-unaligned")) {
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size <= 0) {
         fprintf(stderr, "FAIL get-page-size\n");
         goto out;
      }
      const VkDeviceSize import_size =
         !strcmp(name, "wrong-size-unaligned")
            ? ((size + (VkDeviceSize)page_size - 1) /
               (VkDeviceSize)page_size) * (VkDeviceSize)page_size
            : size * 2;
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, import_size,
                             memory_type, &imported);
      ret = expect_invalid(name, result, async_alloc);
      if (result == VK_SUCCESS) {
         if (async_alloc)
            return use_rejected_memory(&owner, name, imported);
         vkFreeMemory(owner.device, imported, NULL);
      }
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
      ret = expect_invalid(name, result, async_alloc);
      if (result == VK_SUCCESS) {
         if (async_alloc)
            return use_rejected_memory(&owner, name, imported);
         vkFreeMemory(owner.device, imported, NULL);
      }
      else
         close(import_fd);
   } else if (!strcmp(name, "wrong-memory-type")) {
      int import_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
      VkDeviceMemory imported = VK_NULL_HANDLE;
      result = import_memory(&owner, import_fd, size,
                             memory_type + 1, &imported);
      ret = expect_invalid(name, result, async_alloc);
      if (result == VK_SUCCESS) {
         if (async_alloc)
            return use_rejected_memory(&owner, name, imported);
         vkFreeMemory(owner.device, imported, NULL);
      }
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
      ret = expect_invalid(name, result, async_alloc);
      if (result == VK_SUCCESS) {
         if (async_alloc)
            return use_rejected_memory(&foreign, name, imported);
         vkFreeMemory(foreign.device, imported, NULL);
      }
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
              "usage: %s happy|happy-unaligned|implicit-unaligned|foreign|"
              "wrong-size|wrong-size-unaligned|wrong-offset|wrong-memory-type\n",
              argv[0]);
      return 2;
   }
   return run_case(argv[1]);
}
