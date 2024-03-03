#ifndef __APPLE__

#include "gpu_info_vulkan.h"

#include <string.h>

void vulkan_init(char *vulkan_lib_path, vulkan_init_resp_t *resp) {
  VkResult ret;
  resp->err = NULL;
  const int buflen = 256;
  char buf[buflen + 1];
  int i;
  struct lookup {
    char *s;
    void **p;
  } l[] = {
      {"vkCreateInstance", (void *)&resp->rh.vulkan_init},
      {"vkDestroyInstance", (void *)&resp->rh.vulkan_shut_down},
      {"vkEnumeratePhysicalDevices", (void *)&resp->rh.vulkan_enumerate_physical_devices},
      {"vkGetPhysicalDeviceMemoryProperties2", (void *)&resp->rh.vulkan_get_physical_memory_properties},
      {"vkGetPhysicalDeviceProperties", (void *)&resp->rh.vulkan_get_physical_device_properties},
      {NULL, NULL},
  };

  resp->rh.handle = LOAD_LIBRARY(vulkan_lib_path, RTLD_LAZY);
  if (!resp->rh.handle) {
    char *msg = LOAD_ERR();
    snprintf(buf, buflen,
             "Unable to load %s library to query for Radeon GPUs: %s\n",
             vulkan_lib_path, msg);
    free(msg);
    resp->err = strdup(buf);
    return;
  }

  // TODO once we've squashed the remaining corner cases remove this log
  LOG(resp->rh.verbose, "wiring vulkan management library functions in %s\n", vulkan_lib_path);

  for (i = 0; l[i].s != NULL; i++) {
    // TODO once we've squashed the remaining corner cases remove this log
    LOG(resp->rh.verbose, "dlsym: %s\n", l[i].s);

    *l[i].p = LOAD_SYMBOL(resp->rh.handle, l[i].s);
    if (!l[i].p) {
      resp->rh.handle = NULL;
      char *msg = LOAD_ERR();
      LOG(resp->rh.verbose, "dlerr: %s\n", msg);
      UNLOAD_LIBRARY(resp->rh.handle);
      snprintf(buf, buflen, "symbol lookup for %s failed: %s", l[i].s,
               msg);
      free(msg);
      resp->err = strdup(buf);
      return;
    }
  }

  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "ollama";
  appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
  appInfo.pEngineName = "No Engine";
  appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
  appInfo.apiVersion = VK_API_VERSION_1_1;
  appInfo.pNext = NULL;

  VkInstanceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pNext = NULL;
  createInfo.flags = 0;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = 0;
  createInfo.ppEnabledExtensionNames = NULL;
  createInfo.enabledLayerCount = 0;
  createInfo.ppEnabledLayerNames = NULL;

  VkInstance instance;
  ret = (*resp->rh.vulkan_init)(&createInfo, NULL, &instance);
  if (ret != VULKAN_STATUS_SUCCESS) {
    LOG(resp->rh.verbose, "vulkan_init err: %d\n", ret);
    UNLOAD_LIBRARY(resp->rh.handle);
    resp->rh.handle = NULL;
    snprintf(buf, buflen, "vulkan vram init failure: %d", ret);
    resp->err = strdup(buf);
    return;
  }

  LOG(resp->rh.verbose, "vulkan_init success\n");
  resp->rh.vkInstance = instance;
  return;
}

void vulkan_get_version(vulkan_handle_t h, vulkan_version_resp_t *resp) {
  const int buflen = 256;
  char buf[buflen + 1];
  if (h.handle == NULL) {
    return;
  }
  VkResult ret;
  int igpuIndex = -1;
  uint32_t device_count = MAX_GPU_COUNT;
  VkPhysicalDevice devices[MAX_GPU_COUNT];
  ret = h.vulkan_enumerate_physical_devices(h.vkInstance, &device_count, devices);
  if (ret != VULKAN_STATUS_SUCCESS) {
    LOG(h.verbose, "vulkan_enumerate_physical_devices err: %d\n", ret);
    snprintf(buf, buflen, "unexpected response on vulkan_enumerate_physical_devices %d", ret);
  }

  LOG(h.verbose, "vulkan_enumerate_physical_devices success found %d devices handle %d\n", device_count, devices);
  if (device_count == 0) {
    snprintf(buf, buflen, "no devices found");
  }
  uint32_t apiVersion = -1;
  for(uint32_t i = 0; i < device_count; i++) {
    VkPhysicalDeviceProperties physProps;
    h.vulkan_get_physical_device_properties(devices[i], &physProps);
    if(physProps.apiVersion < apiVersion && physProps.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      apiVersion = physProps.apiVersion;
    }
  }

  resp->status = ret;
  char verBuf[13];
  snprintf(verBuf, 13, "%d.%d.%d.%d", VK_API_VERSION_VARIANT(apiVersion), VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));
  resp->str = verBuf;
}


void vulkan_check_vram(vulkan_handle_t h, mem_info_t *resp) {
  const int buflen = 256;
  char buf[buflen + 1];
  if (h.handle == NULL) {
    return;
  }
  VkResult ret;
  int igpuIndex = -1;
  uint32_t device_count = MAX_GPU_COUNT;
  VkPhysicalDevice devices[MAX_GPU_COUNT];
  ret = h.vulkan_enumerate_physical_devices(h.vkInstance, &device_count, devices);
  if (ret != VULKAN_STATUS_SUCCESS) {
    LOG(h.verbose, "vulkan_enumerate_physical_devices err: %d\n", ret);
    snprintf(buf, buflen, "unexpected response on vulkan_enumerate_physical_devices %d", ret);
    resp->err = strdup(buf);
  }
  
  LOG(h.verbose, "vulkan_enumerate_physical_devices success found %d devices handle %d\n", device_count, devices);
  if (device_count == 0) {
    snprintf(buf, buflen, "no devices found");
    resp->err = strdup(buf);
  }
  uint64_t totalMem = 0;
  uint64_t usedMem = 0;
  for(uint32_t i = 0; i < device_count; i++) {
    VkPhysicalDeviceMemoryProperties2 props;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps;

    budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    budgetProps.pNext = NULL;

    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    props.pNext = &budgetProps;

    h.vulkan_get_physical_memory_properties(devices[i], &props);
    LOG(h.verbose, "vulkan_get_physical_memory_properties success found %d heaps\n", props.memoryProperties.memoryHeapCount);

    for(uint32_t j = 0; j < props.memoryProperties.memoryHeapCount; j++) {
      LOG(h.verbose, "%d heapBudget %d heapUsage %d\n", j, budgetProps.heapBudget[j], budgetProps.heapUsage[j]);
      if(props.memoryProperties.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT > 0) {
        usedMem += budgetProps.heapUsage[j];
        totalMem += budgetProps.heapBudget[j];
      }
    }

    VkPhysicalDeviceProperties physProps;
    h.vulkan_get_physical_device_properties(devices[i], &physProps);
    LOG(h.verbose, "vulkan_get_physical_device_properties success\n");

    if(physProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      igpuIndex = i;
    }
  }

  resp->total = totalMem;
  resp->free = totalMem - usedMem;
  resp->count = device_count;
  resp->igpu_index = igpuIndex;
}

#endif  // __APPLE__
