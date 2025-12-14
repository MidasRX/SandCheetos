#include "VulkanHook.h"
#include "../gui/gui_sbox.h"
#include "../features/visuals/esp.h"
#include "../../ext/minhook/minhook.h"
#include "../../ext/imgui/imgui.h"
#include "../../ext/imgui/backends/imgui_impl_vulkan.h"
#include "../../ext/imgui/backends/imgui_impl_win32.h"
#include <thread>
#include <chrono>
#include <cstring>
#include <psapi.h>
#include <tlhelp32.h>
#include <unordered_set>
#include <string>
#include <vector>
#include <iostream>
#pragma comment(lib, "psapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace VulkanHook {
    
    // Vulkan state
    VkInstance g_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice g_Device = VK_NULL_HANDLE;
    VkQueue g_Queue = VK_NULL_HANDLE;
    uint32_t g_QueueFamily = (uint32_t)-1;
    VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
    VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
    VkRenderPass g_RenderPass = VK_NULL_HANDLE;
    VkCommandPool g_CommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> g_CommandBuffers;
    std::vector<VkFence> g_Fences;
    VkCommandBuffer g_CurrentCommandBuffer = VK_NULL_HANDLE;
    
    VkSwapchainKHR g_Swapchain = VK_NULL_HANDLE;
    uint32_t g_ImageCount = 0;
    uint32_t g_MinImageCount = 2;
    std::vector<VkImage> g_SwapchainImages;
    std::vector<VkImageView> g_SwapchainImageViews;
    std::vector<VkFramebuffer> g_Framebuffers;
    
    HWND g_Hwnd = nullptr;
    int g_Width = 1920;
    int g_Height = 1080;
    
    bool g_ImGuiInitialized = false;
    
    PFN_vkQueuePresentKHR o_vkQueuePresentKHR = nullptr;
    PFN_vkCreateSwapchainKHR o_vkCreateSwapchainKHR = nullptr;
    PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;
    PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateDevice o_vkCreateDevice = nullptr;
    
    // Original window procedure
    WNDPROC o_WndProc = nullptr;
    
    LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        static bool debugOnce = false;
        static bool insertWasDown = false;
        
        // DEBUG: Log ALL key presses to find out what's happening
        if (msg == WM_KEYDOWN) {
            static int keyPressCount = 0;
            if (keyPressCount++ < 10) { // Log first 10 key presses
                std::cout << "[WndProc] KEY DOWN: VK=" << wParam << " (INSERT=" << VK_INSERT << ")\n";
            }
        }
        
        // Track INSERT key presses (WndProc has priority over GetAsyncKeyState)
        if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
            if (!insertWasDown) {
                GUI::g_MenuOpen = !GUI::g_MenuOpen;
                std::cout << "[WndProc] ******** INSERT PRESSED! ******** Menu toggled: " << (GUI::g_MenuOpen ? "OPEN" : "CLOSED") << "\n";
                insertWasDown = true;
            }
        }
        else if (msg == WM_KEYUP && wParam == VK_INSERT) {
            std::cout << "[WndProc] INSERT released\n";
            insertWasDown = false;
        }
        
        // Let ImGui process input if initialized
        if (g_ImGuiInitialized) {
            // Debug mouse clicks
            if (!debugOnce && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)) {
                debugOnce = true;
                std::cout << "[WndProc] Mouse click detected! MenuOpen=" << GUI::g_MenuOpen 
                          << " msg=" << msg << " (LBUTTON=" << WM_LBUTTONDOWN << ")\n";
                std::cout << "[WndProc] io.WantCaptureMouse=" << ImGui::GetIO().WantCaptureMouse << "\n";
            }
            
            // Always pass to ImGui first
            LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            
            // If menu is open, block ALL input from reaching the game
            if (GUI::g_MenuOpen) {
                switch (msg) {
                    // Block all mouse input
                    case WM_MOUSEMOVE:
                    case WM_LBUTTONDOWN:
                    case WM_LBUTTONUP:
                    case WM_RBUTTONDOWN:
                    case WM_RBUTTONUP:
                    case WM_MBUTTONDOWN:
                    case WM_MBUTTONUP:
                    case WM_MOUSEWHEEL:
                    case WM_XBUTTONDOWN:
                    case WM_XBUTTONUP:
                    // Block all keyboard input (except INSERT for toggling menu)
                    case WM_KEYDOWN:
                    case WM_KEYUP:
                    case WM_SYSKEYDOWN:
                    case WM_SYSKEYUP:
                    case WM_CHAR:
                        if (wParam != VK_INSERT) { // Allow INSERT to toggle menu
                            return 1; // Block from game
                        }
                        break;
                    case WM_INPUT: // Source 2 uses Raw Input - CRITICAL to block
                        return 1; // Block from game
                }
            }
            
            // If ImGui handled it, don't pass to game
            if (imguiResult) {
                return true;
            }
        }
        
        // Pass to original handler
        return CallWindowProc(o_WndProc, hWnd, msg, wParam, lParam);
    }
    
    HWND FindSboxWindow() {
        // Try multiple window class names
        HWND hwnd = FindWindowA("SDL_app", nullptr);
        if (hwnd) return hwnd;
        
        hwnd = FindWindowW(nullptr, L"s&box");
        if (hwnd) return hwnd;
        
        // Enumerate windows and find by process
        DWORD pid = GetCurrentProcessId();
        HWND result = nullptr;
        
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            DWORD windowPid;
            GetWindowThreadProcessId(hwnd, &windowPid);
            if (windowPid == (DWORD)lParam && IsWindowVisible(hwnd)) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&result);
        
        return result;
    }
    
    bool Initialize() {
        std::cout << "[VulkanHook] ========================================\n";
        std::cout << "[VulkanHook] Initializing Vulkan hooks...\n";
        std::cout << "[VulkanHook] ========================================\n";
        
        // Get vulkan-1.dll (wait for manual map injection)
        HMODULE vulkanModule = GetModuleHandleA("vulkan-1.dll");
        if (!vulkanModule) {
            std::cout << "[VulkanHook] vulkan-1.dll not loaded yet, waiting...\n";
            for (int i = 0; i < 50; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                vulkanModule = GetModuleHandleA("vulkan-1.dll");
                if (vulkanModule) break;
            }
            
            if (!vulkanModule) {
                std::cerr << "[VulkanHook] vulkan-1.dll failed to load!\n";
                return false;
            }
            std::cout << "[VulkanHook] vulkan-1.dll loaded!\n";
        }
        
        std::cout << "[VulkanHook] Found vulkan-1.dll at 0x" << std::hex << (uintptr_t)vulkanModule << std::dec << "\n";
        
        // Get function addresses - try hooking ALL Vulkan present/rendering functions
        std::cout << "[VulkanHook] Scanning for Vulkan rendering functions...\n";
        
        auto vkQueuePresentKHR_addr = GetProcAddress(vulkanModule, "vkQueuePresentKHR");
        auto vkCreateSwapchainKHR_addr = GetProcAddress(vulkanModule, "vkCreateSwapchainKHR");
        auto vkGetDeviceProcAddr_addr = GetProcAddress(vulkanModule, "vkGetDeviceProcAddr");
        auto vkGetInstanceProcAddr_addr = GetProcAddress(vulkanModule, "vkGetInstanceProcAddr");
        auto vkCreateDevice_addr = GetProcAddress(vulkanModule, "vkCreateDevice");
        auto vkQueueSubmit_addr = GetProcAddress(vulkanModule, "vkQueueSubmit");
        auto vkQueueSubmit2_addr = GetProcAddress(vulkanModule, "vkQueueSubmit2");
        auto vkQueueSubmit2KHR_addr = GetProcAddress(vulkanModule, "vkQueueSubmit2KHR");
        auto vkAcquireNextImageKHR_addr = GetProcAddress(vulkanModule, "vkAcquireNextImageKHR");
        auto vkAcquireNextImage2KHR_addr = GetProcAddress(vulkanModule, "vkAcquireNextImage2KHR");
        auto vkCreateInstance_addr = GetProcAddress(vulkanModule, "vkCreateInstance");
        
        std::cout << "[VulkanHook] Function addresses found:\n";
        if (vkQueuePresentKHR_addr) std::cout << "  vkQueuePresentKHR: 0x" << std::hex << (uintptr_t)vkQueuePresentKHR_addr << std::dec << "\n";
        if (vkCreateSwapchainKHR_addr) std::cout << "  vkCreateSwapchainKHR: 0x" << std::hex << (uintptr_t)vkCreateSwapchainKHR_addr << std::dec << "\n";
        if (vkGetDeviceProcAddr_addr) std::cout << "  vkGetDeviceProcAddr: 0x" << std::hex << (uintptr_t)vkGetDeviceProcAddr_addr << std::dec << "\n";
        if (vkGetInstanceProcAddr_addr) std::cout << "  vkGetInstanceProcAddr: 0x" << std::hex << (uintptr_t)vkGetInstanceProcAddr_addr << std::dec << "\n";
        if (vkCreateDevice_addr) std::cout << "  vkCreateDevice: 0x" << std::hex << (uintptr_t)vkCreateDevice_addr << std::dec << "\n";
        if (vkQueueSubmit_addr) std::cout << "  vkQueueSubmit: 0x" << std::hex << (uintptr_t)vkQueueSubmit_addr << std::dec << "\n";
        if (vkQueueSubmit2_addr) std::cout << "  vkQueueSubmit2: 0x" << std::hex << (uintptr_t)vkQueueSubmit2_addr << std::dec << "\n";
        if (vkQueueSubmit2KHR_addr) std::cout << "  vkQueueSubmit2KHR: 0x" << std::hex << (uintptr_t)vkQueueSubmit2KHR_addr << std::dec << "\n";
        if (vkAcquireNextImageKHR_addr) std::cout << "  vkAcquireNextImageKHR: 0x" << std::hex << (uintptr_t)vkAcquireNextImageKHR_addr << std::dec << "\n";
        if (vkAcquireNextImage2KHR_addr) std::cout << "  vkAcquireNextImage2KHR: 0x" << std::hex << (uintptr_t)vkAcquireNextImage2KHR_addr << std::dec << "\n";
        if (vkCreateDevice_addr) std::cout << "  vkCreateDevice: 0x" << std::hex << (uintptr_t)vkCreateDevice_addr << std::dec << "\n";
        if (vkCreateInstance_addr) std::cout << "  vkCreateInstance: 0x" << std::hex << (uintptr_t)vkCreateInstance_addr << std::dec << "\n";
        
        if (!vkQueuePresentKHR_addr || !vkCreateSwapchainKHR_addr || !vkGetDeviceProcAddr_addr) {
            std::cerr << "[VulkanHook] Failed to find critical Vulkan functions!\n";
            return false;
        }
        
        // Initialize MinHook
        if (MH_Initialize() != MH_OK) {
            std::cerr << "[VulkanHook] MinHook initialization failed!\n";
            return false;
        }
        
        std::cout << "[VulkanHook] Installing hooks (early interception)...\n";
        
        // Hook vkGetInstanceProcAddr FIRST - this is called before device creation
        if (vkGetInstanceProcAddr_addr && MH_CreateHook(vkGetInstanceProcAddr_addr, (void*)&hkGetInstanceProcAddr,
            reinterpret_cast<void**>(&o_vkGetInstanceProcAddr)) == MH_OK) {
            std::cout << "[VulkanHook] ✓ Hooked vkGetInstanceProcAddr (early interception)\n";
        }
        
        // Hook vkCreateDevice - captures device creation before dispatch table is built
        if (vkCreateDevice_addr && MH_CreateHook(vkCreateDevice_addr, (void*)&hkCreateDevice,
            reinterpret_cast<void**>(&o_vkCreateDevice)) == MH_OK) {
            std::cout << "[VulkanHook] ✓ Hooked vkCreateDevice (dispatch table interception)\n";
        }
        
        // Hook vkGetDeviceProcAddr to intercept when S&box requests function pointers
        if (MH_CreateHook(vkGetDeviceProcAddr_addr, (void*)&hkGetDeviceProcAddr,
            reinterpret_cast<void**>(&o_vkGetDeviceProcAddr)) != MH_OK) {
            std::cerr << "[VulkanHook] Failed to hook vkGetDeviceProcAddr!\n";
            return false;
        }
        
        // Hook vkCreateSwapchainKHR to capture swapchain info
        if (MH_CreateHook(vkCreateSwapchainKHR_addr, &hkCreateSwapchainKHR, 
            reinterpret_cast<void**>(&o_vkCreateSwapchainKHR)) != MH_OK) {
            std::cerr << "[VulkanHook] Failed to hook vkCreateSwapchainKHR!\n";
            return false;
        }
        
        // Hook vkQueuePresentKHR to render our overlay
        if (MH_CreateHook(vkQueuePresentKHR_addr, &hkQueuePresentKHR, 
            reinterpret_cast<void**>(&o_vkQueuePresentKHR)) != MH_OK) {
            std::cerr << "[VulkanHook] Failed to hook vkQueuePresentKHR!\n";
            return false;
        }
        
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            std::cerr << "[VulkanHook] Failed to enable hooks!\n";
            return false;
        }
        
        std::cout << "[VulkanHook] ========================================\n";
        std::cout << "[VulkanHook] Vulkan hooks installed successfully!\n";
        std::cout << "[VulkanHook] ========================================\n";
        
        // Try to find vkQueuePresentKHR in rendersystemvulkan.dll
        HMODULE renderSystem = GetModuleHandleA("rendersystemvulkan.dll");
        if (renderSystem) {
            std::cout << "[VulkanHook] ========================================\n";
            std::cout << "[VulkanHook] Analyzing rendersystemvulkan.dll...\n";
            std::cout << "[VulkanHook] ========================================\n";
            
            uintptr_t base = (uintptr_t)renderSystem;
            MODULEINFO modInfo;
            if (GetModuleInformation(GetCurrentProcess(), renderSystem, &modInfo, sizeof(modInfo))) {
                std::cout << "[VulkanHook] Base: 0x" << std::hex << base << std::dec << "\n";
                std::cout << "[VulkanHook] Size: 0x" << std::hex << modInfo.SizeOfImage << std::dec << " bytes\n";
                
                // Scan for references to vkQueuePresentKHR address
                uintptr_t targetAddr = (uintptr_t)vkQueuePresentKHR_addr;
                int foundReferences = 0;
                
                std::cout << "[VulkanHook] Scanning for references to vkQueuePresentKHR (0x" << std::hex << targetAddr << std::dec << ")...\n";
                
                for (uintptr_t addr = base; addr < base + modInfo.SizeOfImage - 8; addr++) {
                    // Use IsBadReadPtr instead of SEH
                    if (!IsBadReadPtr((void*)addr, sizeof(uintptr_t))) {
                        uintptr_t value = *(uintptr_t*)addr;
                        if (value == targetAddr) {
                            std::cout << "[VulkanHook] Found reference at 0x" << std::hex << addr << std::dec << " (offset +0x" << std::hex << (addr - base) << std::dec << ")\n";
                            foundReferences++;
                            if (foundReferences >= 10) {
                                std::cout << "[VulkanHook] ... (stopping after 10 references)\n";
                                break;
                            }
                        }
                    }
                }
                
                if (foundReferences == 0) {
                    std::cout << "[VulkanHook] No direct references found - S&box uses dispatch table\n";
                    std::cout << "[VulkanHook] Searching for VkDevice dispatch tables in memory...\n";
                    
                    // S&box's dispatch table will have vkQueuePresentKHR somewhere
                    // Look for clusters of Vulkan function pointers
                    std::vector<uintptr_t> potentialTables;
                    int consecutiveVulkanPointers = 0;
                    uintptr_t tableStart = 0;
                    
                    for (uintptr_t addr = base; addr < base + modInfo.SizeOfImage - 8; addr += 8) {
                        if (!IsBadReadPtr((void*)addr, sizeof(uintptr_t))) {
                            uintptr_t value = *(uintptr_t*)addr;
                            
                            // Check if this looks like a Vulkan function pointer
                            // (points into vulkan-1.dll range)
                            uintptr_t vulkanBase = (uintptr_t)vulkanModule;
                            if (value >= vulkanBase && value < vulkanBase + 0x100000) {
                                if (consecutiveVulkanPointers == 0) {
                                    tableStart = addr;
                                }
                                consecutiveVulkanPointers++;
                                
                                // Dispatch tables usually have 50+ function pointers
                                if (consecutiveVulkanPointers >= 20) {
                                    std::cout << "[VulkanHook] Found potential dispatch table at 0x" << std::hex << tableStart << std::dec;
                                    std::cout << " (+" << consecutiveVulkanPointers << " Vulkan pointers)\n";
                                    potentialTables.push_back(tableStart);
                                    consecutiveVulkanPointers = 0;
                                }
                            } else {
                                consecutiveVulkanPointers = 0;
                            }
                        }
                    }
                    
                    if (!potentialTables.empty()) {
                        std::cout << "[VulkanHook] Found " << potentialTables.size() << " potential dispatch table(s)\n";
                        std::cout << "[VulkanHook] Scanning tables for vkQueuePresentKHR pointer...\n";
                        
                        // Search each table for vkQueuePresentKHR
                        for (uintptr_t tableAddr : potentialTables) {
                            // Scan up to 200 entries (typical device dispatch table size)
                            for (int i = 0; i < 200; i++) {
                                uintptr_t* entry = (uintptr_t*)(tableAddr + i * 8);
                                if (!IsBadReadPtr(entry, sizeof(uintptr_t))) {
                                    if (*entry == targetAddr) {
                                        std::cout << "\n[VulkanHook] ========================================\n";
                                        std::cout << "[VulkanHook] FOUND IT! vkQueuePresentKHR in dispatch table!\n";
                                        std::cout << "[VulkanHook] Table: 0x" << std::hex << tableAddr << std::dec << "\n";
                                        std::cout << "[VulkanHook] Entry: 0x" << std::hex << (uintptr_t)entry << std::dec << " (index " << i << ")\n";
                                        std::cout << "[VulkanHook] Current value: 0x" << std::hex << *entry << std::dec << "\n";
                                        std::cout << "[VulkanHook] ========================================\n";
                                        
                                        // Patch it to point to our hook!
                                        DWORD oldProtect;
                                        if (VirtualProtect(entry, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                                            *entry = (uintptr_t)&hkQueuePresentKHR;
                                            VirtualProtect(entry, sizeof(uintptr_t), oldProtect, &oldProtect);
                                            
                                            std::cout << "[VulkanHook] ✓ PATCHED! Dispatch table now points to our hook\n";
                                            std::cout << "[VulkanHook] New value: 0x" << std::hex << *entry << std::dec << "\n";
                                            std::cout << "[VulkanHook] S&box will now call our vkQueuePresentKHR!\n";
                                            std::cout << "[VulkanHook] ========================================\n\n";
                                        } else {
                                            std::cerr << "[VulkanHook] Failed to change memory protection!\n";
                                        }
                                        
                                        goto found_and_patched;
                                    }
                                }
                            }
                        }
                        
                        found_and_patched:
                        (void)0; // Label target
                    } else {
                        std::cout << "[VulkanHook] No dispatch tables found - may need different approach\n";
                    }
                }
            }
            std::cout << "[VulkanHook] ========================================\n";
        } else {
            std::cout << "[VulkanHook] rendersystemvulkan.dll not found for analysis\n";
        }
        
        // Find and hook window immediately for input
        g_Hwnd = FindSboxWindow();
        if (g_Hwnd) {
            std::cout << "[VulkanHook] Found S&box window: 0x" << std::hex << (uintptr_t)g_Hwnd << std::dec << "\n";
            o_WndProc = (WNDPROC)SetWindowLongPtr(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
            std::cout << "[VulkanHook] Window procedure hooked - input active!\n";
            std::cout << "[VulkanHook] ImGui will be initialized after capturing Vulkan objects\n";
        } else {
            std::cout << "[VulkanHook] Window not found yet - will setup on first Present\n";
        }
        
        // Start monitoring thread to see if hooks are being called
        std::thread([]() {
            for (int i = 0; i < 10; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::cout << "[VulkanHook] Status check " << (i+1) << "/10: ";
                std::cout << "Device=" << (g_Device ? "CAPTURED" : "null") << ", ";
                std::cout << "Swapchain=" << (g_Swapchain ? "CAPTURED" : "null") << ", ";
                std::cout << "Queue=" << (g_Queue ? "CAPTURED" : "null") << "\n";
            }
            
            if (!g_Device && !g_Swapchain && !g_Queue) {
                std::cout << "\n[VulkanHook] ========================================\n";
                std::cout << "[VulkanHook] CRITICAL: No Vulkan hooks triggered!\n";
                std::cout << "[VulkanHook] S&box created its device BEFORE injection\n";
                std::cout << "[VulkanHook] Try injecting earlier (at process start)\n";
                std::cout << "[VulkanHook] ========================================\n\n";
            }
        }).detach();
        
        return true;
    }
    
    void Shutdown() {
        std::cout << "[VulkanHook] Shutting down...\n";
        
        if (g_ImGuiInitialized) {
            CleanupImGui();
        }
        
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        
        if (o_WndProc && g_Hwnd) {
            SetWindowLongPtr(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)o_WndProc);
        }
        
        std::cout << "[VulkanHook] Shutdown complete\n";
    }
    
    VkResult VKAPI_CALL hkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
        
        static bool firstCall = true;
        if (firstCall) {
            std::cout << "\n[VulkanHook] ========================================\n";
            std::cout << "[VulkanHook] *** vkCreateSwapchainKHR HOOKED! ***\n";
            std::cout << "[VulkanHook] ========================================\n";
            firstCall = false;
            
            // Capture device if we don't have it yet (mid-game injection)
            if (!g_Device) {
                g_Device = device;
                std::cout << "[VulkanHook] Device captured from swapchain: 0x" << std::hex << (uintptr_t)g_Device << std::dec << "\n";
            }
        }
        
        std::cout << "[VulkanHook] Swapchain extent: " << pCreateInfo->imageExtent.width << "x" << pCreateInfo->imageExtent.height << "\n";
        std::cout << "[VulkanHook] Image count: " << pCreateInfo->minImageCount << "\n";
        
        // Update dimensions
        g_Width = pCreateInfo->imageExtent.width;
        g_Height = pCreateInfo->imageExtent.height;
        g_MinImageCount = pCreateInfo->minImageCount;
        
        // Call original
        VkResult result = o_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        
        if (result == VK_SUCCESS) {
            g_Swapchain = *pSwapchain;
            std::cout << "[VulkanHook] Swapchain created: 0x" << std::hex << (uintptr_t)g_Swapchain << std::dec << "\n";
            
            // Query actual image count from swapchain
            uint32_t imageCount = 0;
            vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
            g_ImageCount = imageCount;
            std::cout << "[VulkanHook] Actual swapchain image count: " << g_ImageCount << "\n";
            
            // Get swapchain images
            g_SwapchainImages.resize(g_ImageCount);
            vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, g_SwapchainImages.data());
            std::cout << "[VulkanHook] Retrieved " << g_ImageCount << " swapchain images\n";
        }
        
        return result;
    }
    
    VkResult VKAPI_CALL hkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
        static bool firstCall = true;
        
        if (firstCall) {
            std::cout << "\n[VulkanHook] ========================================\n";
            std::cout << "[VulkanHook] *** vkQueuePresentKHR HOOKED! ***\n";
            std::cout << "[VulkanHook] ========================================\n";
            firstCall = false;
            
            // Capture queue
            g_Queue = queue;
            std::cout << "[VulkanHook] Queue captured: 0x" << std::hex << (uintptr_t)g_Queue << std::dec << "\n";
            
            // For mid-game injection: Get VkInstance and VkPhysicalDevice from device
            if (!g_Instance && g_Device) {
                // VkDevice contains pointer to instance in its internal structure
                // We'll use vkEnumeratePhysicalDevices to get them properly
                std::cout << "[VulkanHook] Getting VkInstance for mid-game injection...\n";
                
                // Find queue family by calling vkGetDeviceQueue with queue family 0
                // This is safe because we already have the queue
                g_QueueFamily = 0; // Graphics queue is typically family 0
                std::cout << "[VulkanHook] Using queue family 0 (graphics queue)\n";
                
                // For ImGui we need VkInstance - we'll get it from VkPhysicalDevice
                // Use a workaround: Get physical device from enumerate
                HMODULE vulkanModule = GetModuleHandleA("vulkan-1.dll");
                if (!vulkanModule) {
                    std::cerr << "[VulkanHook] vulkan-1.dll not loaded!\n";
                } else {
                    auto vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)GetProcAddress(
                        vulkanModule, "vkEnumeratePhysicalDevices");
                    
                    if (vkEnumeratePhysicalDevices) {
                        // We need the instance - try to find it from rendersystemvulkan.dll
                        // For now, create a temporary minimal instance
                        auto vkCreateInstance = (PFN_vkCreateInstance)GetProcAddress(
                            vulkanModule, "vkCreateInstance");
                        
                        if (vkCreateInstance) {
                            VkApplicationInfo appInfo = {};
                            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                            appInfo.pApplicationName = "Sigma";
                            appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
                            appInfo.pEngineName = "Sigma";
                            appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
                            appInfo.apiVersion = VK_API_VERSION_1_2;
                            
                            VkInstanceCreateInfo createInfo = {};
                            createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
                            createInfo.pApplicationInfo = &appInfo;
                            
                            if (vkCreateInstance(&createInfo, nullptr, &g_Instance) == VK_SUCCESS) {
                                std::cout << "[VulkanHook] Created temporary VkInstance: 0x" << std::hex << (uintptr_t)g_Instance << std::dec << "\n";
                                
                                // Get physical device
                                uint32_t deviceCount = 0;
                                vkEnumeratePhysicalDevices(g_Instance, &deviceCount, nullptr);
                                if (deviceCount > 0) {
                                    VkPhysicalDevice devices[8];
                                    vkEnumeratePhysicalDevices(g_Instance, &deviceCount, devices);
                                    g_PhysicalDevice = devices[0]; // Use first GPU
                                    std::cout << "[VulkanHook] Physical device: 0x" << std::hex << (uintptr_t)g_PhysicalDevice << std::dec << "\n";
                                }
                            }
                        }
                    }
                }
            }
            
            // Setup window/ImGui if not already done
            if (!g_Hwnd) {
                g_Hwnd = FindSboxWindow();
                if (g_Hwnd) {
                    std::cout << "[VulkanHook] Found S&box window: 0x" << std::hex << (uintptr_t)g_Hwnd << std::dec << "\n";
                    o_WndProc = (WNDPROC)SetWindowLongPtr(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
                    std::cout << "[VulkanHook] Hooked window procedure\n";
                } else {
                    std::cerr << "[VulkanHook] Failed to find S&box window!\n";
                }
            }
            
            // Now we have ALL Vulkan objects - initialize ImGui
            if (g_Hwnd && !g_ImGuiInitialized && g_Instance && g_PhysicalDevice && g_Device) {
                std::cout << "[VulkanHook] All Vulkan objects ready - initializing ImGui!\n";
                SetupImGui();
            }
        }
        
        // Render ImGui overlay
        if (g_ImGuiInitialized && pPresentInfo && pPresentInfo->swapchainCount > 0) {
            // Get the current swapchain image index
            uint32_t imageIndex = pPresentInfo->pImageIndices ? pPresentInfo->pImageIndices[0] : 0;
            RenderImGui(imageIndex);
        }
        
        // Call original
        return o_vkQueuePresentKHR(queue, pPresentInfo);
    }
    
    PFN_vkVoidFunction VKAPI_CALL hkGetDeviceProcAddr(VkDevice device, const char* pName) {
        // Log ALL function requests to see what S&box is actually using
        static std::unordered_set<std::string> loggedFunctions;
        static int callCount = 0;
        callCount++;
        
        if (callCount == 1) {
            std::cout << "\n[VulkanHook] ========================================\n";
            std::cout << "[VulkanHook] *** vkGetDeviceProcAddr HOOKED! ***\n";
            std::cout << "[VulkanHook] S&box is requesting function pointers!\n";
            std::cout << "[VulkanHook] ========================================\n";
        }
        
        if (pName && loggedFunctions.find(pName) == loggedFunctions.end()) {
            std::cout << "[VulkanHook] vkGetDeviceProcAddr: " << pName << "\n";
            loggedFunctions.insert(pName);
        }
        
        // Intercept critical rendering functions
        if (pName && strcmp(pName, "vkQueuePresentKHR") == 0) {
            std::cout << "[VulkanHook] *** S&box requested vkQueuePresentKHR! Returning hooked function ***\n";
            return (PFN_vkVoidFunction)hkQueuePresentKHR;
        }
        
        if (pName && strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            std::cout << "[VulkanHook] *** S&box requested vkCreateSwapchainKHR! Returning hooked function ***\n";
            return (PFN_vkVoidFunction)hkCreateSwapchainKHR;
        }
        
        if (pName && strcmp(pName, "vkQueueSubmit") == 0) {
            std::cout << "[VulkanHook] S&box uses vkQueueSubmit (not vkQueueSubmit2)\n";
        }
        
        if (pName && strcmp(pName, "vkAcquireNextImageKHR") == 0) {
            std::cout << "[VulkanHook] S&box uses vkAcquireNextImageKHR\n";
        }
        
        // Call original for everything else
        return o_vkGetDeviceProcAddr(device, pName);
    }
    
    PFN_vkVoidFunction VKAPI_CALL hkGetInstanceProcAddr(VkInstance instance, const char* pName) {
        static int callCount = 0;
        callCount++;
        
        if (callCount == 1) {
            std::cout << "\n[VulkanHook] ========================================\n";
            std::cout << "[VulkanHook] *** vkGetInstanceProcAddr HOOKED! ***\n";
            std::cout << "[VulkanHook] ========================================\n";
            
            // Capture instance
            if (instance) {
                g_Instance = instance;
                std::cout << "[VulkanHook] VkInstance captured: 0x" << std::hex << (uintptr_t)g_Instance << std::dec << "\n";
            }
        }
        
        if (pName) {
            std::cout << "[VulkanHook] vkGetInstanceProcAddr: " << pName << "\n";
        }
        
        // Intercept instance-level function requests
        if (pName && strcmp(pName, "vkCreateDevice") == 0) {
            std::cout << "[VulkanHook] *** S&box requested vkCreateDevice via vkGetInstanceProcAddr ***\n";
            return (PFN_vkVoidFunction)hkCreateDevice;
        }
        
        if (pName && strcmp(pName, "vkGetDeviceProcAddr") == 0) {
            std::cout << "[VulkanHook] *** S&box requested vkGetDeviceProcAddr via vkGetInstanceProcAddr ***\n";
            return (PFN_vkVoidFunction)hkGetDeviceProcAddr;
        }
        
        return o_vkGetInstanceProcAddr(instance, pName);
    }
    
    VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
        
        std::cout << "[VulkanHook] ========================================\n";
        std::cout << "[VulkanHook] *** vkCreateDevice called! ***\n";
        std::cout << "[VulkanHook] This is where S&box creates its Vulkan device\n";
        std::cout << "[VulkanHook] ========================================\n";
        
        // Capture physical device
        g_PhysicalDevice = physicalDevice;
        
        // Call original
        VkResult result = o_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        
        if (result == VK_SUCCESS) {
            g_Device = *pDevice;
            std::cout << "[VulkanHook] Device created successfully: 0x" << std::hex << (uintptr_t)g_Device << std::dec << "\n";
            std::cout << "[VulkanHook] Physical device: 0x" << std::hex << (uintptr_t)g_PhysicalDevice << std::dec << "\n";
            
            // Try to find queue family index
            if (pCreateInfo->queueCreateInfoCount > 0) {
                g_QueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
                std::cout << "[VulkanHook] Queue family index: " << g_QueueFamily << "\n";
            }
        }
        
        return result;
    }
    
    void SetupImGui() {
        std::cout << "[VulkanHook] ========================================\n";
        std::cout << "[VulkanHook] Setting up ImGui...\n";
        std::cout << "[VulkanHook] ========================================\n";
        
        if (!g_Hwnd) {
            std::cerr << "[VulkanHook] No window handle - cannot initialize ImGui!\n";
            return;
        }
        
        // Verify we have all required Vulkan objects
        if (!g_Instance || !g_PhysicalDevice || !g_Device || !g_Queue) {
            std::cerr << "[VulkanHook] Missing Vulkan objects - cannot initialize ImGui yet!\n";
            std::cerr << "[VulkanHook] Instance: " << (g_Instance ? "OK" : "MISSING") << "\n";
            std::cerr << "[VulkanHook] PhysicalDevice: " << (g_PhysicalDevice ? "OK" : "MISSING") << "\n";
            std::cerr << "[VulkanHook] Device: " << (g_Device ? "OK" : "MISSING") << "\n";
            std::cerr << "[VulkanHook] Queue: " << (g_Queue ? "OK" : "MISSING") << "\n";
            return;
        }
        
        // Create ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.MouseDrawCursor = true;  // Let ImGui draw its own cursor
        io.IniFilename = nullptr;  // Disable imgui.ini
        io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
        
        std::cout << "[VulkanHook] ImGui context created with display size: " << g_Width << "x" << g_Height << "\n";
        
        // Initialize Win32 backend
        if (!ImGui_ImplWin32_Init(g_Hwnd)) {
            std::cerr << "[VulkanHook] Failed to initialize ImGui Win32 backend!\n";
            ImGui::DestroyContext();
            return;
        }
        
        std::cout << "[VulkanHook] ImGui Win32 backend initialized\n";
        
        // Create descriptor pool for ImGui
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
        };
        
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        
        if (vkCreateDescriptorPool(g_Device, &pool_info, nullptr, &g_DescriptorPool) != VK_SUCCESS) {
            std::cerr << "[VulkanHook] Failed to create descriptor pool!\n";
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        
        std::cout << "[VulkanHook] Descriptor pool created\n";
        
        // Create a simple render pass for ImGui
        VkAttachmentDescription attachment = {};
        attachment.format = VK_FORMAT_B8G8R8A8_UNORM; // Common swapchain format
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // Like working example
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Game already transitioned to present
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        
        VkAttachmentReference color_attachment = {};
        color_attachment.attachment = 0;
        color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment;
        
        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        if (vkCreateRenderPass(g_Device, &renderPassInfo, nullptr, &g_RenderPass) != VK_SUCCESS) {
            std::cerr << "[VulkanHook] Failed to create render pass!\n";
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        
        std::cout << "[VulkanHook] Render pass created\n";
        
        // Ensure we have image count (should be set by vkCreateSwapchainKHR)
        if (g_ImageCount == 0) {
            std::cerr << "[VulkanHook] WARNING: Image count is 0, using MinImageCount\n";
            g_ImageCount = g_MinImageCount > 0 ? g_MinImageCount : 2;
        }
        
        // Create image views and framebuffers for swapchain images
        if (!g_SwapchainImages.empty() && g_SwapchainImageViews.empty()) {
            g_SwapchainImageViews.resize(g_ImageCount);
            g_Framebuffers.resize(g_ImageCount);
            
            for (uint32_t i = 0; i < g_ImageCount; i++) {
                // Create image view
                VkImageViewCreateInfo viewInfo = {};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = g_SwapchainImages[i];
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
                viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
                viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
                viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
                viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 1;
                
                if (vkCreateImageView(g_Device, &viewInfo, nullptr, &g_SwapchainImageViews[i]) != VK_SUCCESS) {
                    std::cerr << "[VulkanHook] Failed to create image view " << i << "\n";
                    continue;
                }
                
                // Create framebuffer
                VkFramebufferCreateInfo fbInfo = {};
                fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                fbInfo.renderPass = g_RenderPass;
                fbInfo.attachmentCount = 1;
                fbInfo.pAttachments = &g_SwapchainImageViews[i];
                fbInfo.width = g_Width;
                fbInfo.height = g_Height;
                fbInfo.layers = 1;
                
                if (vkCreateFramebuffer(g_Device, &fbInfo, nullptr, &g_Framebuffers[i]) != VK_SUCCESS) {
                    std::cerr << "[VulkanHook] Failed to create framebuffer " << i << "\n";
                }
            }
            
            std::cout << "[VulkanHook] Created " << g_ImageCount << " image views and framebuffers\n";
        }
        
        std::cout << "[VulkanHook] Using ImageCount=" << g_ImageCount << ", MinImageCount=" << g_MinImageCount << "\n";
        
        // Initialize ImGui Vulkan backend (new API)
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.ApiVersion = VK_API_VERSION_1_0;
        init_info.Instance = g_Instance;
        init_info.PhysicalDevice = g_PhysicalDevice;
        init_info.Device = g_Device;
        init_info.QueueFamily = g_QueueFamily;
        init_info.Queue = g_Queue;
        init_info.PipelineCache = g_PipelineCache;
        init_info.DescriptorPool = g_DescriptorPool;
        init_info.MinImageCount = g_MinImageCount;
        init_info.ImageCount = g_ImageCount;
        init_info.Allocator = nullptr;
        init_info.CheckVkResultFn = nullptr;
        init_info.PipelineInfoMain.RenderPass = g_RenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        
        if (!ImGui_ImplVulkan_Init(&init_info)) {
            std::cerr << "[VulkanHook] Failed to initialize ImGui Vulkan backend!\n";
            vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        
        std::cout << "[VulkanHook] ImGui Vulkan backend initialized\n";
        std::cout << "[VulkanHook] Fonts will be uploaded automatically on first frame\n";
        
        // Create persistent command pool for rendering
        VkCommandPoolCreateInfo cmdPoolInfo = {};
        cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmdPoolInfo.queueFamilyIndex = g_QueueFamily;
        cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        
        if (vkCreateCommandPool(g_Device, &cmdPoolInfo, nullptr, &g_CommandPool) != VK_SUCCESS) {
            std::cerr << "[VulkanHook] Failed to create render command pool!\n";
            ImGui_ImplVulkan_Shutdown();
            vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        
        std::cout << "[VulkanHook] Render command pool created\n";
        
        // Allocate command buffers (one per swapchain image)
        g_CommandBuffers.resize(g_ImageCount);
        VkCommandBufferAllocateInfo cmdBufAllocInfo = {};
        cmdBufAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufAllocInfo.commandPool = g_CommandPool;
        cmdBufAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufAllocInfo.commandBufferCount = g_ImageCount;
        
        if (vkAllocateCommandBuffers(g_Device, &cmdBufAllocInfo, g_CommandBuffers.data()) != VK_SUCCESS) {
            std::cerr << "[VulkanHook] Failed to allocate command buffers!\n";
            vkDestroyCommandPool(g_Device, g_CommandPool, nullptr);
            ImGui_ImplVulkan_Shutdown();
            vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return;
        }
        
        // Create fences for synchronization (one per swapchain image)
        g_Fences.resize(g_ImageCount);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled
        
        for (uint32_t i = 0; i < g_ImageCount; i++) {
            if (vkCreateFence(g_Device, &fenceInfo, nullptr, &g_Fences[i]) != VK_SUCCESS) {
                std::cerr << "[VulkanHook] Failed to create fence " << i << "!\n";
            }
        }
        
        std::cout << "[VulkanHook] Allocated " << g_ImageCount << " command buffers\n";
        
        // Initialize GUI system
        GUI::Initialize();
        
        std::cout << "[VulkanHook] ========================================\n";
        std::cout << "[VulkanHook] ImGui setup complete!\n";
        std::cout << "[VulkanHook] ========================================\n";
        
        g_ImGuiInitialized = true;
    }
    
    void RenderImGui(uint32_t imageIndex) {
        if (!g_ImGuiInitialized || g_CommandBuffers.empty() || g_Framebuffers.empty()) return;
        if (imageIndex >= g_CommandBuffers.size() || imageIndex >= g_Framebuffers.size()) {
            std::cerr << "[VulkanHook] Invalid image index: " << imageIndex << "\n";
            return;
        }
        
        static int frameCount = 0;
        static bool debugLogged = false;
        
        // ====================================================================
        // COMPLETE MOUSE INPUT OVERRIDE - Don't let Win32 backend touch mouse!
        // ====================================================================
        
        ImGuiIO& io = ImGui::GetIO();
        
        // CRITICAL: Disable Win32 backend from updating mouse (it calls broken ScreenToClient)
        // We'll handle ALL mouse input ourselves
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
        io.BackendFlags &= ~ImGuiBackendFlags_HasSetMousePos;
        
        // Start backend frames
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        
        // Re-enable flags after Win32 backend finishes (so we can still use cursors)
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        
        // Force correct display size
        io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
        
        // ====================================================================
        // MANUAL MOUSE INPUT - Direct coordinates, no window transformation
        // ====================================================================
        
        POINT mousePos;
        if (GetCursorPos(&mousePos)) {
            // For fullscreen Vulkan: GetCursorPos returns screen coords which ARE the window coords
            io.AddMousePosEvent((float)mousePos.x, (float)mousePos.y);
            
            // Debug output
            static int mouseDeb = 0;
            if (GUI::g_MenuOpen && mouseDeb++ < 5) {
                std::cout << "[MOUSE] Cursor=" << mousePos.x << "," << mousePos.y 
                          << " WantCapture=" << io.WantCaptureMouse << "\n";
            }
        }
        
        // Manual mouse buttons
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        
        // BACKUP: Check INSERT key here too (in case WndProc misses it)
        static bool insertKeyPrevious = false;
        static int insertCheckCount = 0;
        bool insertKeyNow = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        
        // DEBUG: Log INSERT state changes
        if (insertCheckCount++ % 300 == 0) { // Every 5 seconds at 60fps
            std::cout << "[GetAsyncKeyState] INSERT check - Current state: " << (insertKeyNow ? "DOWN" : "UP") << "\n";
        }
        
        if (insertKeyNow && !insertKeyPrevious) {
            GUI::g_MenuOpen = !GUI::g_MenuOpen;
            std::cout << "[RenderImGui] ******** BACKUP INSERT DETECTED! ******** Menu: " << (GUI::g_MenuOpen ? "OPEN" : "CLOSED") << "\n";
        }
        insertKeyPrevious = insertKeyNow;
        insertKeyPrevious = insertKeyNow;
        
        ImGui::NewFrame();
        
        // Debug: Check if ImGui wants mouse
        static int debugCounter = 0;
        if (GUI::g_MenuOpen && debugCounter++ < 10) {
            std::cout << "[RenderImGui] Frame " << debugCounter 
                      << " WantCaptureMouse=" << io.WantCaptureMouse
                      << " MousePos=(" << io.MousePos.x << "," << io.MousePos.y << ")\n";
        }
        
        // Render GUI
        GUI::Render();
        
        // Render ESP (always drawn, not just when menu is open)
        Features::g_ESP.Render();
        
        ImGui::EndFrame();
        ImGui::Render();
        
        // Get draw data
        ImDrawData* draw_data = ImGui::GetDrawData();
        
        // Debug: Log first few frames
        if (frameCount < 5) {
            frameCount++;
            std::cout << "[VulkanHook] RenderImGui frame #" << frameCount 
                      << " - MenuOpen=" << (GUI::g_MenuOpen ? "YES" : "NO")
                      << " - DrawData=" << (draw_data ? "YES" : "NULL")
                      << " - TotalVtx=" << (draw_data ? draw_data->TotalVtxCount : 0)
                      << " - ImageIndex=" << imageIndex
                      << "\n";
        }
        
        if (draw_data && draw_data->TotalVtxCount > 0) {
            VkCommandBuffer cmd = g_CommandBuffers[imageIndex];
            VkFence fence = g_Fences[imageIndex];
            
            // Wait for fence from previous frame
            // Wait for fence with 100ms timeout to prevent lag
            VkResult waitResult = vkWaitForFences(g_Device, 1, &fence, VK_TRUE, 100000000); // 100ms in nanoseconds
            if (waitResult == VK_TIMEOUT) {
                // Timeout - skip this frame to prevent lag
                vkDestroyFence(g_Device, fence, nullptr);
                return;
            }
            vkResetFences(g_Device, 1, &fence);
            
            // Reset and begin command buffer
            vkResetCommandBuffer(cmd, 0);
            
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &beginInfo);
            
            // Begin render pass
            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = g_RenderPass;
            renderPassInfo.framebuffer = g_Framebuffers[imageIndex];
            renderPassInfo.renderArea.offset = {0, 0};
            renderPassInfo.renderArea.extent = {(uint32_t)g_Width, (uint32_t)g_Height};
            renderPassInfo.clearValueCount = 0;
            renderPassInfo.pClearValues = nullptr;
            
            vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);
            
            // Submit with fence
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmd;
            
            vkQueueSubmit(g_Queue, 1, &submitInfo, fence);
        }
    }
    
    void CleanupImGui() {
        if (!g_ImGuiInitialized) return;
        
        std::cout << "[VulkanHook] Cleaning up ImGui...\n";
        
        // Wait for device to finish
        if (g_Device) {
            vkDeviceWaitIdle(g_Device);
        }
        
        // Destroy fences
        for (auto fence : g_Fences) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(g_Device, fence, nullptr);
            }
        }
        g_Fences.clear();
        
        // Free command buffers
        if (g_CommandPool && !g_CommandBuffers.empty()) {
            vkFreeCommandBuffers(g_Device, g_CommandPool, (uint32_t)g_CommandBuffers.size(), g_CommandBuffers.data());
            g_CommandBuffers.clear();
        }
        
        // Destroy command pool
        if (g_CommandPool) {
            vkDestroyCommandPool(g_Device, g_CommandPool, nullptr);
            g_CommandPool = VK_NULL_HANDLE;
        }
        
        // Destroy framebuffers
        for (auto fb : g_Framebuffers) {
            if (fb) vkDestroyFramebuffer(g_Device, fb, nullptr);
        }
        g_Framebuffers.clear();
        
        // Destroy image views
        for (auto view : g_SwapchainImageViews) {
            if (view) vkDestroyImageView(g_Device, view, nullptr);
        }
        g_SwapchainImageViews.clear();
        g_SwapchainImages.clear();
        
        // Shutdown ImGui backends
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        
        // Destroy Vulkan resources
        if (g_RenderPass && g_Device) {
            vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
            g_RenderPass = VK_NULL_HANDLE;
        }
        
        if (g_DescriptorPool && g_Device) {
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            g_DescriptorPool = VK_NULL_HANDLE;
        }
        
        ImGui::DestroyContext();
        
        g_ImGuiInitialized = false;
    }
}
