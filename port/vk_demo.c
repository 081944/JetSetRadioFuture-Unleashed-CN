/*
 * Window takeover: the whole app, running on Vulkan, clearing the screen.
 *
 * Two questions are left before a backend can be written: can this app take
 * ITS OWN window -- the one SDL gives it, which on Android is the only one
 * (making a second and destroying it killed the GL context: measured, see
 * docs/android-vulkan.md) -- with SDL_WINDOW_VULKAN, and can frames made that
 * way reach the display through Turnip?
 *
 * RECOMP_VK_DEMO=1 (setprop debug.jsrf.recomp_vk_demo 1) answers both: it
 * opens the window, makes an instance on Turnip's loader, a surface, a
 * swapchain, and presents cleared frames until the app is left or 600 of them
 * have gone by. Then it exits; the game is not started at all in this mode,
 * so nothing else is involved.
 *
 * This is also the skeleton the backend will be built on: the instance,
 * surface, device, swapchain, acquire/submit/present and the recreate path are
 * the same code, and the clear becomes the title's first pass.
 */
#if defined(__ANDROID__)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>

#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <SDL.h>
#include <SDL_syswm.h>

#include <adrenotools/driver.h>

#include "../src/nv2a_gl/jsrf_debug_env.h"

#define VKD_LIB "libvulkan_freedreno.so"

static void vkd(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    jsrf_dbg_log("[VKM] %s\n", line);
}

static int vkd_dir(char *out, size_t n)
{
    Dl_info info;
    char *slash;
    if (!dladdr((void *)vkd_dir, &info) || !info.dli_fname) return 0;
    snprintf(out, n, "%s", info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash) return 0;
    slash[1] = '\0';
    return 1;
}

/* A switch is on unless it says otherwise: "0" and empty are off. Every probe
 * here is read from a system property, and a property cannot be removed from
 * adb, so `setprop debug.jsrf.recomp_vk_demo 0` has to mean off. */
static int vk_switch_on(const char *name)
{
    const char *v = jsrf_getenv(name);
    if (!v || !*v) return 0;
    return !(v[0] == '0' && !v[1]);
}

int nv2a_vk_demo_requested(void)
{
    return vk_switch_on("RECOMP_VK_DEMO");
}

