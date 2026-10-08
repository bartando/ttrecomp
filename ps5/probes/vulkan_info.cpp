// SPDX-License-Identifier: GPL-3.0-or-later
// Check the static RADV device and CPU arena coexistence before game rendering.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <ps5platform/klog.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char*);
extern "C" int sceKernelDebugOutText(int channel, const char* text);
extern "C" int sceKernelUsleep(uint32_t microseconds);
int tt_platform_probe_main();
int tt_compute_probe(VkPhysicalDevice, VkDevice, uint32_t, PFN_vkGetInstanceProcAddr,
                     VkInstance, PFN_vkGetDeviceProcAddr);
int tt_display_probe(VkPhysicalDevice, VkDevice, uint32_t, PFN_vkGetInstanceProcAddr,
                     VkInstance, PFN_vkGetDeviceProcAddr);

namespace {
void report(const char* format, ...) {
  char line[512];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  std::puts(line);
  char kernel[560];
  std::snprintf(kernel, sizeof(kernel), "[tt-vulkan] %s\n", line);
  sceKernelDebugOutText(0, kernel);
}

template <typename T>
T procedure(VkInstance instance, const char* name) {
  return reinterpret_cast<T>(vk_icdGetInstanceProcAddr(instance, name));
}

int check_driver() {
  auto create_instance = procedure<PFN_vkCreateInstance>(nullptr, "vkCreateInstance");
  if (!create_instance) { report("FAIL: missing vkCreateInstance"); return 1; }
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "Table Tennis Vulkan Probe";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  const char* display_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
  instance_info.enabledExtensionCount = 2;
  instance_info.ppEnabledExtensionNames = display_extensions;
  VkInstance instance = VK_NULL_HANDLE;
  report("NEXT: create RADV Vulkan 1.1 instance");
  VkResult result = create_instance(&instance_info, nullptr, &instance);
  if (result != VK_SUCCESS) { report("FAIL: vkCreateInstance = %d", int(result)); return 1; }

  auto destroy_instance = procedure<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
  auto enumerate = procedure<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  auto properties = procedure<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
  auto features = procedure<PFN_vkGetPhysicalDeviceFeatures>(instance, "vkGetPhysicalDeviceFeatures");
  auto queues = procedure<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  auto memory = procedure<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
  auto extensions = procedure<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
  auto create_device = procedure<PFN_vkCreateDevice>(instance, "vkCreateDevice");
  auto device_proc = procedure<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
  if (!destroy_instance || !enumerate || !properties || !features || !queues ||
      !memory || !extensions || !create_device || !device_proc) {
    report("FAIL: missing required instance entry point");
    if (destroy_instance) destroy_instance(instance, nullptr);
    return 1;
  }
  int failures = 0;
  uint32_t count = 1;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  result = enumerate(instance, &count, &physical);
  if (result != VK_SUCCESS || count != 1) {
    report("FAIL: expected one physical device; result=%d count=%u", int(result), count);
    destroy_instance(instance, nullptr); return 1;
  }
  VkPhysicalDeviceProperties props{};
  properties(physical, &props);
  report("Device: %s; vendor=0x%x device=0x%x Vulkan=%u.%u.%u", props.deviceName,
         props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
  VkPhysicalDeviceFeatures supported{};
  features(physical, &supported);
  const struct { const char* name; VkBool32 value; } required[] = {
      {"independentBlend", supported.independentBlend},
      {"fragmentStoresAndAtomics", supported.fragmentStoresAndAtomics},
      {"vertexPipelineStoresAndAtomics", supported.vertexPipelineStoresAndAtomics},
      {"geometryShader", supported.geometryShader},
      {"fillModeNonSolid", supported.fillModeNonSolid},
  };
  for (const auto& feature : required) {
    report("%s: %s", feature.value ? "PASS" : "FAIL", feature.name);
    if (!feature.value) ++failures;
  }
  VkPhysicalDeviceMemoryProperties mem{};
  memory(physical, &mem);
  for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
    report("Memory heap %u: %llu MiB; flags=0x%x", i,
           static_cast<unsigned long long>(mem.memoryHeaps[i].size >> 20), mem.memoryHeaps[i].flags);
  uint32_t queue_count = 32;
  VkQueueFamilyProperties families[32]{};
  queues(physical, &queue_count, families);
  uint32_t graphics_queue = UINT32_MAX;
  for (uint32_t i = 0; i < queue_count; ++i) {
    constexpr auto required_queue = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (families[i].queueCount && (families[i].queueFlags & required_queue) == required_queue) {
      graphics_queue = i; break;
    }
  }
  if (graphics_queue == UINT32_MAX) { report("FAIL: no graphics/compute queue"); ++failures; }
  uint32_t extension_count = 256;
  VkExtensionProperties advertised[256]{};
  result = extensions(physical, nullptr, &extension_count, advertised);
  const char* requested_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  for (const char* name : requested_extensions) {
    bool found = false;
    if (result == VK_SUCCESS)
      for (uint32_t i = 0; i < extension_count; ++i) found |= std::strcmp(advertised[i].extensionName, name) == 0;
    report("%s: %s", found ? "PASS" : "FAIL", name);
    if (!found) ++failures;
  }
  if (!failures) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_queue;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.independentBlend = enabled.fragmentStoresAndAtomics = enabled.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabled.geometryShader = enabled.fillModeNonSolid = VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = requested_extensions;
    device_info.pEnabledFeatures = &enabled;
    VkDevice device = VK_NULL_HANDLE;
    report("NEXT: create device with renderer-required features");
    result = create_device(physical, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) { report("FAIL: vkCreateDevice = %d", int(result)); ++failures; }
    else {
      auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(device_proc(device, "vkDestroyDevice"));
      if (!destroy_device) {
        report("FAIL: missing vkDestroyDevice; retaining device and instance until system close");
        return 1;
      }
      else {
        report("PASS: Vulkan logical device created");
        report("NEXT: CPU arena/alias probe while Vulkan device is alive");
        if (tt_platform_probe_main() != 0) ++failures;
        if (!failures) {
          report("NEXT: GPU compute execution and readback");
          const int gpu = tt_compute_probe(physical, device, graphics_queue,
              vk_icdGetInstanceProcAddr, instance, device_proc);
          if (gpu < 0) return 1;
          failures += gpu;
        }
        if (!failures) {
          report("NEXT: TV display and edge-pixel readback");
          // The live swapchain, device and instance belong to the shell until
          // system close; destroying them here would immediately remove output.
          return tt_display_probe(physical, device, graphics_queue,
              vk_icdGetInstanceProcAddr, instance, device_proc);
        }
        destroy_device(device, nullptr);
      }
    }
  }
  destroy_instance(instance, nullptr);
  return failures;
}
}  // namespace

// Same proven title lifetime rule as the CPU probe: let the shell close it.
extern "C" [[noreturn]] void catchReturnFromMain(int) {
  sceKernelDebugOutText(0, "[tt-vulkan] waiting for system close\n");
  for (;;) sceKernelUsleep(1000000);
}

int main() {
  if (!std::freopen("/app0/tt-vulkan-info.txt", "w", stdout)) {
    sceKernelDebugOutText(0, "[tt-vulkan] FAIL: cannot open results file\n");
    return 1;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  ps5_klog_capture_stderr("tt-vulkan");
  report("TT PS5 Vulkan info: started (device, CPU aliases, bounded GPU compute/readback, TV display)");
  const int failures = check_driver();
  report("TT PS5 Vulkan info: finished with %d failures", failures);
  return failures ? 1 : 0;
}
