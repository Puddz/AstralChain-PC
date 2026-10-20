/* Native Vulkan prerequisite probe. Module creation is not a draw or pipeline. */
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static int probe_device(VkInstance instance,PFN_vkGetInstanceProcAddr get,VkPhysicalDevice physical,
 const uint32_t *code,size_t size) {
#define INSTANCE(name) PFN_##name name=(PFN_##name)get(instance,#name);if(!name)return 1
 INSTANCE(vkGetPhysicalDeviceProperties2);
 INSTANCE(vkGetPhysicalDeviceFeatures);
 INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
 INSTANCE(vkCreateDevice);
 INSTANCE(vkGetDeviceProcAddr);
 VkPhysicalDeviceSubgroupProperties subgroup={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
 VkPhysicalDeviceProperties2 properties={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&subgroup};
 vkGetPhysicalDeviceProperties2(physical,&properties);
 VkPhysicalDeviceFeatures features={0};vkGetPhysicalDeviceFeatures(physical,&features);
 printf("device=%s api=%u.%u.%u vendor=0x%x device=0x%x type=%u\n",properties.properties.deviceName,
  VK_API_VERSION_MAJOR(properties.properties.apiVersion),VK_API_VERSION_MINOR(properties.properties.apiVersion),
  VK_API_VERSION_PATCH(properties.properties.apiVersion),properties.properties.vendorID,properties.properties.deviceID,properties.properties.deviceType);
 printf("shaderInt64=%u vertexStores=%u fragmentStores=%u subgroupSize=%u subgroupStages=0x%x subgroupOperations=0x%x\n",
  features.shaderInt64,features.vertexPipelineStoresAndAtomics,features.fragmentStoresAndAtomics,
  subgroup.subgroupSize,subgroup.supportedStages,subgroup.supportedOperations);
 uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,NULL);
 if(!count || count>64) {fprintf(stderr,"unsupported queue family count\n");return 1;}
 VkQueueFamilyProperties queues[64];uint32_t capacity=count;
 vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues);
 if(count!=capacity)return 1;
 uint32_t family=UINT32_MAX;
 for(uint32_t i=0;i<count;i++)if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {family=i;break;}
 if(family==UINT32_MAX || properties.properties.apiVersion<VK_API_VERSION_1_2) {
  fprintf(stderr,"device lacks required graphics queue or Vulkan 1.2\n");return 1;
 }
 float priority=1;
 VkDeviceQueueCreateInfo queue={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,family,1,&priority};
 VkPhysicalDeviceFeatures enabled={0};enabled.shaderInt64=features.shaderInt64;
 VkDeviceCreateInfo create={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,NULL,0,1,&queue,0,NULL,0,NULL,&enabled};
 VkDevice device=VK_NULL_HANDLE;VkResult result=vkCreateDevice(physical,&create,NULL,&device);
 if(result!=VK_SUCCESS) {fprintf(stderr,"vkCreateDevice=%d\n",result);return 1;}
 PFN_vkDestroyDevice destroy=(PFN_vkDestroyDevice)vkGetDeviceProcAddr(device,"vkDestroyDevice");
 PFN_vkCreateShaderModule make=(PFN_vkCreateShaderModule)vkGetDeviceProcAddr(device,"vkCreateShaderModule");
 PFN_vkDestroyShaderModule release=(PFN_vkDestroyShaderModule)vkGetDeviceProcAddr(device,"vkDestroyShaderModule");
 if(!destroy || !make || !release) {if(destroy)destroy(device,NULL);return 1;}
 VkShaderModuleCreateInfo shader={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,size,code};
 VkShaderModule module=VK_NULL_HANDLE;result=make(device,&shader,NULL,&module);
 printf("graphicsQueue=%u shaderModuleResult=%d bytes=%zu\n",family,result,size);
 if(result==VK_SUCCESS)release(device,module,NULL);
 destroy(device,NULL);return result==VK_SUCCESS?0:1;
}

int main(int argc,char **argv) {
 if(argc!=2) {fprintf(stderr,"usage: vulkan_probe validated-fragment.spv\n");return 2;}
 enum { LIMIT=1024*1024 };
 uint32_t *code=malloc(LIMIT);if(!code)return 2;
 FILE *input=fopen(argv[1],"rb");if(!input) {free(code);return 2;}
 size_t size=fread(code,1,LIMIT,input);int extra=fgetc(input),bad=ferror(input);fclose(input);
 if(bad || extra!=EOF || size<20 || size%4 || code[0]!=0x07230203 || code[4]!=0) {
  fprintf(stderr,"invalid or oversized SPIR-V input\n");free(code);return 2;
 }
 HMODULE library=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!library) {fprintf(stderr,"system Vulkan loader unavailable\n");free(code);return 1;}
 PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(library,"vkGetInstanceProcAddr");
 if(!get) {FreeLibrary(library);free(code);return 1;}
 PFN_vkCreateInstance make=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
 PFN_vkEnumerateInstanceVersion version=(PFN_vkEnumerateInstanceVersion)get(NULL,"vkEnumerateInstanceVersion");
 uint32_t api=VK_API_VERSION_1_0;
 if(!make || !version || version(&api)!=VK_SUCCESS || api<VK_API_VERSION_1_2) {
  fprintf(stderr,"Vulkan 1.2 loader required\n");FreeLibrary(library);free(code);return 1;
 }
 VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"Astral Chain native research",1,NULL,0,VK_API_VERSION_1_2};
 VkInstanceCreateInfo create={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,NULL,0,&app,0,NULL,0,NULL};
 VkInstance instance=VK_NULL_HANDLE;VkResult result=make(&create,NULL,&instance);int status=1;
 if(result==VK_SUCCESS) {
  PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
  PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
  if(destroy && enumerate) {
   VkPhysicalDevice devices[16];uint32_t count=16;
   result=enumerate(instance,&count,devices);
   if(result==VK_SUCCESS && count) {
    printf("physicalDevices=%u\n",count);
    for(uint32_t i=0;i<count;i++)if(!probe_device(instance,get,devices[i],code,size))status=0;
   } else fprintf(stderr,"physical device enumeration failed or exceeds 16: %d\n",result);
  }
  if(destroy)destroy(instance,NULL);
 } else fprintf(stderr,"vkCreateInstance=%d\n",result);
 FreeLibrary(library);free(code);return status;
}
