// SPDX-License-Identifier: GPL-3.0-or-later
// Present a blue frame through VK_KHR_display and verify two edge pixels.
// Keep its resources alive until system close so the frame stays visible.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>

extern "C" int sceSystemServiceHideSplashScreen(void);

int tt_display_probe(VkPhysicalDevice physical, VkDevice device, uint32_t family,
                     PFN_vkGetInstanceProcAddr instance_proc, VkInstance instance,
                     PFN_vkGetDeviceProcAddr device_proc) {
#define LOAD_I(name) \
  const auto name = reinterpret_cast<PFN_##name>(instance_proc(instance, #name)); \
  if (!name) { std::printf("FAIL: missing %s\n", #name); return 1; }
#define LOAD_D(name) \
  const auto name = reinterpret_cast<PFN_##name>(device_proc(device, #name)); \
  if (!name) { std::printf("FAIL: missing %s\n", #name); return 1; }
  LOAD_I(vkGetPhysicalDeviceDisplayPropertiesKHR)
  LOAD_I(vkGetDisplayModePropertiesKHR)
  LOAD_I(vkCreateDisplayPlaneSurfaceKHR)
  LOAD_I(vkGetPhysicalDeviceSurfaceSupportKHR)
  LOAD_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
  LOAD_I(vkGetPhysicalDeviceSurfaceFormatsKHR)
  LOAD_I(vkGetPhysicalDeviceMemoryProperties)
  LOAD_D(vkCreateSwapchainKHR)
  LOAD_D(vkGetSwapchainImagesKHR)
  LOAD_D(vkAcquireNextImageKHR)
  LOAD_D(vkQueuePresentKHR)
  LOAD_D(vkCreateSemaphore)
  LOAD_D(vkCreateFence)
  LOAD_D(vkCreateCommandPool)
  LOAD_D(vkAllocateCommandBuffers)
  LOAD_D(vkBeginCommandBuffer)
  LOAD_D(vkEndCommandBuffer)
  LOAD_D(vkCmdPipelineBarrier)
  LOAD_D(vkCmdClearColorImage)
  LOAD_D(vkCmdCopyImageToBuffer)
  LOAD_D(vkCreateBuffer)
  LOAD_D(vkGetBufferMemoryRequirements)
  LOAD_D(vkAllocateMemory)
  LOAD_D(vkBindBufferMemory)
  LOAD_D(vkMapMemory)
  LOAD_D(vkGetDeviceQueue)
  LOAD_D(vkQueueSubmit)
  LOAD_D(vkWaitForFences)
#undef LOAD_I
#undef LOAD_D
  auto failed = [](const char* name, VkResult result) {
    std::printf("FAIL: display %s = %d; resources retained until system close\n", name, int(result));
    return 1;
  };
  uint32_t count = 1;
  VkDisplayPropertiesKHR display{};
  VkResult result = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display);
  if (result != VK_SUCCESS || count != 1) return failed("display enumeration", result);
  VkDisplayModePropertiesKHR modes[32]{};
  count = 32;
  result = vkGetDisplayModePropertiesKHR(physical, display.display, &count, modes);
  if (result != VK_SUCCESS || !count) return failed("mode enumeration", result);
  uint32_t selected = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& p = modes[i].parameters;
    if (p.visibleRegion.width == 1920 && p.visibleRegion.height == 1080 && p.refreshRate <= 60000) {
      selected = i; break;
    }
  }
  const VkExtent2D extent = modes[selected].parameters.visibleRegion;
  std::printf("Display: %s; %ux%u at %.3f Hz\n", display.displayName,
              extent.width, extent.height, modes[selected].parameters.refreshRate / 1000.0);
  VkDisplaySurfaceCreateInfoKHR surface_info{};
  surface_info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
  surface_info.displayMode = modes[selected].displayMode;
  surface_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  surface_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
  surface_info.globalAlpha = 1.0f;
  surface_info.imageExtent = extent;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  result = vkCreateDisplayPlaneSurfaceKHR(instance, &surface_info, nullptr, &surface);
  if (result != VK_SUCCESS) return failed("surface creation", result);
  VkBool32 supported = VK_FALSE;
  result = vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported);
  if (result != VK_SUCCESS || !supported) return failed("queue presentation support", result);
  VkSurfaceCapabilitiesKHR caps{};
  result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
  if (result != VK_SUCCESS) return failed("surface capabilities", result);
  constexpr auto transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((caps.supportedUsageFlags & transfer) != transfer ||
      !(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ||
      !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
    std::puts("FAIL: display lacks transfer, identity-transform or opaque-alpha support"); return 1;
  }
  VkSurfaceFormatKHR formats[32]{};
  count = 32;
  result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats);
  if (result != VK_SUCCESS) return failed("surface formats", result);
  bool blue_format = false;
  for (uint32_t i = 0; i < count; ++i)
    blue_format |= formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
                   formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  if (!blue_format) { std::puts("FAIL: display lacks BGRA8 UNORM / nonlinear sRGB format"); return 1; }
  uint32_t image_count = caps.minImageCount > 3 ? caps.minImageCount : 3;
  if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
  VkSwapchainCreateInfoKHR swapchain_info{};
  swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchain_info.surface = surface;
  swapchain_info.minImageCount = image_count;
  swapchain_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  swapchain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  swapchain_info.imageExtent = extent;
  swapchain_info.imageArrayLayers = 1;
  swapchain_info.imageUsage = transfer;
  swapchain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapchain_info.clipped = VK_TRUE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::puts("NEXT: create display swapchain");
  result = vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain);
  if (result != VK_SUCCESS) return failed("swapchain creation", result);
  VkImage images[8]{};
  count = 8;
  result = vkGetSwapchainImagesKHR(device, swapchain, &count, images);
  if (result != VK_SUCCESS || !count) return failed("swapchain images", result);

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = 8;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer readback = VK_NULL_HANDLE;
  result = vkCreateBuffer(device, &buffer_info, nullptr, &readback);
  if (result != VK_SUCCESS) return failed("readback buffer creation", result);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, readback, &requirements);
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical, &memory);
  uint32_t memory_type = UINT32_MAX;
  constexpr auto host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & host_flags) == host_flags) {
      memory_type = i; break;
    }
  }
  if (memory_type == UINT32_MAX) { std::puts("FAIL: no coherent display-readback memory"); return 1; }
  VkMemoryAllocateInfo allocation_info{};
  allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation_info.allocationSize = requirements.size;
  allocation_info.memoryTypeIndex = memory_type;
  VkDeviceMemory allocation = VK_NULL_HANDLE;
  result = vkAllocateMemory(device, &allocation_info, nullptr, &allocation);
  if (result != VK_SUCCESS) return failed("readback allocation", result);
  result = vkBindBufferMemory(device, readback, allocation, 0);
  if (result != VK_SUCCESS) return failed("readback binding", result);
  void* mapped = nullptr;
  result = vkMapMemory(device, allocation, 0, 8, 0, &mapped);
  if (result != VK_SUCCESS) return failed("readback mapping", result);
  auto* pixels = static_cast<volatile uint32_t*>(mapped);
  pixels[0] = 0;
  pixels[1] = 0;
  VkSemaphoreCreateInfo semaphore_info{};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
  result = vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired);
  if (result != VK_SUCCESS) return failed("acquire semaphore", result);
  result = vkCreateSemaphore(device, &semaphore_info, nullptr, &rendered);
  if (result != VK_SUCCESS) return failed("render semaphore", result);
  uint32_t index = 0;
  result = vkAcquireNextImageKHR(device, swapchain, 5000000000ull, acquired, VK_NULL_HANDLE, &index);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return failed("acquire image", result);
  if (index >= count) { std::puts("FAIL: acquired image index outside swapchain"); return 1; }
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = family;
  VkCommandPool pool = VK_NULL_HANDLE;
  result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
  if (result != VK_SUCCESS) return failed("command pool", result);
  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.commandPool = pool;
  command_info.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  result = vkAllocateCommandBuffers(device, &command_info, &command);
  if (result != VK_SUCCESS) return failed("command buffer", result);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(command, &begin);
  if (result != VK_SUCCESS) return failed("begin commands", result);
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = images[index];
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkClearColorValue blue{};
  blue.float32[2] = blue.float32[3] = 1.0f;
  vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &blue, 1, &barrier.subresourceRange);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copies[2]{};
  for (auto& copy : copies) {
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {1, 1, 1};
  }
  copies[1].bufferOffset = 4;
  copies[1].imageOffset = {int32_t(extent.width - 1), int32_t(extent.height - 1), 0};
  vkCmdCopyImageToBuffer(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 2, copies);
  VkBufferMemoryBarrier host_barrier{};
  host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.buffer = readback;
  host_barrier.size = 8;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 0, nullptr, 1, &host_barrier, 0, nullptr);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  result = vkEndCommandBuffer(command);
  if (result != VK_SUCCESS) return failed("end commands", result);
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  result = vkCreateFence(device, &fence_info, nullptr, &fence);
  if (result != VK_SUCCESS) return failed("completion fence", result);
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, family, 0, &queue);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &acquired;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &rendered;
  std::puts("NEXT: clear display image blue, copy edge pixels, submit");
  result = vkQueueSubmit(queue, 1, &submit, fence);
  if (result != VK_SUCCESS) return failed("queue submit", result);
  result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
  if (result != VK_SUCCESS) return failed("completion wait", result);
  const uint32_t first = pixels[0], last = pixels[1];
  if (first != 0xff0000ffu || last != 0xff0000ffu) {
    std::printf("FAIL: blue edge-pixel readback: first=0x%x last=0x%x\n", first, last); return 1;
  }
  std::puts("PASS: first and last display pixels read back as blue");
  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &rendered;
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain;
  present.pImageIndices = &index;
  result = vkQueuePresentKHR(queue, &present);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return failed("present", result);
  const int hidden = sceSystemServiceHideSplashScreen();
  if (hidden != 0) {
    std::printf("FAIL: sceSystemServiceHideSplashScreen = 0x%x\n", unsigned(hidden)); return 1;
  }
  std::puts("PASS: blue frame presented and launch splash hidden; awaiting visual confirmation");
  return 0;
}
