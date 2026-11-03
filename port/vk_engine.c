/*
 * The Vulkan backend's window and swapchain: the part that has nothing to do
 * with the title's drawing.
 *
 * OpenGL keeps all of this inside nv2a_gl.c -- it makes the SDL window with
 * SDL_WINDOW_OPENGL, owns the GL context, runs a presenter thread that copies
 * the frame and swaps. Vulkan cannot share any of it: the window has to be
 * made with SDL_WINDOW_VULKAN, the instance is Turnip's (libadrenotools),
 * and the swapchain is ours. So this file owns the window, the instance, the
 * surface, the device and the swapchain, and nv2a_gl.c asks it for a frame.
 *
 * The first milestone is a present that clears: the engine boots, the title
 * runs, and the screen shows a colour rather than the picture. Drawing comes
 * after, through nv2a_vk_gfx.c's table.
 *
 * What this file knows that was expensive to learn (docs/android-vulkan.md):
 * one SDL window on Android and it is the one the app was given; the loader
 * must be Turnip's, so no SDL_Vulkan_LoadLibrary and no system libvulkan; and
 * VK_USE_PLATFORM_ANDROID_KHR has to precede vulkan.h.
 */
#if defined(__ANDROID__)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <android/native_window.h>

#include <adrenotools/driver.h>

#include "../src/nv2a_gl/jsrf_debug_env.h"

#define VKE_LIB "libvulkan_freedreno.so"

static void vke(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    jsrf_dbg_log("[VKE] %s\n", line);
}

int nv2a_vk_engine_active(void);
void nv2a_vk_engine_frame(void);
void nv2a_vk_engine_stop(void);

/* ---- state ------------------------------------------------------------- */

static SDL_Window      *w_win;
static ANativeWindow   *w_native;
static VkInstance       w_inst;
static VkSurfaceKHR     w_surf;
static VkPhysicalDevice w_phys;
static VkDevice         w_dev;
static VkQueue          w_queue;
static VkSwapchainKHR   w_sc;
static VkCommandPool    w_pool;
static VkCommandBuffer  w_cmd;
static VkSemaphore      w_acq, w_done;
static VkFence          w_fence;
static VkImage          w_img[8];
static VkImageLayout    w_layout[8];
static uint32_t         w_nimg;
static VkExtent2D       w_extent;
static int              w_active;

static PFN_vkGetInstanceProcAddr p_gipa;
static PFN_vkDestroyInstance              p_destroy_instance;
static PFN_vkGetDeviceQueue               p_get_queue;
static PFN_vkDeviceWaitIdle               p_device_idle;
static PFN_vkDestroySurfaceKHR            p_destroy_surface;
static PFN_vkDestroySwapchainKHR          p_destroy_swapchain;
static PFN_vkGetSwapchainImagesKHR        p_sc_images;
static PFN_vkAcquireNextImageKHR          p_acquire;
static PFN_vkQueuePresentKHR              p_present;
static PFN_vkCreateSemaphore              p_sem_new;
static PFN_vkDestroySemaphore             p_sem_free;
static PFN_vkCreateFence                  p_fence_new;
static PFN_vkDestroyFence                 p_fence_free;
static PFN_vkWaitForFences                p_wait;
static PFN_vkResetFences                  p_reset;
static PFN_vkDestroyCommandPool           p_pool_free;
static PFN_vkBeginCommandBuffer           p_begin;
static PFN_vkEndCommandBuffer             p_end;
static PFN_vkCmdPipelineBarrier           p_barrier;
static PFN_vkCmdClearColorImage           p_clear;
static PFN_vkQueueSubmit                  p_submit;
static PFN_vkDestroyDevice                p_destroy_device;
static PFN_vkCreateImageView              p_view_new;
static PFN_vkDestroyImageView             p_view_free;
static PFN_vkCreateShaderModule           p_shader_new;
static PFN_vkDestroyShaderModule          p_shader_free;
static PFN_vkCreatePipelineLayout         p_layout_new;
static PFN_vkDestroyPipelineLayout        p_layout_free;
static PFN_vkCreateGraphicsPipelines      p_pipe_new;
static PFN_vkDestroyPipeline              p_pipe_free;
static PFN_vkCmdBeginRendering            p_begin_rendering;
static PFN_vkCmdEndRendering              p_end_rendering;
static PFN_vkCmdBindPipeline              p_bind_pipeline;
static PFN_vkCmdDraw                      p_cmd_draw;
static PFN_vkCmdSetViewport               p_viewport;
static PFN_vkCmdSetScissor                p_scissor;
static int vke_pipeline(void);
static int SDLCALL vke_present_main(void *arg);
static void vke_frame_body(void);