int nv2a_vk_demo(void)
{
    char lib_dir[512];
    void *handle = NULL;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create_instance = NULL;
    PFN_vkDestroyInstance destroy_instance = NULL;
    PFN_vkEnumeratePhysicalDevices enum_dev = NULL;
    PFN_vkGetPhysicalDeviceProperties dev_props = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_families = NULL;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support = NULL;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps = NULL;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surface_formats = NULL;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR surface_modes = NULL;
    PFN_vkCreateDevice create_device = NULL;
    PFN_vkDestroyDevice destroy_device = NULL;
    PFN_vkGetDeviceQueue get_queue = NULL;
    PFN_vkDeviceWaitIdle device_idle = NULL;
    PFN_vkDestroySurfaceKHR destroy_surface = NULL;
    PFN_vkCreateSwapchainKHR create_swapchain = NULL;
    PFN_vkDestroySwapchainKHR destroy_swapchain = NULL;
    PFN_vkGetSwapchainImagesKHR swapchain_images = NULL;
    PFN_vkAcquireNextImageKHR acquire = NULL;
    PFN_vkQueuePresentKHR present = NULL;
    PFN_vkCreateSemaphore create_sem = NULL;
    PFN_vkDestroySemaphore destroy_sem = NULL;
    PFN_vkCreateFence create_fence = NULL;
    PFN_vkDestroyFence destroy_fence = NULL;
    PFN_vkWaitForFences wait_fence = NULL;
    PFN_vkResetFences reset_fence = NULL;
    PFN_vkCreateCommandPool create_pool = NULL;
    PFN_vkDestroyCommandPool destroy_pool = NULL;
    PFN_vkAllocateCommandBuffers alloc_cmds = NULL;
    PFN_vkBeginCommandBuffer begin_cmd = NULL;
    PFN_vkEndCommandBuffer end_cmd = NULL;
    PFN_vkCmdPipelineBarrier pipeline_barrier = NULL;
    PFN_vkCmdClearColorImage clear_image = NULL;
    PFN_vkQueueSubmit queue_submit = NULL;

    SDL_Window *win = NULL;
    ANativeWindow *native_window = NULL;
    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acq = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkImage images[8];
    VkImageLayout layout[8];
    uint32_t nimg = 0, family = UINT32_MAX, i;
    VkSurfaceFormatKHR fmt;
    VkExtent2D extent;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    int frames = 0, ok = 0;
    VkResult r;

    if (!vkd_dir(lib_dir, sizeof lib_dir)) { vkd("no native dir"); return 1; }
    handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                        NULL, lib_dir, lib_dir, VKD_LIB, NULL, NULL);
    if (!handle) { vkd("no Turnip loader in %s", lib_dir); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) { vkd("no vkGetInstanceProcAddr"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0 && SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        vkd("SDL video would not start: %s", SDL_GetError());
        return 1;
    }
    /* The app's one window -- SDL makes it, and SDL's own WM info is where its
     * native window is. No SDL Vulkan loader is involved: that one would open
     * the system's libvulkan.so, and the whole point is that Turnip's loader
     * owns this instance (docs/android-vulkan.md). */
    win = SDL_CreateWindow("JSRF Unleashed (Vulkan)", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, 0, 0,
                           SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN_DESKTOP
                         | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { vkd("SDL_CreateWindow(VULKAN): %s", SDL_GetError()); return 1; }
    {   SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (!SDL_GetWindowWMInfo(win, &wmi) || wmi.subsystem != SDL_SYSWM_ANDROID) {
            vkd("no WM info: %s", SDL_GetError());
            return 1;
        }
        native_window = wmi.info.android.window;
    }
    if (!native_window) { vkd("SDL has no native window"); return 1; }
    vkd("native window %p (%dx%d)", (void *)native_window,
        ANativeWindow_getWidth(native_window), ANativeWindow_getHeight(native_window));
    {   unsigned n = 0;
        const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        VkApplicationInfo app;
        VkInstanceCreateInfo ici;
        n = 2;
        memset(&app, 0, sizeof app);
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "JSRF Unleashed";
        app.apiVersion = VK_API_VERSION_1_3;
        memset(&ici, 0, sizeof ici);
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = n;
        ici.ppEnabledExtensionNames = exts;
        create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
        if (!create_instance) { vkd("no vkCreateInstance"); return 1; }
        r = create_instance(&ici, NULL, &inst);
        if (r != VK_SUCCESS) { vkd("vkCreateInstance -> %d", (int)r); return 1; }
        destroy_instance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
        enum_dev = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
        dev_props = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
        queue_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
        surface_support = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
        surface_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        surface_formats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        surface_modes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        destroy_surface = (PFN_vkDestroySurfaceKHR)gipa(inst, "vkDestroySurfaceKHR");
    }
    {   PFN_vkCreateAndroidSurfaceKHR create_android_surface =
            (PFN_vkCreateAndroidSurfaceKHR)gipa(inst, "vkCreateAndroidSurfaceKHR");
        VkAndroidSurfaceCreateInfoKHR aci;
        if (!create_android_surface) { vkd("no vkCreateAndroidSurfaceKHR"); goto out; }
        memset(&aci, 0, sizeof aci);
        aci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        aci.window = native_window;
        r = create_android_surface(inst, &aci, NULL, &surf);
        if (r != VK_SUCCESS) { vkd("vkCreateAndroidSurfaceKHR -> %d", (int)r); goto out; }
    }
    {   uint32_t n = 0;
        if (enum_dev(inst, &n, NULL) != VK_SUCCESS || !n) { vkd("no device"); goto out; }
        if (n > 4) n = 4;
        {   VkPhysicalDevice devs[4];
            enum_dev(inst, &n, devs);
            phys = devs[0];
            {   VkPhysicalDeviceProperties pr;
                dev_props(phys, &pr);
                vkd("device %s (%u.%u.%u)", pr.deviceName,
                    (unsigned)VK_VERSION_MAJOR(pr.apiVersion),
                    (unsigned)VK_VERSION_MINOR(pr.apiVersion),
                    (unsigned)VK_VERSION_PATCH(pr.apiVersion)); }
        }
    }
    {   uint32_t nq = 0, k;
        VkQueueFamilyProperties qp[8];
        queue_families(phys, &nq, NULL);
        if (nq > 8) nq = 8;
        queue_families(phys, &nq, qp);
        for (k = 0; k < nq; k++) {
            VkBool32 sup = VK_FALSE;
            if (!(qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            surface_support(phys, k, surf, &sup);
            if (sup) { family = k; break; }
        }
    }
    if (family == UINT32_MAX) { vkd("no graphics+present queue"); goto out; }
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
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_ext;
        create_device = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
        if (!create_device || create_device(phys, &dci, NULL, &dev) != VK_SUCCESS) {
            vkd("vkCreateDevice failed"); goto out;
        }
        destroy_device = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
        get_queue = (PFN_vkGetDeviceQueue)gipa(inst, "vkGetDeviceQueue");
        device_idle = (PFN_vkDeviceWaitIdle)gipa(inst, "vkDeviceWaitIdle");
        get_queue(dev, family, 0, &queue);
    }
    create_swapchain = (PFN_vkCreateSwapchainKHR)gipa(inst, "vkCreateSwapchainKHR");
    destroy_swapchain = (PFN_vkDestroySwapchainKHR)gipa(inst, "vkDestroySwapchainKHR");
    swapchain_images = (PFN_vkGetSwapchainImagesKHR)gipa(inst, "vkGetSwapchainImagesKHR");
    acquire = (PFN_vkAcquireNextImageKHR)gipa(inst, "vkAcquireNextImageKHR");
    present = (PFN_vkQueuePresentKHR)gipa(inst, "vkQueuePresentKHR");
    create_sem = (PFN_vkCreateSemaphore)gipa(inst, "vkCreateSemaphore");
    destroy_sem = (PFN_vkDestroySemaphore)gipa(inst, "vkDestroySemaphore");
    create_fence = (PFN_vkCreateFence)gipa(inst, "vkCreateFence");
    destroy_fence = (PFN_vkDestroyFence)gipa(inst, "vkDestroyFence");
    wait_fence = (PFN_vkWaitForFences)gipa(inst, "vkWaitForFences");
    reset_fence = (PFN_vkResetFences)gipa(inst, "vkResetFences");
    create_pool = (PFN_vkCreateCommandPool)gipa(inst, "vkCreateCommandPool");
    destroy_pool = (PFN_vkDestroyCommandPool)gipa(inst, "vkDestroyCommandPool");
    alloc_cmds = (PFN_vkAllocateCommandBuffers)gipa(inst, "vkAllocateCommandBuffers");
    begin_cmd = (PFN_vkBeginCommandBuffer)gipa(inst, "vkBeginCommandBuffer");
    end_cmd = (PFN_vkEndCommandBuffer)gipa(inst, "vkEndCommandBuffer");
    pipeline_barrier = (PFN_vkCmdPipelineBarrier)gipa(inst, "vkCmdPipelineBarrier");
    clear_image = (PFN_vkCmdClearColorImage)gipa(inst, "vkCmdClearColorImage");
    queue_submit = (PFN_vkQueueSubmit)gipa(inst, "vkQueueSubmit");
    if (!create_swapchain || !swapchain_images || !acquire || !present
        || !create_sem || !create_fence || !wait_fence || !reset_fence
        || !create_pool || !alloc_cmds || !begin_cmd || !end_cmd
        || !pipeline_barrier || !clear_image || !queue_submit) {
        vkd("device entry points incomplete"); goto out;
    }
    {   VkSurfaceCapabilitiesKHR caps;
        uint32_t nf = 0, nm = 0;
        VkSurfaceFormatKHR fmts[32];
        VkPresentModeKHR modes[8];
        surface_caps(phys, surf, &caps);
        surface_formats(phys, surf, &nf, NULL);
        if (nf > 32) nf = 32;
        surface_formats(phys, surf, &nf, fmts);
        surface_modes(phys, surf, &nm, NULL);
        if (nm > 8) nm = 8;
        surface_modes(phys, surf, &nm, modes);
        if (!nf) { vkd("no surface formats"); goto out; }
        fmt = fmts[0];
        for (i = 0; i < nf; i++)
            if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM
             || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = fmts[i]; break; }
        for (i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
        extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) {
            extent.width = 480; extent.height = 320;
            if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
            if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
        }
        vkd("swapchain %ux%u, format %d, %u present modes",
            extent.width, extent.height, (int)fmt.format, nm);
        {   VkSwapchainCreateInfoKHR sci;
            memset(&sci, 0, sizeof sci);
            sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            sci.surface = surf;
            sci.minImageCount = caps.minImageCount + 1;
            if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
                sci.minImageCount = caps.maxImageCount;
            sci.imageFormat = fmt.format;
            sci.imageColorSpace = fmt.colorSpace;
            sci.imageExtent = extent;
            sci.imageArrayLayers = 1;
            sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = caps.currentTransform;
            sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            sci.presentMode = mode;
            sci.clipped = VK_TRUE;
            r = create_swapchain(dev, &sci, NULL, &sc);
            if (r != VK_SUCCESS) { vkd("vkCreateSwapchainKHR -> %d", (int)r); goto out; }
        }
        if (swapchain_images(dev, sc, &nimg, NULL) != VK_SUCCESS || !nimg) {
            vkd("no swapchain images"); goto out;
        }
        if (nimg > 8) nimg = 8;
        swapchain_images(dev, sc, &nimg, images);
        for (i = 0; i < nimg; i++) layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    {   VkCommandPoolCreateInfo pci;
        VkCommandBufferAllocateInfo cbi;
        VkFenceCreateInfo fci;
        VkSemaphoreCreateInfo sci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = family;
        if (create_pool(dev, &pci, NULL, &pool) != VK_SUCCESS) { vkd("pool"); goto out; }
        memset(&cbi, 0, sizeof cbi);
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        if (alloc_cmds(dev, &cbi, &cmd) != VK_SUCCESS) { vkd("cmd"); goto out; }
        memset(&fci, 0, sizeof fci);
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        create_fence(dev, &fci, NULL, &fence);
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create_sem(dev, &sci, NULL, &acq);
        create_sem(dev, &sci, NULL, &done);
    }

    for (i = 0; i < 600; i++) {
        uint32_t idx = 0;
        VkCommandBufferBeginInfo bi;
        VkClearColorValue clr;
        VkImageSubresourceRange rg;
        VkImageMemoryBarrier b;
        VkSubmitInfo si;
        VkPresentInfoKHR pi;
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) i = 600;
        if (i >= 600) break;

        r = acquire(dev, sc, 1000000000ull, acq, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) continue;
        if (r != VK_SUCCESS || idx >= nimg) { vkd("acquire -> %d", (int)r); break; }

        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_cmd(cmd, &bi);
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = layout[idx];
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        pipeline_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        memset(&clr, 0, sizeof clr);
        clr.float32[0] = 0.05f;
        clr.float32[1] = 0.25f + 0.15f * (float)((i / 30) & 1);
        clr.float32[2] = 0.55f;
        clr.float32[3] = 1.0f;
        memset(&rg, 0, sizeof rg);
        rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.levelCount = 1;
        rg.layerCount = 1;
        clear_image(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rg);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        end_cmd(cmd);
        layout[idx] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acq;
        {   VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            si.pWaitDstStageMask = &st; }
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &done;
        reset_fence(dev, 1, &fence);
        if (queue_submit(queue, 1, &si, fence) != VK_SUCCESS) break;
        if (wait_fence(dev, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
            vkd("fence never signalled"); break;
        }
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        r = present(queue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR) {
            vkd("present -> %d", (int)r);
            break;
        }
        frames++;
        if (frames == 1) vkd("first frame presented on %s", "Turnip");
    }
    ok = frames > 0;
    vkd("%d frames presented%s", frames,
        ok ? " -- window takeover, surface, swapchain and present all work" : "");

out:
    if (dev) {
        if (device_idle) device_idle(dev);
        if (fence && destroy_fence) destroy_fence(dev, fence, NULL);
        if (acq && destroy_sem) destroy_sem(dev, acq, NULL);
        if (done && destroy_sem) destroy_sem(dev, done, NULL);
        if (pool && destroy_pool) destroy_pool(dev, pool, NULL);
        if (sc && destroy_swapchain) destroy_swapchain(dev, sc, NULL);
        if (destroy_device) destroy_device(dev, NULL);
    }
    if (surf && destroy_surface) destroy_surface(inst, surf, NULL);
    if (inst && destroy_instance) destroy_instance(inst, NULL);
    if (native_window) ANativeWindow_release(native_window);
    return ok ? 0 : 1;
}

#else

int nv2a_vk_demo_requested(void) { return 0; }
int nv2a_vk_demo(void) { return 0; }

#endif
