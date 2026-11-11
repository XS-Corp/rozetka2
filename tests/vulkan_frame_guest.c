/* Exercise the graphics ABI needed after Minecraft creates its Vulkan device. */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL Vulkan frame: %s\n",message);exit(91);}}
#define OK(call) do {VkResult status=(call);if(status!=VK_SUCCESS)fprintf(stderr,"Vulkan result=%d\n",status);require(status==VK_SUCCESS,#call);} while(0)
#include "vulkan_gui.inc"
int main(int argc,char **argv) {
    bool draw=argc==3;gui_resources gui={0};
    void *sdl=dlopen("libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);require(sdl!=NULL,"SDL3 bridge");
#define SDL_FN(result,name,args) result (*name)args=dlsym(sdl,#name);require(name!=NULL,#name)
    SDL_FN(bool,SDL_Init,(uint32_t));SDL_FN(bool,SDL_Vulkan_LoadLibrary,(const char *));
    SDL_FN(void *,SDL_CreateWindow,(const char *,int,int,uint64_t));SDL_FN(void,SDL_DestroyWindow,(void *));SDL_FN(void,SDL_Quit,(void));
    SDL_FN(PFN_vkGetInstanceProcAddr,SDL_Vulkan_GetVkGetInstanceProcAddr,(void));SDL_FN(const char *const *,SDL_Vulkan_GetInstanceExtensions,(uint32_t *));
    SDL_FN(bool,SDL_Vulkan_CreateSurface,(void *,VkInstance,const void *,VkSurfaceKHR *));
    require(SDL_Init(0x20)&&SDL_Vulkan_LoadLibrary(NULL),"SDL video/Vulkan initialization");
    void *window=SDL_CreateWindow("rozetka2 Vulkan frame",320,200,UINT64_C(0x10000008));require(window!=NULL,"hidden Vulkan window");
    PFN_vkGetInstanceProcAddr get=SDL_Vulkan_GetVkGetInstanceProcAddr();require(get!=NULL,"Vulkan procedure getter");
#define INSTANCE(name) PFN_##name name=(void *)get(instance,#name);require(name!=NULL,#name)
    VkInstance instance=VK_NULL_HANDLE;INSTANCE(vkCreateInstance);
    uint32_t extension_count=0;const char *const *extensions=SDL_Vulkan_GetInstanceExtensions(&extension_count);
    VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="rozetka2 Vulkan frame",.apiVersion=draw?VK_API_VERSION_1_3:VK_API_VERSION_1_0};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application,.enabledExtensionCount=extension_count,.ppEnabledExtensionNames=extensions};
    OK(vkCreateInstance(&info,NULL,&instance));
    INSTANCE(vkEnumeratePhysicalDevices);INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);INSTANCE(vkGetPhysicalDeviceSurfaceSupportKHR);
    INSTANCE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);INSTANCE(vkGetPhysicalDeviceSurfaceFormatsKHR);INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE(vkCreateDevice);INSTANCE(vkGetDeviceProcAddr);INSTANCE(vkDestroySurfaceKHR);INSTANCE(vkDestroyInstance);
    VkSurfaceKHR surface=VK_NULL_HANDLE;require(SDL_Vulkan_CreateSurface(window,instance,NULL,&surface),"SDL Vulkan surface");
    uint32_t count=8;VkPhysicalDevice physicals[8];OK(vkEnumeratePhysicalDevices(instance,&count,physicals));require(count>0,"physical device");VkPhysicalDevice physical=physicals[0];
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count;i++){VkBool32 supported=0;OK(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&supported));if(supported&&(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;break;}}
    require(family!=UINT32_MAX,"graphics/present queue");float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    if(draw){INSTANCE(vkGetPhysicalDeviceFeatures2);VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&dynamic};vkGetPhysicalDeviceFeatures2(physical,&features);require(dynamic.dynamicRendering,"native dynamic rendering support");}
    const char *device_extensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=draw?&dynamic:NULL,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.enabledExtensionCount=draw?2:1,.ppEnabledExtensionNames=device_extensions};
    VkDevice device=VK_NULL_HANDLE;OK(vkCreateDevice(physical,&device_info,NULL,&device));
