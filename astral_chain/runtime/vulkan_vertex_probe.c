/* Execute an instrumented vertex candidate using captured first-draw buffers.
 * Rasterization is discarded: this verifies vertex math, not clipping/pixels.
 * Instrumentation stores vec4 position/varying pairs at set 0, binding 20. */
#include "vulkan_probe_support.h"
int main(int argc,char **argv) {
 if(argc!=8) {fprintf(stderr,"usage: vertex_probe instrumented.spv vertices0 vertices1 cbuf3 cbuf4 indices output.bin\n");return 2;}
 uint32_t *code=malloc(1024*1024);unsigned char v0[48],v1[32],c3[256],c4[256],indices[8];
 if(!code)return 2;FILE *f=fopen(argv[1],"rb");if(!f){free(code);return 2;}
 size_t code_size=fread(code,1,1024*1024,f);int extra=fgetc(f),bad=ferror(f);fclose(f);
 if(extra!=EOF || bad || code_size<20 || code_size%4 || code[0]!=0x07230203 || code[4] ||
  !read_exact(argv[2],v0,sizeof(v0)) || !read_exact(argv[3],v1,sizeof(v1)) ||
  !read_exact(argv[4],c3,sizeof(c3)) || !read_exact(argv[5],c4,sizeof(c4)) || !read_exact(argv[6],indices,sizeof(indices))) {
  fprintf(stderr,"invalid probe input extent/header\n");free(code);return 2;
 }
 unsigned seen=0;for(unsigned i=0;i<4;i++){unsigned n=indices[i*2]|indices[i*2+1]<<8;if(n>3 || (seen&(1u<<n))){free(code);return 2;}seen|=1u<<n;}
 int status=1;HMODULE library=NULL;VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
 VkShaderModule shader=VK_NULL_HANDLE;VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
 VkPipelineLayout layout=VK_NULL_HANDLE;VkDescriptorPool descriptor_pool=VK_NULL_HANDLE;
 VkRenderPass render_pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
 VkCommandPool command_pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;Buffer buffers[6]={0};
 library=LoadLibraryExW(L"vulkan-1.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)goto cleanup;
 PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(library,"vkGetInstanceProcAddr");if(!get)goto cleanup;
 PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");if(!create)goto cleanup;
 VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"Astral Chain vertex research",1,NULL,0,VK_API_VERSION_1_2};
 VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;CHECK(create(&ici,NULL,&instance));
#define LOAD_INSTANCE(name) name=(PFN_##name)get(instance,#name);if(!name)goto cleanup;
 INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
 VkPhysicalDevice physicals[16];uint32_t count=16;CHECK(vkEnumeratePhysicalDevices(instance,&count,physicals));
 VkPhysicalDevice physical=VK_NULL_HANDLE;uint32_t family=0;VkPhysicalDeviceFeatures enabled={0};
 for(unsigned pass=0;pass<2 && !physical;pass++)for(unsigned i=0;i<count;i++) {
  VkPhysicalDeviceProperties p;VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceProperties(physicals[i],&p);vkGetPhysicalDeviceFeatures(physicals[i],&features);
  if(p.apiVersion<VK_API_VERSION_1_2 || !features.vertexPipelineStoresAndAtomics || (!pass && p.deviceType!=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))continue;
  uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&n,NULL);if(!n || n>64)continue;
  VkQueueFamilyProperties q[64];vkGetPhysicalDeviceQueueFamilyProperties(physicals[i],&n,q);
  for(unsigned j=0;j<n;j++)if(q[j].queueCount && (q[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
   physical=physicals[i];family=j;enabled.vertexPipelineStoresAndAtomics=VK_TRUE;enabled.shaderInt64=features.shaderInt64;
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
 CHECK(buffer_create(device,&memory,&buffers[0],48,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,v0));
 CHECK(buffer_create(device,&memory,&buffers[1],32,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,v1));
 CHECK(buffer_create(device,&memory,&buffers[2],256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,c3));
 CHECK(buffer_create(device,&memory,&buffers[3],256,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,c4));
 CHECK(buffer_create(device,&memory,&buffers[4],8,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices));
 CHECK(buffer_create(device,&memory,&buffers[5],128,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,NULL));
 VkShaderModuleCreateInfo sci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,code_size,code};CHECK(vkCreateShaderModule(device,&sci,NULL,&shader));
 VkDescriptorSetLayoutBinding bindings[]={{3,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL},
  {4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL},{20,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL}};
 VkDescriptorSetLayoutCreateInfo slci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,3,bindings};CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
 VkPipelineLayoutCreateInfo plci={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&set_layout};CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
 VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
 VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,1,2,sizes};CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&descriptor_pool));
 VkDescriptorSetAllocateInfo dsai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,NULL,descriptor_pool,1,&set_layout};VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
 VkDescriptorBufferInfo infos[]={{buffers[2].buffer,0,256},{buffers[3].buffer,0,256},{buffers[5].buffer,0,128}};
 VkWriteDescriptorSet writes[3]={0};for(unsigned i=0;i<3;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=bindings[i].binding;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pBufferInfo=&infos[i];}vkUpdateDescriptorSets(device,3,writes,0,NULL);
 VkSubpassDescription subpass={0};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
 VkRenderPassCreateInfo rpci={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rpci.subpassCount=1;rpci.pSubpasses=&subpass;CHECK(vkCreateRenderPass(device,&rpci,NULL,&render_pass));
 VkFramebufferCreateInfo fbci={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fbci.renderPass=render_pass;fbci.width=1;fbci.height=1;fbci.layers=1;CHECK(vkCreateFramebuffer(device,&fbci,NULL,&framebuffer));
 VkPipelineShaderStageCreateInfo stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,shader,"main",NULL};
 VkVertexInputBindingDescription vb[]={{0,12,VK_VERTEX_INPUT_RATE_VERTEX},{1,8,VK_VERTEX_INPUT_RATE_VERTEX}};
 VkVertexInputAttributeDescription va[]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{5,1,VK_FORMAT_R32G32_SFLOAT,0}};
 VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,NULL,0,2,vb,2,va};
 VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,NULL,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,VK_FALSE};
 VkPipelineRasterizationStateCreateInfo raster={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.rasterizerDiscardEnable=VK_TRUE;raster.lineWidth=1;
 VkGraphicsPipelineCreateInfo pci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pci.stageCount=1;pci.pStages=&stage;pci.pVertexInputState=&vi;pci.pInputAssemblyState=&ia;pci.pRasterizationState=&raster;pci.layout=layout;pci.renderPass=render_pass;pci.basePipelineIndex=-1;
 CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
 VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,0,family};CHECK(vkCreateCommandPool(device,&cpci,NULL,&command_pool));
 VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,command_pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
 VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
 VkRenderPassBeginInfo rpbi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rpbi.renderPass=render_pass;rpbi.framebuffer=framebuffer;rpbi.renderArea.extent.width=1;rpbi.renderArea.extent.height=1;
 vkCmdBeginRenderPass(command,&rpbi,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
 vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL);
 VkBuffer vertex_buffers[]={buffers[0].buffer,buffers[1].buffer};VkDeviceSize offsets[]={0,0};vkCmdBindVertexBuffers(command,0,2,vertex_buffers,offsets);
 vkCmdBindIndexBuffer(command,buffers[4].buffer,0,VK_INDEX_TYPE_UINT16);vkCmdDrawIndexed(command,4,1,0,0,0);vkCmdEndRenderPass(command);
 VkMemoryBarrier barrier={VK_STRUCTURE_TYPE_MEMORY_BARRIER,NULL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
 vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
 VkFenceCreateInfo fci={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(vkCreateFence(device,&fci,NULL,&fence));
 VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;CHECK(vkQueueSubmit(queue,1,&submit,fence));
 VkResult waited=vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull);
 if(waited!=VK_SUCCESS){fprintf(stderr,"vertex execution completion unavailable: %d\n",waited);return 1;}
 f=fopen(argv[7],"wbx");if(!f){perror("exclusive output");goto cleanup;}
 size_t written=fwrite(buffers[5].mapped,1,128,f);int closed=fclose(f);if(written!=128 || closed)goto cleanup;
 printf("indexedVertices=4 outputBytes=128 completed=1 rasterization=discarded\n");status=0;
cleanup:
 if(device && vkDestroyDevice) {
  if(fence)vkDestroyFence(device,fence,NULL);if(command_pool)vkDestroyCommandPool(device,command_pool,NULL);
  if(pipeline)vkDestroyPipeline(device,pipeline,NULL);if(framebuffer)vkDestroyFramebuffer(device,framebuffer,NULL);
  if(render_pass)vkDestroyRenderPass(device,render_pass,NULL);if(descriptor_pool)vkDestroyDescriptorPool(device,descriptor_pool,NULL);
  if(layout)vkDestroyPipelineLayout(device,layout,NULL);if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,NULL);
  if(shader)vkDestroyShaderModule(device,shader,NULL);
  for(unsigned i=0;i<6;i++){if(buffers[i].mapped)vkUnmapMemory(device,buffers[i].memory);if(buffers[i].buffer)vkDestroyBuffer(device,buffers[i].buffer,NULL);if(buffers[i].memory)vkFreeMemory(device,buffers[i].memory,NULL);}
  vkDestroyDevice(device,NULL);
 }
 if(instance && vkDestroyInstance)vkDestroyInstance(instance,NULL);if(library)FreeLibrary(library);free(code);return status;
}
