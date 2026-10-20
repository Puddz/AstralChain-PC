/* Offline first-draw experiment: captured buffers, decoded depth and shaders.
 * Uses diagnostic full-size viewport/no blend/depth; not game state replay. */
#include "vulkan_probe_support.h"
#include "vulkan_draw_api.h"
enum { WIDTH=1600, HEIGHT=900, PIXELS=WIDTH*HEIGHT, OUTPUT_BYTES=PIXELS*16 };
typedef struct {VkImage image;VkDeviceMemory memory;VkImageView view;} Image;
static VkResult image_create(VkDevice device,const VkPhysicalDeviceMemoryProperties *memory,
 Image *out,VkFormat format,VkImageUsageFlags usage) {
 VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;
 ci.format=format;ci.extent=(VkExtent3D){WIDTH,HEIGHT,1};ci.mipLevels=ci.arrayLayers=1;
 ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;
 VkResult r=vkCreateImage(device,&ci,NULL,&out->image);if(r)return r;
 VkMemoryRequirements need;vkGetImageMemoryRequirements(device,out->image,&need);
 unsigned type=memory->memoryTypeCount;
 for(unsigned i=0;i<memory->memoryTypeCount;i++)if(need.memoryTypeBits&(1u<<i)){type=i;break;}
 if(type==memory->memoryTypeCount)return VK_ERROR_FEATURE_NOT_PRESENT;
 VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,need.size,type};
 r=vkAllocateMemory(device,&ai,NULL,&out->memory);if(r)return r;
 r=vkBindImageMemory(device,out->image,out->memory,0);if(r)return r;
 VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=out->image;
 vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
 vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 return vkCreateImageView(device,&vi,NULL,&out->view);
}
static void image_barrier(VkCommandBuffer command,VkImage image,VkImageLayout before,VkImageLayout after,
 VkAccessFlags source,VkAccessFlags dest,VkPipelineStageFlags source_stage,VkPipelineStageFlags dest_stage) {
 VkImageMemoryBarrier b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=source;b.dstAccessMask=dest;
 b.oldLayout=before;b.newLayout=after;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 b.image=image;b.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 vkCmdPipelineBarrier(command,source_stage,dest_stage,0,0,NULL,0,NULL,1,&b);
}
static size_t read_module(const char *path,uint32_t *code) {
 FILE *f=fopen(path,"rb");if(!f)return 0;
 size_t n=fread(code,1,1024*1024,f);int extra=fgetc(f),bad=ferror(f);fclose(f);
 return extra==EOF && !bad && n>=20 && n%4==0 && code[0]==0x07230203 && !code[4] ? n : 0;
}
static volatile LONG draw_busy;
__declspec(dllexport) int ac_vulkan_draw(const AcVulkanDraw *request) {
 if(!request || request->version!=1 || request->struct_size!=sizeof(*request) || request->reserved || request->mode>1)return 2;
 const size_t input_sizes[]={0,0,48,32,256,256,8,256,PIXELS*4};
 size_t output_bytes=request->mode ? PIXELS*4 : OUTPUT_BYTES;
 if(!request->output || request->output_size!=output_bytes)return 2;
 uintptr_t request_at=(uintptr_t)request,output_at=(uintptr_t)request->output;
 if((request_at>=output_at && request_at-output_at<output_bytes) ||
    (output_at>=request_at && output_at-request_at<sizeof(*request)))return 2;
 for(unsigned i=0;i<9;i++) {
  const AcDrawInput *in=&request->inputs[i];if(!in->data)return 2;
  if(i>=2) {if(in->size!=input_sizes[i])return 2;}
  else {
   if(in->size<20 || in->size>1024*1024 || in->size%4 || ((uintptr_t)in->data&3))return 2;
   const uint32_t *words=in->data;if(words[0]!=0x07230203 || words[4])return 2;
  }
  uintptr_t src=(uintptr_t)in->data,dst=(uintptr_t)request->output;
  if((src>=dst && src-dst<output_bytes) || (dst>=src && dst-src<in->size))return 2;
 }
 const uint32_t *code[]={request->inputs[0].data,request->inputs[1].data};
 size_t code_size[]={request->inputs[0].size,request->inputs[1].size};
 const void *v0=request->inputs[2].data,*v1=request->inputs[3].data,*c3=request->inputs[4].data,*c4=request->inputs[5].data,*fc4=request->inputs[7].data;
 const unsigned char *indices=request->inputs[6].data;
 unsigned seen=0;for(unsigned i=0;i<4;i++){unsigned n=indices[i*2]|indices[i*2+1]<<8;if(n>3 || (seen&(1u<<n)))return 2;seen|=1u<<n;}
 if(InterlockedCompareExchange(&draw_busy,1,0))return 1;
 uint32_t *depth=malloc(PIXELS*4);if(!depth){InterlockedExchange(&draw_busy,0);return 1;}
 memcpy(depth,request->inputs[8].data,PIXELS*4);
 VkFormat target_format=request->mode ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
 /* Sampled depth view: stencil discarded; R32 carries the depth component.
  * Exact for this captured all-one texture. General UNORM precision unproven. */
 for(unsigned i=0;i<PIXELS;i++){float d=(float)((double)(depth[i]>>8)/16777215.0);memcpy(&depth[i],&d,4);}
 int status=1;HMODULE library=NULL;VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
 VkShaderModule shaders[2]={0};VkDescriptorSetLayout set_layouts[2]={0};
 VkPipelineLayout layout=VK_NULL_HANDLE;VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
 VkRenderPass render_pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
 VkCommandPool command_pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
 Buffer buffers[9]={0};Image images[2]={0};
 library=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)goto cleanup;
 PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(library,"vkGetInstanceProcAddr");if(!get)goto cleanup;
 PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");if(!create)goto cleanup;
 VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"Astral Chain fragment research",1,NULL,0,VK_API_VERSION_1_2};
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;CHECK(create(&ici,NULL,&instance));
#define LOAD_INSTANCE(name) name=(PFN_##name)get(instance,#name);if(!name)goto cleanup;
 INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
 VkPhysicalDevice physicals[16];uint32_t count=16;CHECK(vkEnumeratePhysicalDevices(instance,&count,physicals));
 VkPhysicalDevice physical=VK_NULL_HANDLE;uint32_t family=0;VkPhysicalDeviceFeatures enabled={0};
 for(unsigned pass=0;pass<2 && !physical;pass++)for(unsigned i=0;i<count;i++) {
  VkPhysicalDeviceProperties p;VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceProperties(physicals[i],&p);vkGetPhysicalDeviceFeatures(physicals[i],&features);
  if(p.apiVersion<VK_API_VERSION_1_2 || (!pass && p.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))continue;
  uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&n,NULL);if(!n || n>64)continue;
  VkQueueFamilyProperties q[64];vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&n,q);
  for(unsigned j=0;j<n;j++)if(q[j].queueCount && (q[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
   physical=physicals[i];family=j;enabled.shaderInt64=features.shaderInt64;
   printf("device=%s graphicsQueue=%u\n",p.deviceName,family);break;
  }
  if(physical)break;
 }
 if(!physical){fprintf(stderr,"no suitable graphics device\n");goto cleanup;}
 float priority=1;VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,family,1,&priority};
 VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.pEnabledFeatures=&enabled;
 CHECK(vkCreateDevice(physical,&dci,NULL,&device));
