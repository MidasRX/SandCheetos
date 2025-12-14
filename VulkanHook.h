#pragma once
#include <vulkan/vulkan.h>
#include <windows.h>
#include <cstdint>
#include <iostream>

namespace VulkanHook {
    
    // Vulkan state captured from S&box
    extern VkInstance g_Instance;
    extern VkPhysicalDevice g_PhysicalDevice;
    extern VkDevice g_Device;
    extern VkQueue g_Queue;
    extern uint32_t g_QueueFamily;
    extern VkPipelineCache g_PipelineCache;
    extern VkDescriptorPool g_DescriptorPool;
    extern VkRenderPass g_RenderPass;
    extern VkCommandPool g_CommandPool;
    extern VkCommandBuffer g_CommandBuffer;
    
    // Swapchain
    extern VkSwapchainKHR g_Swapchain;
    extern uint32_t g_ImageCount;
    extern uint32_t g_MinImageCount;
    
    // Window
    extern HWND g_Hwnd;
    extern int g_Width;
    extern int g_Height;
    
    // ImGui state
    extern bool g_ImGuiInitialized;
    
    // Original function pointers
    extern PFN_vkQueuePresentKHR o_vkQueuePresentKHR;
    extern PFN_vkCreateSwapchainKHR o_vkCreateSwapchainKHR;
    extern PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr;
    extern PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr;
    extern PFN_vkCreateDevice o_vkCreateDevice;
    
    // Hook functions
    bool Initialize();
    void Shutdown();
    
    VkResult VKAPI_CALL hkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    VkResult VKAPI_CALL hkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, 
        const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
    PFN_vkVoidFunction VKAPI_CALL hkGetDeviceProcAddr(VkDevice device, const char* pName);
    PFN_vkVoidFunction VKAPI_CALL hkGetInstanceProcAddr(VkInstance instance, const char* pName);
    VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
    
    // ImGui setup/rendering
    void SetupImGui();
    void RenderImGui(uint32_t imageIndex);
    void CleanupImGui();
    
    // Helper to find S&box window
    HWND FindSboxWindow();
}
