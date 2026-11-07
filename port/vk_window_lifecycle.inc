/* Android replaces Surface/ANativeWindow when the activity returns. Keep
 * guest render targets and pipelines alive; rebuild only WSI resources on
 * the presenter thread. The UI only publishes an owned window reference. */
static pthread_mutex_t w_surface_mu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *w_requested_native;
static unsigned w_surface_generation, w_seen_surface_generation;
static int w_rebuild_window;

void nv2a_vk_engine_surface(ANativeWindow *window)
{
    pthread_mutex_lock(&w_surface_mu);
    ANativeWindow *old = w_requested_native;
    w_requested_native = window; /* takes the JNI caller's acquired reference */
    ++w_surface_generation;
    pthread_mutex_unlock(&w_surface_mu);
    if (old) ANativeWindow_release(old);
}

static int vke_window_result(VkResult r, const char *operation)
{
    if (r == VK_SUCCESS) return 1;
    if (r == VK_SUBOPTIMAL_KHR) { w_rebuild_window = 1; return 1; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        vke("%s -> %d; rebuilding window resources", operation, (int)r);
        w_rebuild_window = 1;
        return 0;
    }
    vke("%s -> %d", operation, (int)r);
    w_failed = 1;
    return 0;
}

/* Queue idle is required before retiring present semaphores, image views and
 * an old native surface. The game uses this same queue mutex for submits. */
