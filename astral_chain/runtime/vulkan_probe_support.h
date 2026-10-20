/* Shared system Vulkan loading and host-coherent buffer helpers for probes. */
#pragma once
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INSTANCE_FUNCTIONS(X) X(vkGetPhysicalDeviceFormatProperties) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdPipelineBarrier) X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences)
#define DECLARE(name) static PFN_##name name;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
typedef struct {VkBuffer buffer;VkDeviceMemory memory;void *mapped;} Buffer;
static VkResult buffer_create(VkDevice device,const VkPhysicalDeviceMemoryProperties *memory,Buffer *out,
 VkDeviceSize size,VkBufferUsageFlags usage,const void *data) {
 VkBufferCreateInfo info={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;info.usage=usage;
 VkResult result=vkCreateBuffer(device,&info,NULL,&out->buffer);if(result)return result;
 VkMemoryRequirements need;vkGetBufferMemoryRequirements(device,out->buffer,&need);
 unsigned type=memory->memoryTypeCount;
 for(unsigned i=0;i<memory->memoryTypeCount;i++)if((need.memoryTypeBits&(1u<<i)) &&
  (memory->memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
  (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {type=i;break;}
 if(type==memory->memoryTypeCount)return VK_ERROR_FEATURE_NOT_PRESENT;
 VkMemoryAllocateInfo alloc={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,need.size,type};
 result=vkAllocateMemory(device,&alloc,NULL,&out->memory);if(result)return result;
 result=vkBindBufferMemory(device,out->buffer,out->memory,0);if(result)return result;
 result=vkMapMemory(device,out->memory,0,size,0,&out->mapped);if(result)return result;
 if(data)memcpy(out->mapped,data,(size_t)size);else memset(out->mapped,0xcd,(size_t)size);
 return VK_SUCCESS;
}
static int read_exact(const char *path,void *bytes,size_t size) {
 FILE *f=fopen(path,"rb");if(!f)return 0;
 size_t got=fread(bytes,1,size,f);int extra=fgetc(f),bad=ferror(f);fclose(f);
 return got==size && extra==EOF && !bad;
}
#define CHECK(call) do {VkResult r_=(call);if(r_!=VK_SUCCESS){fprintf(stderr,"%s failed: %d\n",#call,r_);goto cleanup;}}while(0)