#define DEVICE(name) PFN_##name name=(void *)vkGetDeviceProcAddr(device,#name);require(name!=NULL,#name)
    DEVICE(vkGetDeviceQueue);DEVICE(vkCreateSwapchainKHR);DEVICE(vkGetSwapchainImagesKHR);DEVICE(vkAcquireNextImageKHR);DEVICE(vkDestroySwapchainKHR);
    DEVICE(vkCreateSemaphore);DEVICE(vkDestroySemaphore);DEVICE(vkCreateCommandPool);DEVICE(vkDestroyCommandPool);DEVICE(vkAllocateCommandBuffers);
    DEVICE(vkBeginCommandBuffer);DEVICE(vkEndCommandBuffer);DEVICE(vkCmdPipelineBarrier);DEVICE(vkCmdClearColorImage);DEVICE(vkCmdCopyImageToBuffer);
    DEVICE(vkCreateBuffer);DEVICE(vkGetBufferMemoryRequirements);DEVICE(vkAllocateMemory);DEVICE(vkBindBufferMemory);DEVICE(vkMapMemory);DEVICE(vkUnmapMemory);DEVICE(vkFreeMemory);DEVICE(vkDestroyBuffer);
    DEVICE(vkQueueSubmit);DEVICE(vkQueuePresentKHR);DEVICE(vkQueueWaitIdle);DEVICE(vkDestroyDevice);
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkSurfaceCapabilitiesKHR caps;OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps));
    count=0;OK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,NULL));require(count>0&&count<4096,"surface format count");VkSurfaceFormatKHR *formats=calloc(count,sizeof(*formats));require(formats!=NULL,"surface format allocation");OK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats));VkSurfaceFormatKHR chosen={0};
    for(uint32_t i=0;i<count;i++)if(formats[i].format==VK_FORMAT_B8G8R8A8_UNORM||formats[i].format==VK_FORMAT_R8G8B8A8_UNORM){chosen=formats[i];break;}
    free(formats);require(chosen.format!=0,"unorm surface format");VkExtent2D extent=caps.currentExtent;if(extent.width==UINT32_MAX)extent=(VkExtent2D){320,200};
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;require((caps.supportedUsageFlags&usage)==usage,"swapchain transfer/readback support");
    uint32_t images=caps.minImageCount+1;if(caps.maxImageCount&&images>caps.maxImageCount)images=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&alpha))alpha=(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha&-caps.supportedCompositeAlpha);
    VkSwapchainCreateInfoKHR swap_info={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,.minImageCount=images,.imageFormat=chosen.format,.imageColorSpace=chosen.colorSpace,.imageExtent=extent,.imageArrayLayers=1,.imageUsage=usage,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.currentTransform,.compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    VkSwapchainKHR swap=VK_NULL_HANDLE;OK(vkCreateSwapchainKHR(device,&swap_info,NULL,&swap));
    VkImage swap_images[16];count=16;OK(vkGetSwapchainImagesKHR(device,swap,&count,swap_images));require(count>0,"swapchain images");
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore acquired,finished;OK(vkCreateSemaphore(device,&sem_info,NULL,&acquired));OK(vkCreateSemaphore(device,&sem_info,NULL,&finished));
    uint32_t image_index=0;OK(vkAcquireNextImageKHR(device,swap,UINT64_MAX,acquired,VK_NULL_HANDLE,&image_index));
    VkDeviceSize length=(VkDeviceSize)extent.width*extent.height*4;VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=length,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;OK(vkCreateBuffer(device,&buffer_info,NULL,&buffer));VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,buffer,&requirements);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);uint32_t memory_type=UINT32_MAX;
    for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((requirements.memoryTypeBits&(1u<<i))&&((memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))){memory_type=i;break;}
    require(memory_type!=UINT32_MAX,"coherent GPU readback memory");VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=memory_type};VkDeviceMemory storage;OK(vkAllocateMemory(device,&allocation,NULL,&storage));OK(vkBindBufferMemory(device,buffer,storage,0));
    VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,.queueFamilyIndex=family};VkCommandPool pool;OK(vkCreateCommandPool(device,&pool_info,NULL,&pool));
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;OK(vkAllocateCommandBuffers(device,&command_info,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};OK(vkBeginCommandBuffer(command,&begin));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=0,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=swap_images[image_index],.subresourceRange=range};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkClearColorValue color={.float32={draw?0:0.25f,draw?0:0.5f,draw?0:0.75f,1}};vkCmdClearColorImage(command,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    if(draw)gui_record(vkGetDeviceProcAddr,vkGetPhysicalDeviceMemoryProperties,physical,device,command,barrier.image,chosen.format,extent,argv[1],argv[2],&gui);
    barrier.srcAccessMask=draw?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.oldLayout=draw?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command,draw?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={extent.width,extent.height,1}};vkCmdCopyImageToBuffer(command,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=0;barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);OK(vkEndCommandBuffer(command));
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&acquired,.pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&command,.signalSemaphoreCount=1,.pSignalSemaphores=&finished};OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
    VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&finished,.swapchainCount=1,.pSwapchains=&swap,.pImageIndices=&image_index};OK(vkQueuePresentKHR(queue,&present));OK(vkQueueWaitIdle(queue));
    uint8_t *pixels=NULL;OK(vkMapMemory(device,storage,0,length,0,(void **)&pixels));bool bgra=chosen.format==VK_FORMAT_B8G8R8A8_UNORM;
    if(draw) {
        size_t colored=0;for(VkDeviceSize i=0;i<length;i+=4){require(pixels[i+3]==255,"game shader alpha");if(pixels[i]||pixels[i+1]||pixels[i+2])colored++;}
        require(colored>extent.width*extent.height/4&&colored<extent.width*extent.height*3/4,"actual game shader triangle covers half the framebuffer");
        size_t center=((size_t)extent.height/2*extent.width+extent.width/2)*4;require(pixels[center]>25&&pixels[center+1]>25&&pixels[center+2]>25,"game shader interpolates vertex colors");
        size_t corner=((size_t)extent.height-1)*extent.width*4;require(!pixels[corner]&&!pixels[corner+1]&&!pixels[corner+2],"game shader leaves exterior clear color");
        printf("PASS actual Minecraft GUI shaders: dynamic rendering, UBOs, vertex attributes and %zu colored pixels\n",colored);
    } else for(VkDeviceSize i=0;i<length;i+=4)require(pixels[i]==(bgra?191:64)&&pixels[i+1]==128&&pixels[i+2]==(bgra?64:191)&&pixels[i+3]==255,"actual GPU RGBA readback");
    if(draw)gui_destroy(vkGetDeviceProcAddr,device,&gui);
    vkUnmapMemory(device,storage);vkDestroyCommandPool(device,pool,NULL);vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,storage,NULL);vkDestroySemaphore(device,acquired,NULL);vkDestroySemaphore(device,finished,NULL);vkDestroySwapchainKHR(device,swap,NULL);vkDestroyDevice(device,NULL);vkDestroySurfaceKHR(instance,surface,NULL);vkDestroyInstance(instance,NULL);SDL_DestroyWindow(window);SDL_Quit();dlclose(sdl);
    printf("PASS Vulkan swapchain, GPU clear/copy/present and exact %ux%u pixel readback\n",extent.width,extent.height);return 0;
}
