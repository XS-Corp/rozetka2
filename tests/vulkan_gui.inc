/* Render using SPIR-V produced from Minecraft's actual GUI shaders. */
typedef struct {VkPipeline pipeline;VkPipelineLayout layout;VkDescriptorSetLayout set_layout;VkDescriptorPool descriptor_pool;VkImageView view;VkBuffer buffer;VkDeviceMemory memory;} gui_resources;
static uint32_t *gui_shader_bytes(const char *path,size_t *size)
{
    FILE *file=fopen(path,"rb");require(file&&fseek(file,0,SEEK_END)==0,"open compiled game shader");long length=ftell(file);require(length>=20&&length%4==0&&fseek(file,0,SEEK_SET)==0,"compiled shader size");
    uint32_t *bytes=malloc((size_t)length);require(bytes&&fread(bytes,1,(size_t)length,file)==(size_t)length,"read game SPIR-V");fclose(file);*size=(size_t)length;return bytes;
}
static void gui_record(PFN_vkGetDeviceProcAddr get,PFN_vkGetPhysicalDeviceMemoryProperties properties,VkPhysicalDevice physical,VkDevice device,
                       VkCommandBuffer command,VkImage image,VkFormat format,VkExtent2D extent,const char *vertex,const char *fragment,gui_resources *resources)
{
#define GUI_PROC(name) PFN_##name name=(void *)get(device,#name);require(name!=NULL,#name)
    GUI_PROC(vkCreateShaderModule);GUI_PROC(vkDestroyShaderModule);GUI_PROC(vkCreateImageView);
    GUI_PROC(vkCreateDescriptorSetLayout);GUI_PROC(vkCreatePipelineLayout);GUI_PROC(vkCreateGraphicsPipelines);
    GUI_PROC(vkCreateDescriptorPool);GUI_PROC(vkAllocateDescriptorSets);GUI_PROC(vkUpdateDescriptorSets);
    GUI_PROC(vkCreateBuffer);GUI_PROC(vkGetBufferMemoryRequirements);GUI_PROC(vkAllocateMemory);GUI_PROC(vkBindBufferMemory);GUI_PROC(vkMapMemory);GUI_PROC(vkUnmapMemory);
    GUI_PROC(vkCmdPipelineBarrier);GUI_PROC(vkCmdBeginRenderingKHR);GUI_PROC(vkCmdEndRenderingKHR);GUI_PROC(vkCmdBindPipeline);GUI_PROC(vkCmdBindDescriptorSets);GUI_PROC(vkCmdBindVertexBuffers);GUI_PROC(vkCmdDraw);
    VkShaderModule modules[2];const char *files[]={vertex,fragment};
    for(unsigned i=0;i<2;i++) {size_t size=0;uint32_t *bytes=gui_shader_bytes(files[i],&size);VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=bytes};OK(vkCreateShaderModule(device,&info,NULL,&modules[i]));free(bytes);}
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo sets={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};OK(vkCreateDescriptorSetLayout(device,&sets,NULL,&resources->set_layout));
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&resources->set_layout};OK(vkCreatePipelineLayout(device,&layout,NULL,&resources->layout));
    VkPipelineShaderStageCreateInfo stages[]={{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName="main"},{.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName="main"}};
    VkVertexInputBindingDescription binding={0,7*sizeof(float),VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,3*sizeof(float)}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&binding,.vertexAttributeDescriptionCount=2,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,(float)extent.width,(float)extent.height,0,1};VkRect2D scissor={{0,0},extent};VkPipelineViewportStateCreateInfo view={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo sampling={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    VkGraphicsPipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,.pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&view,.pRasterizationState=&raster,.pMultisampleState=&sampling,.pColorBlendState=&blend,.layout=resources->layout};
    OK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,NULL,&resources->pipeline));for(unsigned i=0;i<2;i++)vkDestroyShaderModule(device,modules[i],NULL);
    VkBufferCreateInfo buffer={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};OK(vkCreateBuffer(device,&buffer,NULL,&resources->buffer));
    VkMemoryRequirements needs;vkGetBufferMemoryRequirements(device,resources->buffer,&needs);VkPhysicalDeviceMemoryProperties memory;properties(physical,&memory);uint32_t kind=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((needs.memoryTypeBits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){kind=i;break;}
    require(kind!=UINT32_MAX,"game shader uniform memory");VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=needs.size,.memoryTypeIndex=kind};OK(vkAllocateMemory(device,&allocation,NULL,&resources->memory));OK(vkBindBufferMemory(device,resources->buffer,resources->memory,0));
    uint8_t *mapped=NULL;OK(vkMapMemory(device,resources->memory,0,1024,0,(void **)&mapped));memset(mapped,0,1024);
    float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};memcpy(mapped,identity,64);memcpy(mapped+64,identity,64);memcpy(mapped+256,identity,64);float color[]={1,1,1,1};memcpy(mapped+128,color,sizeof(color));
    const float vertices[]={-1,-1,0,1,0,0,1, 1,-1,0,0,1,0,1, 0,1,0,0,0,1,1};memcpy(mapped+512,vertices,sizeof(vertices));vkUnmapMemory(device,resources->memory);
    VkDescriptorPoolSize pool_size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2};VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};OK(vkCreateDescriptorPool(device,&pool,NULL,&resources->descriptor_pool));
    VkDescriptorSetAllocateInfo descriptor_allocation={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=resources->descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&resources->set_layout};VkDescriptorSet descriptor;OK(vkAllocateDescriptorSets(device,&descriptor_allocation,&descriptor));
    VkDescriptorBufferInfo buffers[]={{resources->buffer,0,160},{resources->buffer,256,64}};VkWriteDescriptorSet writes[]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=descriptor,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[0]},{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=descriptor,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[1]}};vkUpdateDescriptorSets(device,2,writes,0,NULL);
    VkImageViewCreateInfo image_view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};OK(vkCreateImageView(device,&image_view,NULL,&resources->view));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkRenderingAttachmentInfo color_attachment={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=resources->view,.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo render={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},extent},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&color_attachment};vkCmdBeginRenderingKHR(command,&render);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,resources->pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,resources->layout,0,1,&descriptor,0,NULL);VkDeviceSize offset=512;vkCmdBindVertexBuffers(command,0,1,&resources->buffer,&offset);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderingKHR(command);
}
static void gui_destroy(PFN_vkGetDeviceProcAddr get,VkDevice device,gui_resources *resources)
{
    GUI_PROC(vkDestroyPipeline);GUI_PROC(vkDestroyPipelineLayout);GUI_PROC(vkDestroyDescriptorSetLayout);GUI_PROC(vkDestroyDescriptorPool);GUI_PROC(vkDestroyImageView);GUI_PROC(vkDestroyBuffer);GUI_PROC(vkFreeMemory);
    vkDestroyPipeline(device,resources->pipeline,NULL);vkDestroyPipelineLayout(device,resources->layout,NULL);vkDestroyDescriptorPool(device,resources->descriptor_pool,NULL);vkDestroyDescriptorSetLayout(device,resources->set_layout,NULL);vkDestroyImageView(device,resources->view,NULL);vkDestroyBuffer(device,resources->buffer,NULL);vkFreeMemory(device,resources->memory,NULL);
}
#undef GUI_PROC
