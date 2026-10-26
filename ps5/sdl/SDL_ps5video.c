/*
 * James Pond 2 for the PlayStation 5 - SDL3's video driver for the console.
 *
 * The console has one output and no windows: this gives SDL one display and
 * lets it keep its window bookkeeping (size, flags, events) for a window that
 * is the whole screen. Its Vulkan calls answer for the console: the "library"
 * is RADV, linked into the title, and a window's surface is VK_KHR_display's
 * first display in its first mode. SDL's GPU API (geow2's renderer) draws
 * through that.
 *
 * The display's size is the console's output (3840x2160 on a 4K television,
 * 1920x1080 otherwise), asked of VK_KHR_display at start-up, so SDL's idea of
 * the window is the swapchain's. SDL_PS5_DISPLAY=WIDTHxHEIGHT in the
 * environment overrides it. Its refresh is the output's once the swap chain
 * has chosen 120 or 60 Hz.
 *
 * SPDX-License-Identifier: Zlib
 */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PS5

#include "video/SDL_sysvideo.h"
#include "events/SDL_events_c.h"
#include "events/SDL_displayevents_c.h"

#include "platform.h"

#ifdef SDL_VIDEO_VULKAN
#include "video/SDL_vulkan_internal.h"

/* RADV's entry point: there is no Vulkan loader, the driver is in the title */
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

static bool PS5_Vulkan_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    _this->vulkan_config.vkGetInstanceProcAddr = (void *)vk_icdGetInstanceProcAddr;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties =
        (void *)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!_this->vulkan_config.vkEnumerateInstanceExtensionProperties) {
        return SDL_SetError("RADV has no vkEnumerateInstanceExtensionProperties");
    }
    SDL_strlcpy(_this->vulkan_config.loader_path, "radv", SDL_arraysize(_this->vulkan_config.loader_path));
    return true;
}

static void PS5_Vulkan_UnloadLibrary(SDL_VideoDevice *_this)
{
}

static char const *const *PS5_Vulkan_GetInstanceExtensions(SDL_VideoDevice *_this, Uint32 *count)
{
    static const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    if (count) {
        *count = SDL_arraysize(extensions);
    }
    return extensions;
}

static bool PS5_Vulkan_CreateSurface(SDL_VideoDevice *_this, SDL_Window *window, VkInstance instance,
                                     const struct VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    return SDL_Vulkan_Display_CreateSurface(_this->vulkan_config.vkGetInstanceProcAddr, instance, allocator, surface);
}

static void PS5_Vulkan_DestroySurface(SDL_VideoDevice *_this, VkInstance instance, VkSurfaceKHR surface,
                                      const struct VkAllocationCallbacks *allocator)
{
    SDL_Vulkan_DestroySurface_Internal(_this->vulkan_config.vkGetInstanceProcAddr, instance, surface, allocator);
}
#endif // SDL_VIDEO_VULKAN

/* The console's output, as VK_KHR_display reports its first display */
static bool PS5_OutputSize(int *w, int *h)
{
#ifdef SDL_VIDEO_VULKAN
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    const char *const extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    VkApplicationInfo application = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = SDL_arraysize(extensions),
        .ppEnabledExtensionNames = extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    bool found = false;

    if (!create || create(&info, NULL, &instance) != VK_SUCCESS) {
        return false;
    }
#define PS5_VK(name) PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)
    PS5_VK(vkEnumeratePhysicalDevices);
    PS5_VK(vkGetPhysicalDeviceDisplayPropertiesKHR);
    PS5_VK(vkDestroyInstance);
#undef PS5_VK
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t count = 1;
    if (vkEnumeratePhysicalDevices && vkGetPhysicalDeviceDisplayPropertiesKHR &&
        vkEnumeratePhysicalDevices(instance, &count, &device) >= VK_SUCCESS && count == 1) {
        VkDisplayPropertiesKHR display;
        count = 1;
        if (vkGetPhysicalDeviceDisplayPropertiesKHR(device, &count, &display) >= VK_SUCCESS && count == 1 &&
            display.physicalResolution.width && display.physicalResolution.height) {
            *w = (int)display.physicalResolution.width;
            *h = (int)display.physicalResolution.height;
            found = true;
        }
    }
    if (vkDestroyInstance) {
        vkDestroyInstance(instance, NULL);
    }
    return found;
#else
    return false;
#endif
}