#define LOAD_DEVICE(name) name=(PFN_##name)vkGetDeviceProcAddr(device,#name);if(!name)goto cleanup;
 DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
 VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);

 VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32_SFLOAT,&fp);
 if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))!=
    (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))goto cleanup;
 vkGetPhysicalDeviceFormatProperties(physical,target_format,&fp);
 if((fp.optimalTilingFeatures&(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))!=
    (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))goto cleanup;
 CHECK(buffer_create(device,&memory,&buffers[0],48,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,v0));
 CHECK(buffer_create(device,&memory,&buffers[1],32,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,v1));
 CHECK(buffer_create(device,&memory,&buffers[2],256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,c3));
 CHECK(buffer_create(device,&memory,&buffers[3],256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,c4));
 CHECK(buffer_create(device,&memory,&buffers[4],8,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices));
 CHECK(buffer_create(device,&memory,&buffers[5],256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,fc4));
 float tint[8]={0};CHECK(buffer_create(device,&memory,&buffers[6],32,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,tint));
 CHECK(buffer_create(device,&memory,&buffers[7],PIXELS*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,depth));
 CHECK(buffer_create(device,&memory,&buffers[8],output_bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,NULL));
 CHECK(image_create(device,&memory,&images[0],VK_FORMAT_R32_SFLOAT,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT));
 CHECK(image_create(device,&memory,&images[1],target_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
 VkSamplerCreateInfo sampler_info={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
 sampler_info.magFilter=sampler_info.minFilter=VK_FILTER_NEAREST;sampler_info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
 sampler_info.addressModeU=sampler_info.addressModeV=sampler_info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
 sampler_info.maxLod=15;CHECK(vkCreateSampler(device,&sampler_info,NULL,&sampler));
 for(unsigned i=0;i<2;i++) {VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,code_size[i],code[i]};CHECK(vkCreateShaderModule(device,&ci,NULL,&shaders[i]));}
 VkDescriptorSetLayoutBinding vertex_bindings[]={{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL},{4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL}};
 VkDescriptorSetLayoutBinding fragment_bindings[]={{4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},{19,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},{32,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
 VkDescriptorSetLayoutCreateInfo slci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,2,vertex_bindings};CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layouts[0]));
 slci.bindingCount=3;slci.pBindings=fragment_bindings;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layouts[1]));
 VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,2,set_layouts};CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
 VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
 VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,2,2,sizes};CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&descriptor_pool));
 VkDescriptorSetAllocateInfo dsai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,NULL,descriptor_pool,2,set_layouts};VkDescriptorSet sets[2];CHECK(vkAllocateDescriptorSets(device,&dsai,sets));
 VkDescriptorBufferInfo infos[]={{buffers[2].buffer,0,256},{buffers[3].buffer,0,256},{buffers[5].buffer,0,256},{buffers[6].buffer,0,32}};
 unsigned binding_numbers[]={3,4,4,19};VkWriteDescriptorSet writes[5]={0};
 for(unsigned i=0;i<4;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets[i/2];writes[i].dstBinding=binding_numbers[i];writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[i].pBufferInfo=&infos[i];}
 VkDescriptorImageInfo image_info={sampler,images[0].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
 writes[4].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[4].dstSet=sets[1];writes[4].dstBinding=32;writes[4].descriptorCount=1;writes[4].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[4].pImageInfo=&image_info;
 vkUpdateDescriptorSets(device,5,writes,0,NULL);
 VkAttachmentDescription attachment={0};attachment.format=target_format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
 VkAttachmentReference color_ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
 VkSubpassDescription subpass={0};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color_ref;
 VkRenderPassCreateInfo rpci={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rpci.attachmentCount=1;rpci.pAttachments=&attachment;rpci.subpassCount=1;rpci.pSubpasses=&subpass;CHECK(vkCreateRenderPass(device,&rpci,NULL,&render_pass));
 VkFramebufferCreateInfo fbci={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbci.renderPass=render_pass;fbci.attachmentCount=1;fbci.pAttachments=&images[1].view;fbci.width=WIDTH;fbci.height=HEIGHT;fbci.layers=1;CHECK(vkCreateFramebuffer(device,&fbci,NULL,&framebuffer));
 VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,shaders[0],"main",NULL},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,shaders[1],"main",NULL}};
 VkVertexInputBindingDescription vb[]={{0,12,VK_VERTEX_INPUT_RATE_VERTEX},{1,8,VK_VERTEX_INPUT_RATE_VERTEX}};
 VkVertexInputAttributeDescription va[]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{5,1,VK_FORMAT_R32G32_SFLOAT,0}};
 VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,NULL,0,2,vb,2,va};
 VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,NULL,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,VK_FALSE};
 VkViewport viewport={0,request->mode ? HEIGHT : 0,WIDTH,request->mode ? -HEIGHT : HEIGHT,0,1};VkRect2D scissor={{0,0},{WIDTH,HEIGHT}};
 VkPipelineViewportStateCreateInfo viewport_state={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,NULL,0,1,&viewport,1,&scissor};
 VkPipelineRasterizationStateCreateInfo raster={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.lineWidth=1;if(request->mode){raster.cullMode=VK_CULL_MODE_BACK_BIT;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;}
 VkPipelineMultisampleStateCreateInfo multisample={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState blend_attachment={0};blend_attachment.colorWriteMask=15;
 VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&blend_attachment;
 VkGraphicsPipelineCreateInfo pci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pci.stageCount=2;pci.pStages=stages;pci.pVertexInputState=&vi;pci.pInputAssemblyState=&ia;pci.pRasterizationState=&raster;pci.pViewportState=&viewport_state;pci.pMultisampleState=&multisample;pci.pColorBlendState=&blend;pci.layout=layout;pci.renderPass=render_pass;pci.basePipelineIndex=-1;
 CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
 VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,0,family};CHECK(vkCreateCommandPool(device,&cpci,NULL,&command_pool));
 VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,command_pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
 VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
 image_barrier(command,images[0].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 VkBufferImageCopy copy={0};copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=(VkExtent3D){WIDTH,HEIGHT,1};
 vkCmdCopyBufferToImage(command,buffers[7].buffer,images[0].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
 image_barrier(command,images[0].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
 image_barrier(command,images[1].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
 VkClearValue clear={.color={{-999,-999,-999,-999}}};
 VkRenderPassBeginInfo rpbi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rpbi.renderPass=render_pass;rpbi.framebuffer=framebuffer;rpbi.renderArea=scissor;rpbi.clearValueCount=1;rpbi.pClearValues=&clear;
 vkCmdBeginRenderPass(command,&rpbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
 vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,2,sets,0,NULL);
 VkBuffer vertex_buffers[]={buffers[0].buffer,buffers[1].buffer};VkDeviceSize offsets[]={0,0};vkCmdBindVertexBuffers(command,0,2,vertex_buffers,offsets);
 vkCmdBindIndexBuffer(command,buffers[4].buffer,0,VK_INDEX_TYPE_UINT16);vkCmdDrawIndexed(command,4,1,0,0,0);vkCmdEndRenderPass(command);
 image_barrier(command,images[1].image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 vkCmdCopyImageToBuffer(command,images[1].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[8].buffer,1,&copy);
 VkMemoryBarrier barrier={VK_STRUCTURE_TYPE_MEMORY_BARRIER,NULL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
 VkFenceCreateInfo fci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(vkCreateFence(device,&fci,NULL,&fence));
 VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;CHECK(vkQueueSubmit(queue,1,&submit,fence));
 VkResult waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
 if(waited!=VK_SUCCESS){fprintf(stderr,"fragment completion unavailable: %d\n",waited);free(depth);return 3;}
 memcpy(request->output,buffers[8].mapped,output_bytes);
 printf("indexedVertices=4 width=%u height=%u outputBytes=%zu completed=1 rasterization=enabled mode=%u\n",WIDTH,HEIGHT,output_bytes,request->mode);status=0;
cleanup:
 if(device && vkDestroyDevice) {
  if(fence)vkDestroyFence(device,fence,NULL);if(command_pool)vkDestroyCommandPool(device,command_pool,NULL);
  if(pipeline)vkDestroyPipeline(device,pipeline,NULL);if(framebuffer)vkDestroyFramebuffer(device,framebuffer,NULL);
  if(render_pass)vkDestroyRenderPass(device,render_pass,NULL);if(descriptor_pool)vkDestroyDescriptorPool(device,descriptor_pool,NULL);
  if(layout)vkDestroyPipelineLayout(device,layout,NULL);if(sampler)vkDestroySampler(device,sampler,NULL);
  for(unsigned i=0;i<2;i++) {
   if(set_layouts[i])vkDestroyDescriptorSetLayout(device,set_layouts[i],NULL);if(shaders[i])vkDestroyShaderModule(device,shaders[i],NULL);
   if(images[i].view)vkDestroyImageView(device,images[i].view,NULL);if(images[i].image)vkDestroyImage(device,images[i].image,NULL);if(images[i].memory)vkFreeMemory(device,images[i].memory,NULL);
  }
  for(unsigned i=0;i<9;i++){if(buffers[i].mapped)vkUnmapMemory(device,buffers[i].memory);if(buffers[i].buffer)vkDestroyBuffer(device,buffers[i].buffer,NULL);if(buffers[i].memory)vkFreeMemory(device,buffers[i].memory,NULL);}
  vkDestroyDevice(device,NULL);
 }
 if(instance && vkDestroyInstance)vkDestroyInstance(instance,NULL);if(library)FreeLibrary(library);
 free(depth);InterlockedExchange(&draw_busy,0);return status;
}
#ifndef AC_VULKAN_DRAW_LIBRARY
int main(int argc,char **argv) {
 if(argc!=11){fprintf(stderr,"usage: fragment_probe vertex.spv fragment.spv vertices0 vertices1 vcbuf3 vcbuf4 indices fcbuf4 packed-depth output.bin\n");return 2;}
 uint32_t *code[2]={malloc(1024*1024),malloc(1024*1024)},*depth=malloc(PIXELS*4);
 unsigned char v0[48],v1[32],c3[256],c4[256],indices[8],fc4[256];size_t code_size[2]={0};
 if(!code[0] || !code[1] || !depth)goto invalid;
 code_size[0]=read_module(argv[1],code[0]);code_size[1]=read_module(argv[2],code[1]);
 if(!code_size[0] || !code_size[1] || !read_exact(argv[3],v0,48) || !read_exact(argv[4],v1,32) ||
  !read_exact(argv[5],c3,256) || !read_exact(argv[6],c4,256) || !read_exact(argv[7],indices,8) ||
  !read_exact(argv[8],fc4,256) || !read_exact(argv[9],depth,PIXELS*4))goto invalid;


 uint8_t *output=malloc(OUTPUT_BYTES);if(!output)goto invalid;
 AcVulkanDraw request={0};request.version=1;request.struct_size=sizeof(request);
 request.inputs[0]=(AcDrawInput){code[0],code_size[0]};request.inputs[1]=(AcDrawInput){code[1],code_size[1]};
 request.inputs[2]=(AcDrawInput){v0,48};request.inputs[3]=(AcDrawInput){v1,32};request.inputs[4]=(AcDrawInput){c3,256};request.inputs[5]=(AcDrawInput){c4,256};
 request.inputs[6]=(AcDrawInput){indices,8};request.inputs[7]=(AcDrawInput){fc4,256};request.inputs[8]=(AcDrawInput){depth,PIXELS*4};
 request.output=output;request.output_size=OUTPUT_BYTES;
 int status=ac_vulkan_draw(&request);
 if(!status){FILE *f=fopen(argv[10],"wbx");if(!f)status=1;else {size_t n=fwrite(output,1,OUTPUT_BYTES,f);int closed=fclose(f);if(n!=OUTPUT_BYTES || closed)status=1;}}
 free(output);free(code[0]);free(code[1]);free(depth);return status;
invalid:
 fprintf(stderr,"invalid fragment probe input extent/header or indices\n");free(code[0]);free(code[1]);free(depth);return 2;
}
#endif
