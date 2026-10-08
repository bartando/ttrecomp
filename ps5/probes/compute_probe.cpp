// SPDX-License-Identifier: GPL-3.0-or-later
// Execute a bounded integer shader and verify all of its output on the CPU.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include "compute_shader.h"

// -1 means work may still be in flight: retain the device until system close.
int tt_compute_probe(VkPhysicalDevice physical, VkDevice device, uint32_t family,
                     PFN_vkGetInstanceProcAddr instance_proc, VkInstance instance,
                     PFN_vkGetDeviceProcAddr device_proc) {
#define LOAD(name) \
  const auto name = reinterpret_cast<PFN_##name>(device_proc(device, #name)); \
  if (!name) { std::printf("FAIL: missing %s\n", #name); return 1; }
  LOAD(vkCreateBuffer)
  LOAD(vkDestroyBuffer)
  LOAD(vkGetBufferMemoryRequirements)
  LOAD(vkAllocateMemory)
  LOAD(vkFreeMemory)
  LOAD(vkBindBufferMemory)
  LOAD(vkMapMemory)
  LOAD(vkUnmapMemory)
  LOAD(vkCreateDescriptorSetLayout)
  LOAD(vkDestroyDescriptorSetLayout)
  LOAD(vkCreateDescriptorPool)
  LOAD(vkDestroyDescriptorPool)
  LOAD(vkAllocateDescriptorSets)
  LOAD(vkUpdateDescriptorSets)
  LOAD(vkCreatePipelineLayout)
  LOAD(vkDestroyPipelineLayout)
  LOAD(vkCreateShaderModule)
  LOAD(vkDestroyShaderModule)
  LOAD(vkCreateComputePipelines)
  LOAD(vkDestroyPipeline)
  LOAD(vkCreateCommandPool)
  LOAD(vkDestroyCommandPool)
  LOAD(vkAllocateCommandBuffers)
  LOAD(vkBeginCommandBuffer)
  LOAD(vkEndCommandBuffer)
  LOAD(vkCmdBindPipeline)
  LOAD(vkCmdBindDescriptorSets)
  LOAD(vkCmdDispatch)
  LOAD(vkCmdPipelineBarrier)
  LOAD(vkCreateFence)
  LOAD(vkDestroyFence)
  LOAD(vkGetDeviceQueue)
  LOAD(vkQueueSubmit)
  LOAD(vkWaitForFences)
#undef LOAD
  const auto get_memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!get_memory) { std::puts("FAIL: missing memory properties function"); return 1; }

  constexpr uint32_t words = 256;
  constexpr VkDeviceSize bytes = words * sizeof(uint32_t);
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory allocation = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  void* mapped = nullptr;
  auto cleanup = [&] {
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (mapped) vkUnmapMemory(device, allocation);
    if (buffer) vkDestroyBuffer(device, buffer, nullptr);
    if (allocation) vkFreeMemory(device, allocation, nullptr);
  };
  auto failed = [&](const char* operation, VkResult result) {
    std::printf("FAIL: %s = %d\n", operation, int(result));
    cleanup();
    return 1;
  };
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
  if (result != VK_SUCCESS) return failed("vkCreateBuffer", result);
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  VkPhysicalDeviceMemoryProperties memory{};
  get_memory(physical, &memory);
  uint32_t memory_type = UINT32_MAX;
  constexpr auto host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & host_flags) == host_flags) {
      memory_type = i; break;
    }
  }
  if (memory_type == UINT32_MAX) {
    std::puts("FAIL: no coherent host-visible storage-buffer memory"); cleanup(); return 1;
  }
  VkMemoryAllocateInfo allocation_info{};
  allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation_info.allocationSize = requirements.size;
  allocation_info.memoryTypeIndex = memory_type;
  result = vkAllocateMemory(device, &allocation_info, nullptr, &allocation);
  if (result != VK_SUCCESS) return failed("vkAllocateMemory", result);
  result = vkBindBufferMemory(device, buffer, allocation, 0);
  if (result != VK_SUCCESS) return failed("vkBindBufferMemory", result);
  result = vkMapMemory(device, allocation, 0, bytes, 0, &mapped);
  if (result != VK_SUCCESS) return failed("vkMapMemory", result);
  auto* output = static_cast<volatile uint32_t*>(mapped);
  for (uint32_t i = 0; i < words; ++i) output[i] = UINT32_MAX;

  VkDescriptorSetLayoutBinding binding{};
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  result = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
  if (result != VK_SUCCESS) return failed("vkCreateDescriptorSetLayout", result);
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  result = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);
  if (result != VK_SUCCESS) return failed("vkCreateDescriptorPool", result);
  VkDescriptorSetAllocateInfo set_allocate{};
  set_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_allocate.descriptorPool = descriptor_pool;
  set_allocate.descriptorSetCount = 1;
  set_allocate.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  result = vkAllocateDescriptorSets(device, &set_allocate, &set);
  if (result != VK_SUCCESS) return failed("vkAllocateDescriptorSets", result);
  const VkDescriptorBufferInfo buffer_binding{buffer, 0, bytes};
  VkWriteDescriptorSet update{};
  update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  update.dstSet = set;
  update.descriptorCount = 1;
  update.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  update.pBufferInfo = &buffer_binding;
  vkUpdateDescriptorSets(device, 1, &update, 0, nullptr);
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  result = vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
  if (result != VK_SUCCESS) return failed("vkCreatePipelineLayout", result);
  VkShaderModuleCreateInfo shader_info{};
  shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_info.codeSize = sizeof(tt_compute_shader);
  shader_info.pCode = tt_compute_shader;
  result = vkCreateShaderModule(device, &shader_info, nullptr, &shader);
  if (result != VK_SUCCESS) return failed("vkCreateShaderModule", result);
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.layout = pipeline_layout;
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = shader;
  pipeline_info.stage.pName = "main";
  std::puts("NEXT: compile bounded integer compute pipeline");
  result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
  if (result != VK_SUCCESS) return failed("vkCreateComputePipelines", result);
  VkCommandPoolCreateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  command_info.queueFamilyIndex = family;
  result = vkCreateCommandPool(device, &command_info, nullptr, &command_pool);
  if (result != VK_SUCCESS) return failed("vkCreateCommandPool", result);
  VkCommandBufferAllocateInfo command_allocate{};
  command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_allocate.commandPool = command_pool;
  command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocate.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  result = vkAllocateCommandBuffers(device, &command_allocate, &command);
  if (result != VK_SUCCESS) return failed("vkAllocateCommandBuffers", result);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(command, &begin);
  if (result != VK_SUCCESS) return failed("vkBeginCommandBuffer", result);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
  vkCmdDispatch(command, words / 64, 1, 1);
  VkBufferMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.size = bytes;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 0, nullptr, 1, &barrier, 0, nullptr);
  result = vkEndCommandBuffer(command);
  if (result != VK_SUCCESS) return failed("vkEndCommandBuffer", result);
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  result = vkCreateFence(device, &fence_info, nullptr, &fence);
  if (result != VK_SUCCESS) return failed("vkCreateFence", result);
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, family, 0, &queue);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  std::puts("NEXT: submit four compute workgroups and wait up to 5 seconds");
  result = vkQueueSubmit(queue, 1, &submit, fence);
  if (result != VK_SUCCESS) return failed("vkQueueSubmit", result);
  result = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull);
  if (result != VK_SUCCESS) {
    std::printf("FAIL: vkWaitForFences = %d; retaining GPU resources until system close\n", int(result));
    return -1;
  }
  for (uint32_t i = 0; i < words; ++i) {
    const uint32_t expected = 0x54540000u + i * 3u;
    const uint32_t actual = output[i];
    if (actual != expected) {
      std::printf("FAIL: GPU output[%u] = 0x%x; expected 0x%x\n", i, actual, expected);
      cleanup(); return 1;
    }
  }
  std::puts("PASS: GPU shader execution and all 256 output words read back correctly");
  cleanup();
  return 0;
}
