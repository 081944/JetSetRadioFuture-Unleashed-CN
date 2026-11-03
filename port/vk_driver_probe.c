/*
 * Does a Turnip driver load on this phone?
 *
 * The stock Adreno 650 driver is Vulkan 1.1, and a renderer that wants dynamic
 * rendering, synchronization2 and push descriptors needs a driver that has
 * them. Mesa's Turnip does -- and on Android it is loaded by libadrenotools,
 * which has the system's loader open another driver for this app alone, with
 * no root and no change to the system.
 *
 * This probe answers the question that decides everything after it: does this
 * phone, this APK's native library folder, and this build's libadrenotools get
 * a Turnip instance far enough to name the device and its Vulkan version.
 *
 * RECOMP_VK_DRIVER_PROBE=1 (setprop debug.jsrf.recomp_vk_driver_probe 1).
 *
 * The driver and the hooks live in the APK's lib/arm64-v8a -- the app's
 * nativeLibraryDir, which the loader extracts because extractNativeLibs is
 * true -- so the folders are found from this library's own path (dladdr), not
 * from Java: nothing here needs the activity.
 */
#if defined(__ANDROID__)

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <adrenotools/driver.h>

#include "../src/nv2a_gl/jsrf_debug_env.h"

#define TURNIP_LIB "libvulkan_freedreno.so"

static void vkd_say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    jsrf_dbg_log("[VKD] %s\n", line);
}

/* The folder this library was loaded from: the APK's lib/arm64-v8a, where the
 * hooks and the driver also are. */
static int vkd_native_dir(char *out, size_t n)
{
    Dl_info info;
    char *slash;
    if (!dladdr((void *)vkd_native_dir, &info) || !info.dli_fname) return 0;
    snprintf(out, n, "%s", info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash) return 0;
    slash[1] = '\0';
    return 1;
}

/* A writable directory for adrenotools (it patches hooks there on old APIs).
 * The app's own files dir, named from the package in /proc/self/cmdline so no
 * Java is needed. */
static int vkd_tmp_dir(char *out, size_t n)
{
    char pkg[256];
    FILE *f = fopen("/proc/self/cmdline", "r");
    size_t len;
    if (!f) return 0;
    len = fread(pkg, 1, sizeof pkg - 1, f);
    fclose(f);
    if (!len) return 0;
    pkg[len] = '\0';
    /* /data/user/0 first: that is what API 24 and later hand out. */
    snprintf(out, n, "/data/user/0/%s/files", pkg);
    if (mkdir(out, 0700) == 0 || access(out, W_OK) == 0) return 1;
    snprintf(out, n, "/data/data/%s/files", pkg);
    if (mkdir(out, 0700) == 0 || access(out, W_OK) == 0) return 1;
    return 0;
}

int nv2a_vk_driver_probe(void)
{
    char lib_dir[512], tmp_dir[512];
    void *handle = NULL;
    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkCreateInstance p_create = NULL;
    PFN_vkEnumeratePhysicalDevices p_devices = NULL;
    PFN_vkGetPhysicalDeviceProperties p_props = NULL;
    PFN_vkDestroyInstance p_destroy = NULL;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    int ok = 0;

    if (!vkd_native_dir(lib_dir, sizeof lib_dir)) {
        vkd_say("cannot find this library's folder");
        return 1;
    }
    if (!vkd_tmp_dir(tmp_dir, sizeof tmp_dir)) {
        vkd_say("no writable temp dir");
        return 1;
    }
    vkd_say("native dir %s", lib_dir);
    vkd_say("temp dir   %s", tmp_dir);

    handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                        NULL, lib_dir, lib_dir, TURNIP_LIB,
                                        NULL, NULL);
    if (!handle) {
        const char *e = dlerror();
        vkd_say("adrenotools_open_libvulkan returned null (no %s in %s?): %s",
                TURNIP_LIB, lib_dir, e ? e : "no detail");
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        const char *e = dlerror();
        vkd_say("the loader has no vkGetInstanceProcAddr: %s", e ? e : "no detail");
        return 1;
    }
    /* Only the instance-level entry points come from gipa(NULL, ...): the rest
     * are the instance's, and a loader answers NULL for them until there is an
     * instance to answer for -- measured, that is what the first run of this
     * probe saw and read as a broken loader. */
    p_create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    vkd_say("gipa(NULL): create %p, instance extensions %p, instance version %p",
            (void *)p_create,
            (void *)(PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties"),
            (void *)gipa(NULL, "vkEnumerateInstanceVersion"));
    if (!p_create) {
        vkd_say("the loader did not hand out vkCreateInstance");
        return 1;
    }
    {   VkApplicationInfo app;
        VkInstanceCreateInfo ici;
        uint32_t n = 0;
        memset(&app, 0, sizeof app);
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "JSRF Unleashed";
        app.apiVersion = VK_API_VERSION_1_1;
        memset(&ici, 0, sizeof ici);
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        r = p_create(&ici, NULL, &inst);
        if (r != VK_SUCCESS) {
            vkd_say("vkCreateInstance through Turnip -> %d", (int)r);
            return 1;
        }
        p_devices = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
        p_destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
        p_props   = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
        if (!p_devices || !p_destroy || !p_props) {
            vkd_say("the instance has no device entry points (%p %p %p)",
                    (void *)p_devices, (void *)p_destroy, (void *)p_props);
            if (p_destroy) p_destroy(inst, NULL);
            return 1;
        }
        r = p_devices(inst, &n, NULL);
        if (r != VK_SUCCESS || !n) {
            vkd_say("Turnip enumerated no device (%d, %u)", (int)r, n);
            p_destroy(inst, NULL);
            return 1;
        }
        {   VkPhysicalDevice devs[4];
            uint32_t k = n > 4 ? 4 : n;
            p_devices(inst, &k, devs);
            for (uint32_t i = 0; i < k; i++) {
                VkPhysicalDeviceProperties pr;
                p_props(devs[i], &pr);
                vkd_say("device %u: %s (Vulkan %u.%u.%u, driver 0x%08X)",
                        i, pr.deviceName,
                        (unsigned)VK_VERSION_MAJOR(pr.apiVersion),
                        (unsigned)VK_VERSION_MINOR(pr.apiVersion),
                        (unsigned)VK_VERSION_PATCH(pr.apiVersion),
                        (unsigned)pr.driverVersion);
                if (VK_VERSION_MAJOR(pr.apiVersion) >= 1
                    && VK_VERSION_MINOR(pr.apiVersion) >= 3) ok = 1;
            }
        }
        p_destroy(inst, NULL);
    }
    vkd_say(ok ? "OK: Turnip loads and reports Vulkan 1.3 or newer"
               : "Turnip loaded but does not report Vulkan 1.3");
    return ok ? 0 : 1;
}

#else

int nv2a_vk_driver_probe(void) { return 0; }

#endif