static void vke_window_drop(void)
{
    for (uint32_t i = 0; i < 8; ++i) {
        if (w_view[i]) p_view_free(w_dev, w_view[i], NULL);
        if (w_done[i]) p_sem_free(w_dev, w_done[i], NULL);
        w_view[i] = VK_NULL_HANDLE; w_done[i] = VK_NULL_HANDLE;
        w_img[i] = VK_NULL_HANDLE; w_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (w_acq) p_sem_free(w_dev, w_acq, NULL);
    if (w_fence) p_fence_free(w_dev, w_fence, NULL);
    if (w_sc) p_destroy_swapchain(w_dev, w_sc, NULL);
    if (w_surf) p_destroy_surface(w_inst, w_surf, NULL);
    if (w_native) ANativeWindow_release(w_native);
    w_acq = VK_NULL_HANDLE; w_fence = VK_NULL_HANDLE;
    w_sc = VK_NULL_HANDLE; w_surf = VK_NULL_HANDLE;
    w_native = NULL; w_nimg = 0;
}

static int vke_window_refresh(void)
{
    if (!w_dev || w_failed) return 0;
    pthread_mutex_lock(&w_surface_mu);
    unsigned generation = w_surface_generation;
    if (generation == w_seen_surface_generation && !w_rebuild_window) {
        pthread_mutex_unlock(&w_surface_mu);
        return w_sc != VK_NULL_HANDLE;
    }
    ANativeWindow *window = generation ? w_requested_native : w_native;
    if (window) ANativeWindow_acquire(window);
    w_seen_surface_generation = generation;
    pthread_mutex_unlock(&w_surface_mu);

    SDL_LockMutex(w_q_mu);
    VkResult r = p_device_idle(w_dev);
    if (r != VK_SUCCESS) {
        SDL_UnlockMutex(w_q_mu);
        if (window) ANativeWindow_release(window);
        return vke_window_result(r, "window retire wait");
    }
    vke_window_drop();
    w_rebuild_window = 0;
    if (!window) {
        SDL_UnlockMutex(w_q_mu);
        vke("window suspended (surface generation %u)", generation);
        return 0;
    }
    w_native = window;
    PFN_vkCreateAndroidSurfaceKHR surface_new = (PFN_vkCreateAndroidSurfaceKHR)p_gipa(w_inst,"vkCreateAndroidSurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps_get = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)p_gipa(w_inst,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats_get = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)p_gipa(w_inst,"vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkCreateSwapchainKHR swapchain_new = (PFN_vkCreateSwapchainKHR)p_gipa(w_inst,"vkCreateSwapchainKHR");
    VkAndroidSurfaceCreateInfoKHR aci = {.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=window};
    r = surface_new(w_inst,&aci,NULL,&w_surf);
    if (r != VK_SUCCESS) goto retry;
    VkSurfaceCapabilitiesKHR caps;
    r = caps_get(w_phys,w_surf,&caps);
    if (r != VK_SUCCESS) goto retry;
    VkSurfaceFormatKHR formats[32]; uint32_t count=32;
    r = formats_get(w_phys,w_surf,&count,formats);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !count) { if (!count) r=VK_ERROR_INITIALIZATION_FAILED; goto retry; }
    uint32_t chosen=0;
    while (chosen<count && formats[chosen].format!=w_format) ++chosen;
    if (chosen==count) { r=VK_ERROR_FORMAT_NOT_SUPPORTED; goto retry; }
    w_window_extent=caps.currentExtent;
    if (w_window_extent.width==UINT32_MAX) {
        int width=ANativeWindow_getWidth(window),height=ANativeWindow_getHeight(window);
        if (width<=0 || height<=0) { r=VK_NOT_READY; goto retry; }
        w_window_extent=(VkExtent2D){(uint32_t)width,(uint32_t)height};
        if (w_window_extent.width<caps.minImageExtent.width) w_window_extent.width=caps.minImageExtent.width;
        if (w_window_extent.height<caps.minImageExtent.height) w_window_extent.height=caps.minImageExtent.height;
        if (w_window_extent.width>caps.maxImageExtent.width) w_window_extent.width=caps.maxImageExtent.width;
        if (w_window_extent.height>caps.maxImageExtent.height) w_window_extent.height=caps.maxImageExtent.height;
    }
    if (!w_window_extent.width || !w_window_extent.height) { r=VK_NOT_READY; goto retry; }
    w_extent=w_window_extent; w_transform=caps.currentTransform;
    if (w_transform==VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR || w_transform==VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        uint32_t tmp=w_extent.width; w_extent.width=w_extent.height; w_extent.height=tmp;
    }
    uint32_t images=caps.minImageCount+1;
    if (caps.maxImageCount && images>caps.maxImageCount) images=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha=(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (0u-caps.supportedCompositeAlpha));
    VkSwapchainCreateInfoKHR sci={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=w_surf,
        .minImageCount=images,.imageFormat=w_format,.imageColorSpace=formats[chosen].colorSpace,
        .imageExtent=w_extent,.imageArrayLayers=1,
        .imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=w_transform,
        .compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    r=swapchain_new(w_dev,&sci,NULL,&w_sc);
    if (r!=VK_SUCCESS) goto retry;
    r=p_sc_images(w_dev,w_sc,&w_nimg,NULL);
    if (r!=VK_SUCCESS || !w_nimg || w_nimg>8) { if (r==VK_SUCCESS) r=VK_ERROR_INITIALIZATION_FAILED; goto retry; }
    r=p_sc_images(w_dev,w_sc,&w_nimg,w_img);
    if (r!=VK_SUCCESS) goto retry;
    VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT};
    r=p_sem_new(w_dev,&sem,NULL,&w_acq); if (r!=VK_SUCCESS) goto retry;
    r=p_fence_new(w_dev,&fence,NULL,&w_fence); if (r!=VK_SUCCESS) goto retry;
    for (uint32_t i=0;i<w_nimg;++i) {
        r=p_sem_new(w_dev,&sem,NULL,&w_done[i]); if (r!=VK_SUCCESS) goto retry;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=w_img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=w_format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        r=p_view_new(w_dev,&vi,NULL,&w_view[i]); if (r!=VK_SUCCESS) goto retry;
    }
    SDL_UnlockMutex(w_q_mu);
    vke("window restored: generation %u, %ux%u, %u images",generation,w_window_extent.width,w_window_extent.height,w_nimg);
    return 1;
retry:
    vke_window_drop();
    SDL_UnlockMutex(w_q_mu);
    if (r==VK_NOT_READY || r==VK_ERROR_OUT_OF_DATE_KHR || r==VK_ERROR_SURFACE_LOST_KHR || r==VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        w_rebuild_window=1;
        return 0;
    }
    return vke_window_result(r,"window restore");
}