static bool PS5_VideoInit(SDL_VideoDevice *_this)
{
    SDL_DisplayMode mode;
    SDL_DisplayID display;
    int w = 1920, h = 1080;
    const char *size = SDL_getenv("SDL_PS5_DISPLAY");

    if (size && SDL_sscanf(size, "%dx%d", &w, &h) == 2) {
        say("video: %dx%d from SDL_PS5_DISPLAY", w, h);
    } else if (PS5_OutputSize(&w, &h)) {
        say("video: the console's output is %dx%d", w, h);
    } else {
        w = 1920;
        h = 1080;
        say("video: the console's output size is unknown, taking %dx%d", w, h);
    }
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_XRGB8888;
    mode.w = w;
    mode.h = h;
    mode.refresh_rate = 60.0f;
    display = SDL_AddBasicVideoDisplay(&mode);
    if (display == 0) {
        return false;
    }
    /* A TV seen from across the room: sizes as a 1080p screen shows them */
    SDL_SetDisplayContentScale(SDL_GetVideoDisplay(display), (float)h / 1080.0f);
    return true;
}

static void PS5_VideoQuit(SDL_VideoDevice *_this)
{
}

/* The output's refresh once the swap chain has chosen it (ps5/patches/radv.patch:
 * 119880 on a TV that runs 120 Hz, else 59940; 0 before), as the display
 * mode's: SDL's event for it tells the title, which locks its steps to it */
extern int ps5_output_refresh_millihz;

static void PS5_PumpEvents(SDL_VideoDevice *_this)
{
    static int reported;
    const int millihz = ps5_output_refresh_millihz;

    if (millihz > 0 && millihz != reported && _this->num_displays > 0) {
        SDL_VideoDisplay *display = _this->displays[0];
        SDL_DisplayMode *modes[2] = { &display->desktop_mode, (SDL_DisplayMode *)display->current_mode };

        /* In place: SDL keeps a pointer to the current mode, and its desktop
         * setter leaves a fullscreen display alone */
        reported = millihz;
        for (int i = 0; i < 2; i++) {
            if (modes[i] && (i == 0 || modes[i] != modes[0])) {
                modes[i]->refresh_rate_numerator = millihz;
                modes[i]->refresh_rate_denominator = 1000;
                modes[i]->refresh_rate = SDL_roundf((float)millihz / 10.0f) / 100.0f;
            }
        }
        SDL_SendDisplayEvent(display, SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED, display->desktop_mode.w,
                             display->desktop_mode.h);
        say("video: the output refreshes at %.2f Hz", millihz / 1000.0);
    }
}

static bool PS5_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props)
{
    /* The window is the screen, and always has the pad's focus */
    SDL_SetKeyboardFocus(window);
    return true;
}

static bool PS5_SetWindowPosition(SDL_VideoDevice *_this, SDL_Window *window)
{
    return true;
}

static void PS5_SetWindowSize(SDL_VideoDevice *_this, SDL_Window *window)
{
    /* The size stays the display's */
}

static SDL_FullscreenResult PS5_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
    return SDL_FULLSCREEN_SUCCEEDED;
}

static void PS5_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *PS5_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->VideoInit = PS5_VideoInit;
    device->VideoQuit = PS5_VideoQuit;
    device->PumpEvents = PS5_PumpEvents;
    device->CreateSDLWindow = PS5_CreateWindow;
    device->SetWindowPosition = PS5_SetWindowPosition;
    device->SetWindowSize = PS5_SetWindowSize;
    device->SetWindowFullscreen = PS5_SetWindowFullscreen;
#ifdef SDL_VIDEO_VULKAN
    device->Vulkan_LoadLibrary = PS5_Vulkan_LoadLibrary;
    device->Vulkan_UnloadLibrary = PS5_Vulkan_UnloadLibrary;
    device->Vulkan_GetInstanceExtensions = PS5_Vulkan_GetInstanceExtensions;
    device->Vulkan_CreateSurface = PS5_Vulkan_CreateSurface;
    device->Vulkan_DestroySurface = PS5_Vulkan_DestroySurface;
#endif
    device->free = PS5_DeleteDevice;
    device->device_caps = VIDEO_DEVICE_CAPS_FULLSCREEN_ONLY;
    return device;
}

VideoBootStrap PS5_bootstrap = {
    "ps5", "PlayStation 5 video driver",
    PS5_CreateDevice,
    NULL, // no ShowMessageBox implementation
    false
};

#endif // SDL_VIDEO_DRIVER_PS5