/* What the first pipeline needs: a view per swapchain image, the two shader
 * modules glslang compiled, and the pipeline itself. Dynamic rendering takes
 * no render pass, which is why this is so little for Vulkan 1.3. */
static VkImageView      w_view[8];
static VkShaderModule   w_vs, w_fs;
static VkPipelineLayout w_pl;
static VkPipeline       w_pipe;
static int              w_draw_ready;

/* The frame belongs to a thread of its own, as the GL backend's does: the
 * pushbuffer thread asks for a frame and moves on. Anything else means the
 * window's thread and the renderer's are both inside Vulkan. */
static SDL_Thread      *w_thread;
static SDL_mutex       *w_mu;
static SDL_cond        *w_cv;
static int              w_req, w_quit;

static int vke_switch_on(const char *name)
{
    const char *v = jsrf_getenv(name);
    if (!v || !*v) return 0;
    return !(v[0] == '0' && !v[1]);
}

static int vke_native_dir(char *out, size_t n)
{
    Dl_info info;
    char *slash;
    if (!dladdr((void *)vke_native_dir, &info) || !info.dli_fname) return 0;
    snprintf(out, n, "%s", info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash) return 0;
    slash[1] = '\0';
    return 1;
}

/* ---- start ------------------------------------------------------------- */

