/*
 * A Vulkan smoke test for the backend that does not exist yet.
 *
 * The engine renders through OpenGL ES today, and every measurement says the
 * cost that matters is the per-draw submission -- 443 draws a frame at about
 * 21 us each, of which the driver's own glDraw work is 7. A Vulkan backend is
 * the only way to change that, and before one is written it is worth knowing
 * that the phone's driver, the NDK's loader and this build can do the whole
 * thing at all: instance, device, an image, a clear, a copy back, and the
 * pixel read out of it.
 *
 * RECOMP_VK_PROBE=1 runs it once at start-up (setprop
 * debug.jsrf.recomp_vk_probe 1 on a device) and says so in the log. Nothing
 * here touches the renderer: it makes its own objects and frees them.
 *
 * It is off unless NV2A_VK_SYSTEM_LOADER is defined, and that is the point:
 * calling the vk* functions by name makes libmain.so depend on the system's
 * libvulkan.so, which loads that loader before libadrenotools can put Turnip
 * in its place -- measured, the driver probe then gets no entry points out of
 * the patched loader. The driver probe (vk_driver_probe.c) needs no such
 * dependency and covers the same ground through whichever driver is chosen.
 */
#if defined(__ANDROID__) && defined(NV2A_VK_SYSTEM_LOADER)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../src/nv2a_gl/jsrf_debug_env.h"

#define VK_W 640
#define VK_H 480

static VkInstance       vk_instance;
static VkPhysicalDevice vk_phys;
static VkDevice         vk_dev;
static VkQueue          vk_queue;
static VkImage          vk_image;
static VkDeviceMemory   vk_img_mem;
static VkBuffer         vk_buf;
static VkDeviceMemory   vk_buf_mem;
static VkCommandPool    vk_pool;
static VkCommandBuffer  vk_cmd;
static VkFence          vk_fence;

static void vk_say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    jsrf_dbg_log("[VK] %s\n", line);
}

static const char *vk_what(VkResult r)
{
    switch (r) {
    case VK_SUCCESS:                       return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:   return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:             return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:       return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:       return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:   return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:     return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:     return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_TIMEOUT:                       return "VK_TIMEOUT";
    default:                               return "VK_ERROR_<other>";
    }
}

#define VK_MUST(call, what) do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            vk_say("FAILED: %s -> %s (%d)", what, vk_what(_r), (int)_r); \
            goto out; \
        } \
    } while (0)

/* A memory type with every bit of `need`, and with `want` if any type has
 * both: device-local where it exists, host-visible for the readback. */