int nv2a_vk_engine_start(int want_w, int want_h)
{
    char lib_dir[512];
    void *handle;
    PFN_vkCreateInstance p_create_instance;
    PFN_vkEnumeratePhysicalDevices p_enum;
    PFN_vkGetPhysicalDeviceProperties p_props;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties p_qf;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_support;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_caps;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_formats;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR p_modes;
    PFN_vkCreateDevice p_create_device;
    PFN_vkCreateSwapchainKHR p_sc_new;
    PFN_vkCreateCommandPool p_pool_new;
    PFN_vkAllocateCommandBuffers p_cmds;
    uint32_t family = UINT32_MAX, i, n;
    VkSurfaceFormatKHR fmt;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkResult r;
    int ok = 0;

    (void)want_w;
    (void)want_h;

    {   extern int nv2a_vk_spirv_test(void);
        if (vke_switch_on("RECOMP_VK_SPIRV_TEST")) nv2a_vk_spirv_test(); }
    if (!vke_native_dir(lib_dir, sizeof lib_dir)) { vke("no native dir"); return 0; }
    handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                        NULL, lib_dir, lib_dir, VKE_LIB, NULL, NULL);
    if (!handle) { vke("no %s in %s", VKE_LIB, lib_dir); return 0; }
    p_gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!p_gipa) { vke("no vkGetInstanceProcAddr"); return 0; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0 && SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        vke("SDL video would not start: %s", SDL_GetError());
        return 0;
    }
    w_win = SDL_CreateWindow("JSRF Unleashed", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, 0, 0,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP
                           | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w_win) { vke("SDL_CreateWindow(VULKAN): %s", SDL_GetError()); return 0; }
    {   SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (!SDL_GetWindowWMInfo(w_win, &wmi) || wmi.subsystem != SDL_SYSWM_ANDROID) {
            vke("no WM info: %s", SDL_GetError());
            goto fail;
        }
        w_native = wmi.info.android.window;
    }
    if (!w_native) { vke("SDL has no native window"); goto fail; }

    {   const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        VkApplicationInfo app;
        VkInstanceCreateInfo ici;
        memset(&app, 0, sizeof app);
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "JSRF Unleashed";
        app.apiVersion = VK_API_VERSION_1_3;
        memset(&ici, 0, sizeof ici);
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;
        p_create_instance = (PFN_vkCreateInstance)p_gipa(NULL, "vkCreateInstance");
        if (!p_create_instance) { vke("no vkCreateInstance"); goto fail; }
        r = p_create_instance(&ici, NULL, &w_inst);
        if (r != VK_SUCCESS) { vke("vkCreateInstance -> %d", (int)r); goto fail; }
    }
    p_destroy_instance = (PFN_vkDestroyInstance)p_gipa(w_inst, "vkDestroyInstance");
    p_enum = (PFN_vkEnumeratePhysicalDevices)p_gipa(w_inst, "vkEnumeratePhysicalDevices");
    p_props = (PFN_vkGetPhysicalDeviceProperties)p_gipa(w_inst, "vkGetPhysicalDeviceProperties");
    p_qf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)p_gipa(w_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    p_support = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)p_gipa(w_inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    p_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)p_gipa(w_inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    p_formats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)p_gipa(w_inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    p_modes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)p_gipa(w_inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    p_destroy_surface = (PFN_vkDestroySurfaceKHR)p_gipa(w_inst, "vkDestroySurfaceKHR");
    {   PFN_vkCreateAndroidSurfaceKHR p_as =
            (PFN_vkCreateAndroidSurfaceKHR)p_gipa(w_inst, "vkCreateAndroidSurfaceKHR");
        VkAndroidSurfaceCreateInfoKHR aci;
        if (!p_as) { vke("no vkCreateAndroidSurfaceKHR"); goto fail; }
        memset(&aci, 0, sizeof aci);
        aci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        aci.window = w_native;
        r = p_as(w_inst, &aci, NULL, &w_surf);
        if (r != VK_SUCCESS) { vke("vkCreateAndroidSurfaceKHR -> %d", (int)r); goto fail; }
    }
    n = 0;
    if (!p_enum || p_enum(w_inst, &n, NULL) != VK_SUCCESS || !n) { vke("no device"); goto fail; }
    if (n > 4) n = 4;
    {   VkPhysicalDevice devs[4];
        p_enum(w_inst, &n, devs);
        w_phys = devs[0];
        {   VkPhysicalDeviceProperties pr;
            p_props(w_phys, &pr);
            vke("device %s (%u.%u.%u)", pr.deviceName,
                (unsigned)VK_VERSION_MAJOR(pr.apiVersion),
                (unsigned)VK_VERSION_MINOR(pr.apiVersion),
                (unsigned)VK_VERSION_PATCH(pr.apiVersion)); } }
    {   uint32_t nq = 0, k;
        VkQueueFamilyProperties qp[8];
        p_qf(w_phys, &nq, NULL);
        if (nq > 8) nq = 8;
        p_qf(w_phys, &nq, qp);
        for (k = 0; k < nq; k++) {
            VkBool32 sup = VK_FALSE;
            if (!(qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            p_support(w_phys, k, w_surf, &sup);
            if (sup) { family = k; break; }
        } }
    if (family == UINT32_MAX) { vke("no graphics+present queue"); goto fail; }
    p_create_device = (PFN_vkCreateDevice)p_gipa(w_inst, "vkCreateDevice");
    p_destroy_device = (PFN_vkDestroyDevice)p_gipa(w_inst, "vkDestroyDevice");
    p_get_queue = (PFN_vkGetDeviceQueue)p_gipa(w_inst, "vkGetDeviceQueue");
    p_device_idle = (PFN_vkDeviceWaitIdle)p_gipa(w_inst, "vkDeviceWaitIdle");
    {   VkDeviceCreateInfo dci;
        VkDeviceQueueCreateInfo qci;
        const char *dev_ext[1];
        float prio = 1.0f;
        dev_ext[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        memset(&qci, 0, sizeof qci);
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        {   /* Dynamic rendering and synchronized submission are core in 1.3;
             * without the feature enabled vkCmdBeginRendering is not legal. */
            static VkPhysicalDeviceVulkan13Features f13;
            memset(&f13, 0, sizeof f13);
            f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            f13.dynamicRendering = VK_TRUE;
            f13.synchronization2 = VK_TRUE;
            /* RECOMP_VK_NO_F13=1: create the device as the demo did, with no
             * 1.3 features asked for. It is the last thing this path does
             * that the demo, which presented 600 frames, did not. */
            if (!vke_switch_on("RECOMP_VK_NO_F13")) dci.pNext = &f13;
            dci.enabledExtensionCount = 1;
            dci.ppEnabledExtensionNames = dev_ext;
            r = p_create_device(w_phys, &dci, NULL, &w_dev);
        }
        if (r != VK_SUCCESS) { vke("vkCreateDevice -> %d", (int)r); goto fail; }
    }
    p_get_queue(w_dev, family, 0, &w_queue);
    p_sc_new = (PFN_vkCreateSwapchainKHR)p_gipa(w_inst, "vkCreateSwapchainKHR");
    p_destroy_swapchain = (PFN_vkDestroySwapchainKHR)p_gipa(w_inst, "vkDestroySwapchainKHR");
    p_sc_images = (PFN_vkGetSwapchainImagesKHR)p_gipa(w_inst, "vkGetSwapchainImagesKHR");
    p_acquire = (PFN_vkAcquireNextImageKHR)p_gipa(w_inst, "vkAcquireNextImageKHR");
    p_present = (PFN_vkQueuePresentKHR)p_gipa(w_inst, "vkQueuePresentKHR");
    p_sem_new = (PFN_vkCreateSemaphore)p_gipa(w_inst, "vkCreateSemaphore");
    p_sem_free = (PFN_vkDestroySemaphore)p_gipa(w_inst, "vkDestroySemaphore");
    p_fence_new = (PFN_vkCreateFence)p_gipa(w_inst, "vkCreateFence");
    p_fence_free = (PFN_vkDestroyFence)p_gipa(w_inst, "vkDestroyFence");
    p_wait = (PFN_vkWaitForFences)p_gipa(w_inst, "vkWaitForFences");
    p_reset = (PFN_vkResetFences)p_gipa(w_inst, "vkResetFences");
    p_pool_new = (PFN_vkCreateCommandPool)p_gipa(w_inst, "vkCreateCommandPool");
    p_pool_free = (PFN_vkDestroyCommandPool)p_gipa(w_inst, "vkDestroyCommandPool");
    p_cmds = (PFN_vkAllocateCommandBuffers)p_gipa(w_inst, "vkAllocateCommandBuffers");
    p_begin = (PFN_vkBeginCommandBuffer)p_gipa(w_inst, "vkBeginCommandBuffer");
    p_end = (PFN_vkEndCommandBuffer)p_gipa(w_inst, "vkEndCommandBuffer");
    p_barrier = (PFN_vkCmdPipelineBarrier)p_gipa(w_inst, "vkCmdPipelineBarrier");
    p_clear = (PFN_vkCmdClearColorImage)p_gipa(w_inst, "vkCmdClearColorImage");
    p_submit = (PFN_vkQueueSubmit)p_gipa(w_inst, "vkQueueSubmit");
    p_view_new = (PFN_vkCreateImageView)p_gipa(w_inst, "vkCreateImageView");
    p_view_free = (PFN_vkDestroyImageView)p_gipa(w_inst, "vkDestroyImageView");
    p_shader_new = (PFN_vkCreateShaderModule)p_gipa(w_inst, "vkCreateShaderModule");
    p_shader_free = (PFN_vkDestroyShaderModule)p_gipa(w_inst, "vkDestroyShaderModule");
    p_layout_new = (PFN_vkCreatePipelineLayout)p_gipa(w_inst, "vkCreatePipelineLayout");
    p_layout_free = (PFN_vkDestroyPipelineLayout)p_gipa(w_inst, "vkDestroyPipelineLayout");
    p_pipe_new = (PFN_vkCreateGraphicsPipelines)p_gipa(w_inst, "vkCreateGraphicsPipelines");
    p_pipe_free = (PFN_vkDestroyPipeline)p_gipa(w_inst, "vkDestroyPipeline");
    p_begin_rendering = (PFN_vkCmdBeginRendering)p_gipa(w_inst, "vkCmdBeginRendering");
    p_end_rendering = (PFN_vkCmdEndRendering)p_gipa(w_inst, "vkCmdEndRendering");
    p_bind_pipeline = (PFN_vkCmdBindPipeline)p_gipa(w_inst, "vkCmdBindPipeline");
    p_cmd_draw = (PFN_vkCmdDraw)p_gipa(w_inst, "vkCmdDraw");
    p_viewport = (PFN_vkCmdSetViewport)p_gipa(w_inst, "vkCmdSetViewport");
    p_scissor = (PFN_vkCmdSetScissor)p_gipa(w_inst, "vkCmdSetScissor");
    {   VkSurfaceCapabilitiesKHR caps;
        uint32_t nf = 0, nm = 0;
        VkSurfaceFormatKHR fmts[32];
        VkPresentModeKHR modes[8];
        p_caps(w_phys, w_surf, &caps);
        p_formats(w_phys, w_surf, &nf, NULL);
        if (nf > 32) nf = 32;
        p_formats(w_phys, w_surf, &nf, fmts);
        p_modes(w_phys, w_surf, &nm, NULL);
        if (nm > 8) nm = 8;
        p_modes(w_phys, w_surf, &nm, modes);
        if (!nf) { vke("no surface formats"); goto fail; }
        fmt = fmts[0];
        for (i = 0; i < nf; i++)
            if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM
             || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = fmts[i]; break; }
        for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
        w_extent = caps.currentExtent;
        if (w_extent.width == 0xFFFFFFFFu) {
            w_extent.width = 480; w_extent.height = 320;
            if (w_extent.width > caps.maxImageExtent.width) w_extent.width = caps.maxImageExtent.width;
            if (w_extent.height > caps.maxImageExtent.height) w_extent.height = caps.maxImageExtent.height;
        }
        {   VkSwapchainCreateInfoKHR sci;
            memset(&sci, 0, sizeof sci);
            sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            sci.surface = w_surf;
            sci.minImageCount = caps.minImageCount + 1;
            if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
                sci.minImageCount = caps.maxImageCount;
            sci.imageFormat = fmt.format;
            sci.imageColorSpace = fmt.colorSpace;
            sci.imageExtent = w_extent;
            sci.imageArrayLayers = 1;
            sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = caps.currentTransform;
            sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            sci.presentMode = mode;
            sci.clipped = VK_TRUE;
            r = p_sc_new(w_dev, &sci, NULL, &w_sc);
            if (r != VK_SUCCESS) { vke("vkCreateSwapchainKHR -> %d", (int)r); goto fail; }
        }
    }
    if (p_sc_images(w_dev, w_sc, &w_nimg, NULL) != VK_SUCCESS || !w_nimg) {
        vke("no swapchain images"); goto fail;
    }
    if (w_nimg > 8) w_nimg = 8;
    p_sc_images(w_dev, w_sc, &w_nimg, w_img);
    for (i = 0; i < w_nimg; i++) w_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    vke_pipeline();
    {   VkCommandPoolCreateInfo pci;
        VkCommandBufferAllocateInfo cbi;
        VkFenceCreateInfo fci;
        VkSemaphoreCreateInfo sci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = family;
        if (p_pool_new(w_dev, &pci, NULL, &w_pool) != VK_SUCCESS) { vke("pool"); goto fail; }
        memset(&cbi, 0, sizeof cbi);
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = w_pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        if (p_cmds(w_dev, &cbi, &w_cmd) != VK_SUCCESS) { vke("cmd"); goto fail; }
        memset(&fci, 0, sizeof fci);
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        p_fence_new(w_dev, &fci, NULL, &w_fence);
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        p_sem_new(w_dev, &sci, NULL, &w_acq);
        p_sem_new(w_dev, &sci, NULL, &w_done);
    }
    w_active = 1;
    vke("swapchain %ux%u, format %d, %u images", w_extent.width, w_extent.height,
        (int)fmt.format, w_nimg);
    /* RECOMP_VK_MAINFRAMES=<n>: a few frames here, on the thread that made the
     * window, before the engine ever asks for one. It separates "these calls
     * work from this thread" from "they work from the pushbuffer thread",
     * which is the difference the crash has left. */
    vke("fn acquire %p, begin %p, barrier %p, clear %p, submit %p, wait %p, present %p",
        (void *)p_acquire, (void *)p_begin, (void *)p_barrier, (void *)p_clear,
        (void *)p_submit, (void *)p_wait, (void *)p_present);
    if (!w_thread) {
        w_mu = SDL_CreateMutex();
        w_cv = SDL_CreateCond();
        if (w_mu && w_cv)
            w_thread = SDL_CreateThread(vke_present_main, "nv2a-vulkan-present", NULL);
        if (w_thread) vke("presenter thread started");
    }
    /* The surface may not be attached to the window yet. Give Android a
     * moment before the first acquire: a swapchain whose surface the driver
     * has not been told about would explain a fault inside the driver. */
    usleep(600000);
    {   const char *v = jsrf_getenv("RECOMP_VK_MAINFRAMES");
        int n = v ? atoi(v) : 0, k;
        for (k = 0; k < n; k++) {
            vke("main-thread frame %d", k + 1);
            nv2a_vk_engine_frame();
        }
        if (n) vke("main-thread frames done (%d)", n);
    }
    ok = 1;
    goto done;

fail:
    vke("engine start failed");
done:
    if (!ok) nv2a_vk_engine_stop();
    return ok;
}

/* Views, shaders and the pipeline the first draw needs. The SPIR-V comes from
 * port/vk_shader.c, so this is also the proof that the run-time compiler's
 * output is accepted by the driver. */
static int vke_pipeline(void)
{
    extern int nv2a_vk_glsl_to_spirv(const char *glsl, int stage,
                                     uint32_t **words, size_t *nwords);
    static const char *vs =
        "#version 450\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        "#version 450\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(0.95, 0.55, 0.15, 1.0); }\n";
    uint32_t *wv = NULL, *wf = NULL;
    size_t nv = 0, nf = 0;
    VkShaderModuleCreateInfo smi;
    VkPipelineLayoutCreateInfo pli;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineRenderingCreateInfoKHR ri;
    VkGraphicsPipelineCreateInfo gp;
    uint32_t i;

    /* RECOMP_VK_NO_DRAW=1: keep the clear path. It separates a crash in the
     * dynamic-rendering path from one the engine has regardless. */
    { extern int not_here(void); }
    if (vke_switch_on("RECOMP_VK_NO_DRAW")) return 0;
    if (!p_view_new || !p_shader_new || !p_pipe_new || !p_begin_rendering) return 0;
    for (i = 0; i < w_nimg; i++) {
        VkImageViewCreateInfo vci;
        memset(&vci, 0, sizeof vci);
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = w_img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (p_view_new(w_dev, &vci, NULL, &w_view[i]) != VK_SUCCESS) return 0;
    }
    if (!nv2a_vk_glsl_to_spirv(vs, 0, &wv, &nv) || !nv2a_vk_glsl_to_spirv(fs, 1, &wf, &nf)) {
        vke("the test shaders did not compile");
        free(wv); free(wf);
        return 0;
    }
    memset(&smi, 0, sizeof smi);
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = nv * 4; smi.pCode = wv;
    if (p_shader_new(w_dev, &smi, NULL, &w_vs) != VK_SUCCESS) { vke("vertex module"); return 0; }
    smi.codeSize = nf * 4; smi.pCode = wf;
    if (p_shader_new(w_dev, &smi, NULL, &w_fs) != VK_SUCCESS) { vke("fragment module"); return 0; }
    free(wv); free(wf);

    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (p_layout_new(w_dev, &pli, NULL, &w_pl) != VK_SUCCESS) { vke("layout"); return 0; }

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = w_vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = w_fs;
    stages[1].pName = "main";
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &(VkFormat){ VK_FORMAT_R8G8B8A8_UNORM };
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &ri;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = w_pl;
    if (p_pipe_new(w_dev, VK_NULL_HANDLE, 1, &gp, NULL, &w_pipe) != VK_SUCCESS) {
        vke("graphics pipeline"); return 0;
    }
    w_draw_ready = 1;
    vke("pipeline ready (dynamic rendering, SPIR-V from glslang)");
    return 1;
}

int nv2a_vk_engine_active(void) { return w_active; }

/* The frame belongs to a thread of its own, as the GL backend's does: the
 * pushbuffer thread asks for one and moves on, and nothing else ever touches
 * the swapchain. */
static int SDLCALL vke_present_main(void *arg)
{
    (void)arg;
    for (;;) {
        SDL_LockMutex(w_mu);
        while (!w_req && !w_quit) SDL_CondWait(w_cv, w_mu);
        if (w_quit) { SDL_UnlockMutex(w_mu); break; }
        w_req = 0;
        SDL_UnlockMutex(w_mu);
        vke_frame_body();
    }
    return 0;
}

/* Called from the pushbuffer thread: ask, do not do. */
void nv2a_vk_engine_frame(void)
{
    if (!w_active || !w_mu) return;
    SDL_LockMutex(w_mu);
    w_req = 1;
    SDL_CondSignal(w_cv);
    SDL_UnlockMutex(w_mu);
}

static void vke_frame_body(void)
{
    uint32_t idx = 0, i;
    VkCommandBufferBeginInfo bi;
    VkClearColorValue clr;
    VkImageSubresourceRange rg;
    VkImageMemoryBarrier b;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkResult r;

    if (!w_active) return;
    { static int st; if (!st) { st = 1; vke("f1: acquire"); } }
    r = p_acquire(w_dev, w_sc, 1000000000ull, w_acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return;
    if (r != VK_SUCCESS || idx >= w_nimg) { vke("acquire -> %d", (int)r); return; }

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    { static int st; if (!st) { st = 1; vke("f1: record begin"); } }
    p_begin(w_cmd, &bi);
    { static int st; if (!st) { st = 1; vke("f1: barrier"); } }
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.dstAccessMask = w_draw_ready ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                    : VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = w_layout[idx];
    b.newLayout = w_draw_ready ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = w_img[idx];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    { static int st; if (!st) { st = 1; vke("f1: barrier call"); } }
    p_barrier(w_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              0, 0, NULL, 0, NULL, 1, &b);            /* dependencyFlags, barriers, buffers, images */
    memset(&clr, 0, sizeof clr);
    /* A colour that says the window is alive underneath whatever is drawn. */
    clr.float32[0] = 0.06f;
    clr.float32[1] = 0.10f;
    clr.float32[2] = 0.22f;
    clr.float32[3] = 1.0f;
    if (w_draw_ready) {
        /* Vulkan 1.3: no render pass, no framebuffer -- the attachment is
         * named here and the clear rides along with it. */
        VkRenderingAttachmentInfo att;
        VkRenderingInfo ri;
        VkViewport vp;
        VkRect2D sc;
        memset(&att, 0, sizeof att);
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = w_view[idx];
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = clr;
        memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.offset.x = 0;
        ri.renderArea.offset.y = 0;
        ri.renderArea.extent = w_extent;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        { static int st; if (!st) { st = 1; vke("f1: rendering begin"); } }
        p_begin_rendering(w_cmd, &ri);
        vp.x = 0.0f; vp.y = 0.0f;
        vp.width = (float)w_extent.width;
        vp.height = (float)w_extent.height;
        vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
        p_viewport(w_cmd, 0, 1, &vp);
        sc.offset.x = 0; sc.offset.y = 0;
        sc.extent = w_extent;
        p_scissor(w_cmd, 0, 1, &sc);
        p_bind_pipeline(w_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w_pipe);
        { static int st2; if (!st2) { st2 = 1; vke("f1: draw"); } }
        p_cmd_draw(w_cmd, 3, 1, 0, 0);
        { static int st3; if (!st3) { st3 = 1; vke("f1: end rendering"); } }
        p_end_rendering(w_cmd);
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        memset(&rg, 0, sizeof rg);
        rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.levelCount = 1;
        rg.layerCount = 1;
    { static int st; if (!st) { st = 1; vke("f1: clear call"); } }
        p_clear(w_cmd, w_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rg);
    { static int st; if (!st) { st = 1; vke("f1: clear done"); } }
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    p_barrier(w_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &w_acq;
    {   VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        si.pWaitDstStageMask = &st; }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &w_cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &w_done;
    p_reset(w_dev, 1, &w_fence);
    { static int st5; if (!st5) { st5 = 1; vke("f1: submit"); } }
    if (p_submit(w_queue, 1, &si, w_fence) != VK_SUCCESS) return;
    { static int st6; if (!st6) { st6 = 1; vke("f1: wait"); } }
    if (p_wait(w_dev, 1, &w_fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return;

    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &w_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &w_sc;
    pi.pImageIndices = &idx;
    if (p_present(w_queue, &pi) == VK_SUCCESS) {
        static int shown;
        if (!shown) { shown = 1; vke("first frame presented (triangle drawn with dynamic rendering)"); }
    }
    for (i = 0; i < w_nimg; i++)
        if (w_layout[i] != VK_IMAGE_LAYOUT_UNDEFINED) break;
}

void nv2a_vk_engine_stop(void)
{
    w_active = 0;
    if (w_dev) {
        if (p_device_idle) p_device_idle(w_dev);
        if (w_fence && p_fence_free) p_fence_free(w_dev, w_fence, NULL);
        if (w_acq && p_sem_free) p_sem_free(w_dev, w_acq, NULL);
        if (w_done && p_sem_free) p_sem_free(w_dev, w_done, NULL);
        if (w_pool && p_pool_free) p_pool_free(w_dev, w_pool, NULL);
        if (w_sc && p_destroy_swapchain) p_destroy_swapchain(w_dev, w_sc, NULL);
        if (p_destroy_device) p_destroy_device(w_dev, NULL);
    }
    if (w_surf && p_destroy_surface) p_destroy_surface(w_inst, w_surf, NULL);
    if (w_inst && p_destroy_instance) p_destroy_instance(w_inst, NULL);
    if (w_win) SDL_DestroyWindow(w_win);
    w_dev = VK_NULL_HANDLE; w_inst = VK_NULL_HANDLE; w_sc = VK_NULL_HANDLE;
    w_surf = VK_NULL_HANDLE; w_pool = VK_NULL_HANDLE; w_win = NULL;
}

int nv2a_vk_engine_requested(void)
{
    return vke_switch_on("RECOMP_BACKEND_VULKAN") || vke_switch_on("RECOMP_VK_ENGINE");
}

#else

int nv2a_vk_engine_start(int w, int h) { (void)w; (void)h; return 0; }
int nv2a_vk_engine_active(void) { return 0; }
void nv2a_vk_engine_frame(void) { }
void nv2a_vk_engine_stop(void) { }
int nv2a_vk_engine_requested(void) { return 0; }

#endif