static uint32_t vk_mem_type(uint32_t need, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i, fallback = UINT32_MAX;
    vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        if ((mp.memoryTypes[i].propertyFlags & need) != need) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

static void vk_barrier(VkCommandBuffer cmd, VkImage img,
                       VkImageLayout from, VkImageLayout to,
                       VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_free_all(void)
{
    if (vk_dev) {
        vkDeviceWaitIdle(vk_dev);
        if (vk_fence)  vkDestroyFence(vk_dev, vk_fence, NULL);
        if (vk_pool)   vkDestroyCommandPool(vk_dev, vk_pool, NULL);
        if (vk_buf)    vkDestroyBuffer(vk_dev, vk_buf, NULL);
        if (vk_buf_mem) vkFreeMemory(vk_dev, vk_buf_mem, NULL);
        if (vk_image)  vkDestroyImage(vk_dev, vk_image, NULL);
        if (vk_img_mem) vkFreeMemory(vk_dev, vk_img_mem, NULL);
        vkDestroyDevice(vk_dev, NULL);
    }
    if (vk_instance) vkDestroyInstance(vk_instance, NULL);
    vk_dev = VK_NULL_HANDLE; vk_instance = VK_NULL_HANDLE;
}

int nv2a_vk_probe(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkDeviceCreateInfo dci;
    VkDeviceQueueCreateInfo qci;
    VkImageCreateInfo img;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cbi;
    VkCommandBufferBeginInfo bi;
    VkClearColorValue clear;
    VkImageSubresourceRange range;
    VkBufferImageCopy region;
    VkFenceCreateInfo fci;
    VkSubmitInfo si;
    uint32_t n = 0, i, family = UINT32_MAX;
    float prio = 1.0f;
    VkPhysicalDeviceProperties props;
    const uint8_t *px;
    void *mapped = NULL;
    uint8_t want[4] = { 64, 128, 191, 255 };   /* 0.25, 0.5, 0.75, 1.0 */
    int ok = 0;

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "JSRF Unleashed";
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VK_MUST(vkCreateInstance(&ici, NULL, &vk_instance), "vkCreateInstance");

    VK_MUST(vkEnumeratePhysicalDevices(vk_instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (!n) { vk_say("no Vulkan device at all"); goto out; }
    /* The first is the one Android gives a phone: the Adreno. */
    VK_MUST(vkEnumeratePhysicalDevices(vk_instance, &n, &vk_phys), "vkEnumeratePhysicalDevices(2)");
    vkGetPhysicalDeviceProperties(vk_phys, &props);
    vk_say("device: %s (type %d, Vulkan %u.%u.%u, driver 0x%08X)",
           props.deviceName,
           (int)props.deviceType,
           (unsigned)VK_VERSION_MAJOR(props.apiVersion),
           (unsigned)VK_VERSION_MINOR(props.apiVersion),
           (unsigned)VK_VERSION_PATCH(props.apiVersion),
           (unsigned)props.driverVersion);

    {   /* A queue that can do graphics, which is all this needs. */
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qn, NULL);
        if (qn) {
            VkQueueFamilyProperties *qp = malloc(qn * sizeof *qp);
            if (qp) {
                vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qn, qp);
                for (i = 0; i < qn && family == UINT32_MAX; i++)
                    if (qp[i].queueCount && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                        family = i;
                free(qp);
            }
        }
    }
    if (family == UINT32_MAX) { vk_say("no graphics queue family"); goto out; }
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK_MUST(vkCreateDevice(vk_phys, &dci, NULL, &vk_dev), "vkCreateDevice");
    vkGetDeviceQueue(vk_dev, family, 0, &vk_queue);

    /* The image the renderer would draw into. */
    memset(&img, 0, sizeof img);
    img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.extent.width = VK_W;
    img.extent.height = VK_H;
    img.extent.depth = 1;
    img.mipLevels = 1;
    img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_MUST(vkCreateImage(vk_dev, &img, NULL, &vk_image), "vkCreateImage");
    vkGetImageMemoryRequirements(vk_dev, vk_image, &req);
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { vk_say("no memory type for the image"); goto out; }
    VK_MUST(vkAllocateMemory(vk_dev, &mai, NULL, &vk_img_mem), "vkAllocateMemory(image)");
    VK_MUST(vkBindImageMemory(vk_dev, vk_image, vk_img_mem, 0), "vkBindImageMemory");

    /* Where the picture is read back to: the frame buffer of the console's
     * memory, as the GL backend has one. */
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)VK_W * VK_H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_MUST(vkCreateBuffer(vk_dev, &bci, NULL, &vk_buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(vk_dev, vk_buf, &req);
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = vk_mem_type(req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { vk_say("no host-visible memory type"); goto out; }
    VK_MUST(vkAllocateMemory(vk_dev, &mai, NULL, &vk_buf_mem), "vkAllocateMemory(buffer)");
    VK_MUST(vkBindBufferMemory(vk_dev, vk_buf, vk_buf_mem, 0), "vkBindBufferMemory");

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    VK_MUST(vkCreateCommandPool(vk_dev, &pci, NULL, &vk_pool), "vkCreateCommandPool");
    memset(&cbi, 0, sizeof cbi);
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = vk_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_MUST(vkAllocateCommandBuffers(vk_dev, &cbi, &vk_cmd), "vkAllocateCommandBuffers");
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_MUST(vkCreateFence(vk_dev, &fci, NULL, &vk_fence), "vkCreateFence");

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_MUST(vkBeginCommandBuffer(vk_cmd, &bi), "vkBeginCommandBuffer");

    vk_barrier(vk_cmd, vk_image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&clear, 0, sizeof clear);
    clear.float32[0] = 0.25f;
    clear.float32[1] = 0.5f;
    clear.float32[2] = 0.75f;
    clear.float32[3] = 1.0f;
    memset(&range, 0, sizeof range);
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(vk_cmd, vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);
    vk_barrier(vk_cmd, vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = VK_W;
    region.imageExtent.height = VK_H;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(vk_cmd, vk_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           vk_buf, 1, &region);
    VK_MUST(vkEndCommandBuffer(vk_cmd), "vkEndCommandBuffer");

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk_cmd;
    VK_MUST(vkQueueSubmit(vk_queue, 1, &si, vk_fence), "vkQueueSubmit");
    VK_MUST(vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, 1000000000ull), "vkWaitForFences");
    VK_MUST(vkMapMemory(vk_dev, vk_buf_mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    px = (const uint8_t *)mapped;
    vk_say("cleared %dx%d; first pixel %u %u %u %u, wanted %u %u %u %u",
           VK_W, VK_H, px[0], px[1], px[2], px[3],
           want[0], want[1], want[2], want[3]);
    {   /* Every pixel, not just the first: a driver that clears only the tile
         * it touched would still pass a one-pixel test. */
        size_t p, bad = 0;
        for (p = 0; p < (size_t)VK_W * VK_H * 4; p += 4)
            if (px[p] != want[0] || px[p + 1] != want[1] || px[p + 2] != want[2])
                bad++;
        vk_say("%llu of %d pixels differ from the clear colour",
               (unsigned long long)bad, VK_W * VK_H);
        ok = (bad == 0);
    }
    vkUnmapMemory(vk_dev, vk_buf_mem);
    vk_say(ok ? "OK: instance, device, image, clear and readback all work"
              : "the clear did not land in the readback");

out:
    if (mapped) vkUnmapMemory(vk_dev, vk_buf_mem);
    vk_free_all();
    return ok ? 0 : 1;
}

#else  /* not Android: no loader to test against */

int nv2a_vk_probe(void) { return 0; }

#endif
