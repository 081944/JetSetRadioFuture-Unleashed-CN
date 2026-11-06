/* Vulkan implementation of Nv2aGfx. Included by vk_engine.c so the renderer
 * uses the device/queue already owned by the Android window, never a second
 * loader. All recording happens on the pushbuffer thread. Three fenced
 * upload/descriptor slots keep allocations alive until the GPU retires them. */
#include "../src/nv2a_gl/nv2a_backend.h"
#include "../src/nv2a_gl/nv2a_gl.h"
#include <ctype.h>
extern int nv2a_vk_glsl_to_spirv(const char *, int, uint32_t **, size_t *);

#define VG_SLOTS 3
#define VG_UPLOAD (32u << 20)
#define VG_TEX 4096
#define VG_RT 64
#define VG_PROG 4096
#define VG_PIPES 8192
#define VG_PIPE_BUCKETS 4096
#define VG_SETS 4096
#define VG_UNIFORM_CAP 16384
#define VG_STATIC (128u << 20)
#define VG_VBUFS 16384
#define VG_RANGES 16384
#define VG_DESCRIPTOR_HASH 8192

#define VG_FUNCTIONS(X) \
 X(CreateSampler) X(DestroySampler) X(CmdCopyBufferToImage) X(CmdCopyImage) \
 X(CmdBlitImage) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindDescriptorSets) \
 X(CmdClearAttachments) X(CreatePipelineCache) X(DestroyPipelineCache) \
 X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdBindVertexBuffers) X(CreateQueryPool) X(DestroyQueryPool) \
 X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults)
#define VG_DECLARE(n) static PFN_vk##n vg##n;
VG_FUNCTIONS(VG_DECLARE)
#undef VG_DECLARE

typedef struct { uint32_t off, size; } VgRange;
typedef struct { uint32_t generation, tex[4]; VkDescriptorSet set; } VgDescriptor;
typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
    uint32_t w, h, levels;
    int owner;
} VgTexture;
typedef struct {
    VgTexture texture;
    int state;
    uint32_t flip;
    VkDescriptorSet set;
    float aspect; /* Scene shape captured with this frame, before hand-off. */
} VgDisplay;
typedef struct {
    uint32_t color, snapshot, w, h;
    VgTexture depth;
} VgTarget;
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *map;
    VkDescriptorPool descriptors;
    uint32_t pos, sets;
    uint32_t generation;
    VgDescriptor cache[VG_DESCRIPTOR_HASH];
    VkPipeline bound_pipeline;
    VgRange retired[VG_RANGES];
    uint32_t nretired;
    int timed, end_frame;
    uint32_t frame;
} VgSlot;
typedef struct {
    int used, stream;
    uint32_t off, size, vbytes, ibytes;
    uint8_t *vertices, *indices;
} VgVertexBuffer;
typedef struct {
    VkShaderModule vs, fs;
    uint16_t inputs;
    uint8_t uses[NV2A_U_COUNT];
    uint8_t *uniforms;
    uint32_t revision, cache_revision[VG_SLOTS], cache_generation[VG_SLOTS], cache_offset[VG_SLOTS];
} VgProgram;
typedef struct {
    uint32_t prog, prim;
    uint32_t stride, attr[16];
    Nv2aRenderState rs;
    VkPipeline pipeline;
} VgPipeline;

static VgTexture vg_tex[VG_TEX];
static VgTarget vg_rt[VG_RT];
static VgProgram vg_prog[VG_PROG];
static VgPipeline vg_pipes[VG_PIPES];
/* Index + 1, so zero terminates a chain. Dead pipelines remain in their
 * chain until shutdown and are skipped; program IDs can safely be reused. */
static uint32_t vg_pipe_head[VG_PIPE_BUCKETS], vg_pipe_next[VG_PIPES];
static VgSlot vg_slots[VG_SLOTS];
static VgVertexBuffer vg_vbufs[VG_VBUFS];
static VgRange vg_free_ranges[VG_RANGES];
static uint32_t vg_nfree_ranges, vg_cached_vb;
static VkBuffer vg_static_buffer;
static VkDeviceMemory vg_static_memory;
static uint8_t *vg_static_map;
static VkQueryPool vg_queries;
static double vg_timestamp_period, vg_gpu_total_ms, vg_wait_total_ms;
static uint32_t vg_timestamp_bits;
static struct { uint32_t frame; double us; } vg_gpu_frame;
static VkDescriptorSetLayout vg_set_layout;
static VkPipelineLayout vg_pipeline_layout;
static VkPipelineCache vg_pipeline_cache;
static VkFormat vg_depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
static uint32_t vg_uniform_offset[NV2A_U_COUNT], vg_uniform_stride[NV2A_U_COUNT];
static uint32_t vg_uniform_bytes, vg_attr_offset, vg_alignment;
static int vg_uniform_dedup;
static uint64_t vg_uniform_calls, vg_uniform_unchanged, vg_uniform_uploads, vg_uniform_upload_bytes;
static uint32_t vg_current_rt, vg_current_prog, vg_bound_tex[4], vg_white;
static uint32_t vg_slot_index, vg_vertex_offset, vg_vertex_bytes, vg_nverts;
static const float *vg_vertex_data;
static uint32_t vg_npipe, vg_draws, vg_frames, vg_previous_draws;
static atomic_uint vg_window_frames;
static VgDisplay vg_display[3];
static int vg_pending=-1;
static uint32_t vg_window_program;
static VkPipeline vg_window_pipeline;
static VkDescriptorPool vg_window_descriptors;
static VkBuffer vg_window_buffer;
static VkDeviceMemory vg_window_memory;
static uint8_t *vg_window_map;
static int vg_seen_scene;
static double vg_last_window;
static Nv2aRenderState vg_state;
static uint32_t vg_attr_stride, vg_attr_mask, vg_attr_at[16];
static float vg_attr_value[16][4];
static int vg_ready, vg_recording, vg_rendering, vg_depth_clamp;
static int vg_force_nocull, vg_force_nodepth, vg_debug;
static VkCommandBuffer vg_cmd;

static int vg_init(void);
static int vg_begin(void);
static void vg_end_pass(void);
static int vg_submit(int wait);
static void vg_drain(void);
static uint32_t vg_tex_new(uint32_t, uint32_t, const void *, const Nv2aSampler *);
static void vg_tex_free(uint32_t);
static void vg_stop(void);
static int vg_self_test(void);
static int vg_window_init(void);

static void vg_range_free(VgRange r)
{
    if (!r.size) return;
    uint32_t i=0;
    while(i<vg_nfree_ranges && vg_free_ranges[i].off<r.off) i++;
    if (i && vg_free_ranges[i-1].off+vg_free_ranges[i-1].size==r.off) {
        vg_free_ranges[i-1].size+=r.size;i--;
    } else {
        if(vg_nfree_ranges==VG_RANGES){vke("static vertex free-list exhausted");w_failed=1;return;}
        memmove(vg_free_ranges+i+1,vg_free_ranges+i,(vg_nfree_ranges-i)*sizeof r);
        vg_free_ranges[i]=r;vg_nfree_ranges++;
    }
    if(i+1<vg_nfree_ranges && vg_free_ranges[i].off+vg_free_ranges[i].size==vg_free_ranges[i+1].off) {
        vg_free_ranges[i].size+=vg_free_ranges[i+1].size;
        memmove(vg_free_ranges+i+1,vg_free_ranges+i+2,(vg_nfree_ranges-i-2)*sizeof r);vg_nfree_ranges--;
    }
}

static void vg_reap(VgSlot *s)
{
    for(uint32_t i=0;i<s->nretired;i++) vg_range_free(s->retired[i]);
    s->nretired=0;
    if(vg_queries && s->timed) {
        uint64_t ticks[2];
        uint32_t base=(uint32_t)(s-vg_slots)*2;
        if(vgGetQueryPoolResults(w_dev,vg_queries,base,2,sizeof ticks,ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            uint64_t mask=vg_timestamp_bits==64?UINT64_MAX:((1ull<<vg_timestamp_bits)-1);
            double us=(double)((ticks[1]-ticks[0])&mask)*vg_timestamp_period/1000.0;
            vg_gpu_total_ms+=us/1000.0;
            if(vg_gpu_frame.frame!=s->frame){vg_gpu_frame.frame=s->frame;vg_gpu_frame.us=0;}
            vg_gpu_frame.us+=us;
            if(s->end_frame) nv2a_gl_performance_backend_gpu((int)(vg_gpu_frame.us+0.5));
        }
        s->timed=0;
    }
}

static void vg_retire_range(uint32_t off,uint32_t size)
{
    if(!size || !vg_begin()) return;
    VgSlot *s=&vg_slots[vg_slot_index];
    if(s->nretired==VG_RANGES){vke("vertex retirement list exhausted");w_failed=1;return;}
    s->retired[s->nretired++]=(VgRange){off,size};
}

static int vg_range_alloc(uint32_t bytes,uint32_t *off,uint32_t *size)
{
    bytes=(bytes+vg_alignment-1u)&~(vg_alignment-1u);
    for(int attempt=0;attempt<2;attempt++) {
        for(uint32_t i=0;i<vg_nfree_ranges;i++) if(vg_free_ranges[i].size>=bytes) {
            *off=vg_free_ranges[i].off;*size=bytes;
            vg_free_ranges[i].off+=bytes;vg_free_ranges[i].size-=bytes;
            if(!vg_free_ranges[i].size){memmove(vg_free_ranges+i,vg_free_ranges+i+1,(vg_nfree_ranges-i-1)*sizeof(VgRange));vg_nfree_ranges--;}
            return 1;
        }
        vg_drain();
    }
    vke("static vertex arena exhausted");w_failed=1;return 0;
}

static int vg_ok(VkResult r, const char *what)
{
    if (r == VK_SUCCESS) return 1;
    vke("%s -> %d", what, (int)r);
    w_failed = 1;
    return 0;
}

static uint32_t vg_memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties p;
    p_mem_props(w_phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

static int vg_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer *buf,
                     VkDeviceMemory *mem, uint8_t **map)
{
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements req;
    if (!vg_ok(p_buf_new(w_dev, &ci, NULL, buf), "buffer")) return 0;
    p_buf_req(w_dev, *buf, &req);
    uint32_t type = vg_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if(type==UINT32_MAX) type=vg_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) { vke("no coherent upload memory"); return 0; }
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type };
    if (!vg_ok(p_mem_new(w_dev, &ai, NULL, mem), "buffer memory") ||
        !vg_ok(p_buf_bind(w_dev, *buf, *mem, 0), "bind buffer")) return 0;
    return vg_ok(p_map(w_dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map), "map buffer");
}

static int vg_image(VgTexture *t, uint32_t w, uint32_t h, uint32_t levels,
                    VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { w, h, 1 },
        .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryRequirements req;
    t->w = w; t->h = h; t->levels = levels;
    if (!vg_ok(p_image_new(w_dev, &ci, NULL, &t->image), "image")) return 0;
    p_img_req(w_dev, t->image, &req);
    uint32_t type = vg_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type };
    if (!vg_ok(p_mem_new(w_dev, &ai, NULL, &t->memory), "image memory") ||
        !vg_ok(p_img_bind(w_dev, t->image, t->memory, 0), "bind image")) return 0;
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = { aspect, 0, levels, 0, 1 } };
    return vg_ok(p_view_new(w_dev, &vi, NULL, &t->view), "image view");
}

static void vg_image_free(VgTexture *t)
{
    if (t->sampler) vgDestroySampler(w_dev, t->sampler, NULL);
    if (t->view) p_view_free(w_dev, t->view, NULL);
    if (t->image) p_image_free(w_dev, t->image, NULL);
    if (t->memory) p_mem_free(w_dev, t->memory, NULL);
    memset(t, 0, sizeof *t);
}

static int vg_sampler(VgTexture *t, const Nv2aSampler *s)
{
    static const VkSamplerAddressMode wrap[] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT };
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = s->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = s->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = s->mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap[s->wrap_s < 3 ? s->wrap_s : 0],
        .addressModeV = wrap[s->wrap_t < 3 ? s->wrap_t : 0],
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = s->mip ? (float)(t->levels - 1) : 0.0f };
    return vg_ok(vgCreateSampler(w_dev, &ci, NULL, &t->sampler), "sampler");
}

static void vg_transition(VgTexture *t, VkImageLayout to, VkImageAspectFlags aspect)
{
    /* Dynamic rendering has no implicit dependency between separate
     * instances. Reopening an attachment needs a memory barrier even when
     * its layout did not change. Read-only texture reuses need none. */
    if (t->layout == to && to != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        to != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) return;
    vg_end_pass();
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = t->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = t->layout, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t->image, .subresourceRange = { aspect, 0, t->levels, 0, 1 } };
    p_barrier(vg_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;
}

static int vg_begin(void)
{
    if (w_failed) return 0;
    if (vg_recording) return 1;
    VgSlot *s = &vg_slots[vg_slot_index];
    struct timespec wt0,wt1;clock_gettime(CLOCK_MONOTONIC,&wt0);
    if (!vg_ok(p_wait(w_dev, 1, &s->fence, VK_TRUE, UINT64_MAX), "upload slot wait")) return 0;
    clock_gettime(CLOCK_MONOTONIC,&wt1);
    vg_wait_total_ms+=(wt1.tv_sec-wt0.tv_sec)*1000.0+(wt1.tv_nsec-wt0.tv_nsec)/1e6;
    vg_reap(s);
    if (!vg_ok(p_pool_reset(w_dev,s->pool,0),"upload pool reset") ||
        !vg_ok(vgResetDescriptorPool(w_dev,s->descriptors,0),"descriptor pool reset")) return 0;
    s->pos = s->sets = 0;
    if(++s->generation==0){memset(s->cache,0,sizeof s->cache);s->generation=1;}
    s->bound_pipeline=VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vg_cmd = s->cmd;
    vg_recording = vg_ok(p_begin(vg_cmd, &bi), "game command begin");
    if(vg_recording && vg_queries) {
        vgCmdResetQueryPool(vg_cmd,vg_queries,vg_slot_index*2,2);
        vgCmdWriteTimestamp(vg_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,vg_queries,vg_slot_index*2);
    }
    s->frame=vg_frames;s->end_frame=0;
    return vg_recording;
}

static void vg_end_pass(void)
{
    if (vg_rendering) { p_end_rendering(vg_cmd); vg_rendering = 0; }
}

static int vg_submit(int wait)
{
    if (!vg_recording) return !w_failed;
    vg_end_pass();
    VgSlot *s = &vg_slots[vg_slot_index];
    if(vg_queries) vgCmdWriteTimestamp(vg_cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,vg_queries,vg_slot_index*2+1);
    if (!vg_ok(p_end(vg_cmd), "game command end") ||
        !vg_ok(p_reset(w_dev, 1, &s->fence), "game fence reset")) return 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &vg_cmd };
    SDL_LockMutex(w_q_mu);
    VkResult r = p_submit(w_queue, 1, &si, s->fence);
    SDL_UnlockMutex(w_q_mu);
    if (!vg_ok(r, "game submit")) return 0;
    s->timed=vg_queries!=VK_NULL_HANDLE;
    vg_recording = 0;
    vg_slot_index = (vg_slot_index + 1) % VG_SLOTS;
    if (wait) return vg_ok(p_wait(w_dev, 1, &s->fence, VK_TRUE, UINT64_MAX), "game read wait");
    return 1;
}

static void vg_drain(void)
{
    if (!w_failed) vg_submit(0);
    else vg_recording=vg_rendering=0;
    SDL_LockMutex(w_q_mu);
    vg_ok(p_device_idle(w_dev), "game drain");
    SDL_UnlockMutex(w_q_mu);
    for(uint32_t i=0;i<VG_SLOTS;i++) vg_reap(&vg_slots[(vg_slot_index+i)%VG_SLOTS]);
}

static void *vg_upload(uint32_t bytes, uint32_t alignment, uint32_t *offset)
{
    if (!vg_begin() || bytes > VG_UPLOAD) return NULL;
    VgSlot *s = &vg_slots[vg_slot_index];
    uint32_t off = (s->pos + alignment - 1) & ~(alignment - 1);
    /* Do not retire a slot in the middle of a draw: vertex and uniform
     * uploads and descriptors must refer to the same buffer. */
    if (off > VG_UPLOAD - bytes) { vke("game upload arena exhausted"); w_failed = 1; return NULL; }
    *offset = off;
    s->pos = off + bytes;
    return s->map + off;
}

static uint32_t vg_texture_slot(void)
{
    for (uint32_t i = 1; i < VG_TEX; i++) if (!vg_tex[i].image) return i;
    vke("game texture table exhausted"); w_failed = 1; return 0;
}

static uint32_t vg_target_new(uint32_t w, uint32_t h)
{
    if (!vg_init() || !w || !h) return 0;
    uint32_t rt, tex = vg_texture_slot();
    for (rt = 1; rt < VG_RT; rt++) if (!vg_rt[rt].color) break;
    if (!tex || rt == VG_RT) return 0;
    VgTarget *t = &vg_rt[rt];
    t->color = tex; t->w = w; t->h = h;
    Nv2aSampler smp = { .min_linear = 1, .mag_linear = 1 };
    if (!vg_image(&vg_tex[tex], w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
        !vg_sampler(&vg_tex[tex], &smp) ||
        !vg_image(&t->depth, w, h, 1, vg_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return 0;
    vg_tex[tex].owner = (int)rt;
    vke("game target %u: %ux%u", rt, w, h);
    return rt;
}

static void vg_target_free(uint32_t rt)
{
    if (!rt || rt >= VG_RT || !vg_rt[rt].color) return;
    vg_drain();
    VgTarget *t = &vg_rt[rt];
    vg_image_free(&vg_tex[t->color]);
    if (t->snapshot) vg_image_free(&vg_tex[t->snapshot]);
    vg_image_free(&t->depth);
    memset(t, 0, sizeof *t);
    if (vg_current_rt == rt) vg_current_rt = 0;
}

static void vg_target_bind(uint32_t rt)
{
    if (rt != vg_current_rt) vg_end_pass();
    vg_current_rt = rt;
}

static int vg_pass(void)
{
    if (!vg_begin() || !vg_current_rt || vg_current_rt >= VG_RT) return 0;
    if (vg_rendering) return 1;
    VgTarget *t = &vg_rt[vg_current_rt];
    VgTexture *c = &vg_tex[t->color];
    int fresh = c->layout == VK_IMAGE_LAYOUT_UNDEFINED;
    int fresh_z = t->depth.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    vg_transition(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vg_transition(&t->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = c->view, .imageLayout = c->layout,
        .loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo za = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = t->depth.view, .imageLayout = t->depth.layout,
        .loadOp = fresh_z ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.depthStencil = { 1.0f, 0 } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { t->w, t->h } }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &ca,
        .pDepthAttachment = &za, .pStencilAttachment = &za };
    p_begin_rendering(vg_cmd, &ri);
    vg_rendering = 1;
    return 1;
}

static uint32_t vg_target_texture(uint32_t rt)
{
    if (!rt || rt >= VG_RT || !vg_rt[rt].color || !vg_begin()) return 0;
    uint32_t tex = vg_rt[rt].color;
    vg_transition(&vg_tex[tex], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    return tex;
}

static uint32_t vg_target_snapshot(uint32_t rt)
{
    if (!rt || rt >= VG_RT || !vg_rt[rt].color || !vg_begin()) return 0;
    VgTarget *t = &vg_rt[rt];
    if (!t->snapshot) {
        uint32_t tex = vg_texture_slot();
        Nv2aSampler smp = { .min_linear = 1, .mag_linear = 1 };
        if (!tex || !vg_image(&vg_tex[tex], t->w, t->h, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT) || !vg_sampler(&vg_tex[tex], &smp)) return 0;
        t->snapshot = tex;
        vg_tex[tex].owner = -1;
    }
    VgTexture *src = &vg_tex[t->color], *dst = &vg_tex[t->snapshot];
    vg_transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vg_transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { t->w, t->h, 1 } };
    vgCmdCopyImage(vg_cmd, src->image, src->layout, dst->image, dst->layout, 1, &cp);
    vg_transition(dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    return t->snapshot;
}

static uint32_t vg_tex_new(uint32_t w, uint32_t h, const void *rgba, const Nv2aSampler *s)
{
    if (!vg_init() || !rgba || !w || !h || !vg_begin()) return 0;
    uint32_t tex = vg_texture_slot(), levels = 1;
    if (!tex) return 0;
    if (s->mip) {
        uint32_t a = w, b = h;
        while (a > 1 || b > 1) { levels++; a = a > 1 ? a / 2 : 1; b = b > 1 ? b / 2 : 1; }
        if (s->levels > 1 && levels > s->levels) levels = s->levels;
    }
    uint64_t total=0;
    for (uint32_t l=0,a=w,b=h;l<levels;l++,a=a>1?a/2:1,b=b>1?b/2:1) total+=(uint64_t)a*b*4;
    if (total>VG_UPLOAD) { vke("texture upload exceeds arena: %ux%u",w,h);return 0; }
    if (vg_slots[vg_slot_index].pos>VG_UPLOAD-(uint32_t)total) {
        if (!vg_submit(0) || !vg_begin()) return 0;
    }
    VgTexture *t = &vg_tex[tex];
    if (!vg_image(t, w, h, levels, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT) || !vg_sampler(t, s)) return 0;
    VkBufferImageCopy copies[32];
    uint32_t lw = w, lh = h;
    const uint8_t *input = rgba;
    uint8_t *previous = NULL;
    uint32_t prev_w = 0, prev_h = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t off;
        uint8_t *out = vg_upload(lw * lh * 4, 4, &off);
        if (!out) return 0;
        if (!l || s->levels > 1) {
            memcpy(out, input, (size_t)lw * lh * 4);
            input += (size_t)lw * lh * 4;
        } else {
            for (uint32_t y = 0; y < lh; y++) for (uint32_t x = 0; x < lw; x++)
                for (uint32_t k = 0; k < 4; k++) {
                    uint32_t sum = 0;
                    for (uint32_t dy = 0; dy < 2; dy++) for (uint32_t dx = 0; dx < 2; dx++) {
                        uint32_t sx = 2*x+dx, sy = 2*y+dy;
                        if (sx >= prev_w) sx = prev_w - 1;
                        if (sy >= prev_h) sy = prev_h - 1;
                        sum += previous[((size_t)sy * prev_w + sx) * 4 + k];
                    }
                    out[((size_t)y * lw + x)*4+k] = (uint8_t)((sum+2)/4);
                }
        }
        copies[l] = (VkBufferImageCopy){ .bufferOffset = off,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, .imageExtent = { lw, lh, 1 } };
        previous = out; prev_w = lw; prev_h = lh;
        lw = lw > 1 ? lw/2 : 1; lh = lh > 1 ? lh/2 : 1;
    }
    vg_transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vgCmdCopyBufferToImage(vg_cmd, vg_slots[vg_slot_index].buffer, t->image, t->layout, levels, copies);
    vg_transition(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    if (vg_debug)
        vke("texture %u uploaded: %ux%u, first RGBA %u %u %u %u",tex,w,h,
            ((const uint8_t *)rgba)[0],((const uint8_t *)rgba)[1],((const uint8_t *)rgba)[2],((const uint8_t *)rgba)[3]);
    return tex;
}

static void vg_tex_free(uint32_t tex)
{
    if (!tex || tex >= VG_TEX || vg_tex[tex].owner) return;
    vg_drain();
    vg_image_free(&vg_tex[tex]);
    for (int i = 0; i < 4; i++) if (vg_bound_tex[i] == tex) vg_bound_tex[i] = 0;
}

/* GLSL uniforms are given explicit std140 offsets generated from the same
 * descriptor table that drives host writes. No reflection guesses or
 * independently maintained shader/C layouts. */
static void vg_uniform_layout(void)
{
    uint32_t pos = 0;
    for (int i = 0; i < NV2A_U_COUNT; i++) {
        const Nv2aUniformDesc *d = &nv2a_uniform_desc[i];
        if (d->type == NV2A_UT_SAMPLER) continue;
        vg_uniform_offset[i] = pos;
        vg_uniform_stride[i] = d->type == NV2A_UT_MAT2 ? 32 : 16;
        pos += vg_uniform_stride[i] * d->count;
    }
    vg_attr_offset = pos;
    vg_uniform_bytes = pos;
}

typedef struct { char *data; size_t used, cap; int failed; } VgString;
static void vg_text(VgString *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->data + s->used, s->cap - s->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= s->cap - s->used) { s->failed = 1; return; }
    s->used += (size_t)n;
}

static int vg_varying(const char *name)
{
    static const char *names[] = { "oD0", "oD1", "oT0", "oT1", "oT2", "oT3", "oFogC", "frag" };
    for (int i = 0; i < 8; i++) if (!strcmp(name, names[i])) return i == 7 ? 0 : i;
    return -1;
}

static char *vg_source(const char *src, int frag, VgProgram *program)
{
    VgString s = { .cap = strlen(src) + 32768 };
    s.data = calloc(1, s.cap);
    if (!s.data) return NULL;
    vg_text(&s, "#version 450\nlayout(std140,set=0,binding=0) uniform Nv2aU {\n");
    static const char *types[] = { "float", "vec2", "vec3", "vec4", "int", "mat2" };
    for (int i = 0; i < NV2A_U_COUNT; i++) {
        const Nv2aUniformDesc *d = &nv2a_uniform_desc[i];
        if (d->type == NV2A_UT_SAMPLER) continue;
        vg_text(&s, "layout(offset=%u) %s %s", vg_uniform_offset[i], types[d->type], d->name);
        if (d->count > 1) vg_text(&s, "[%u]", d->count);
        vg_text(&s, ";\n");
    }
    vg_text(&s, "};\n");
    /* A lexical pass at global scope: leave shader bodies and directives
     * intact, replace plain uniform declarations, locate interstage IO by
     * stable semantic names, and turn vertex inputs into fetch variables. */
    const char *p = strchr(src, '\n');
    p = p ? p + 1 : src;
    int depth = 0, line_start = 1;
    while (*p) {
        if (line_start && *p == '#') {
            const char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end-p+1) : strlen(p);
            vg_text(&s, "%.*s", (int)len, p); p += len; line_start = 1; continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *end = strchr(p, '\n');
            size_t len = end ? (size_t)(end-p+1) : strlen(p);
            vg_text(&s, "%.*s", (int)len, p); p += len; line_start = 1; continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p+2, "*/");
            if (!end) { s.failed = 1; break; }
            vg_text(&s, "%.*s", (int)(end+2-p), p); p = end+2; continue;
        }
        if (!depth && (isalpha((unsigned char)*p) || *p == '_')) {
            char token[64]; size_t n = 0;
            while (isalnum((unsigned char)p[n]) || p[n] == '_') n++;
            if (n >= sizeof token) { s.failed = 1; break; }
            memcpy(token, p, n); token[n] = 0;
            if (!strcmp(token, "layout")) {
                /* Vertex input layouts are replaced with shader pulling. */
                const char *close = strchr(p, ')');
                const char *q = close ? close+1 : p;
                while (isspace((unsigned char)*q)) q++;
                if (!frag && !strncmp(q, "in ", 3)) { p = q; continue; }
            }
            if (!strcmp(token, "uniform")) {
                char type[48], name[64];
                const char *end = strchr(p, ';');
                if (!end || sscanf(p, "uniform %47s %63[a-zA-Z0-9_]", type, name) != 2) { s.failed = 1; break; }
                int known = 0;
                for (int id = 0; id < NV2A_U_COUNT; id++) if (!strcmp(name, nv2a_uniform_desc[id].name)) {
                    program->uses[id] = 1; known = 1; break;
                }
                if (strstr(type, "sampler")) {
                    int unit = -1;
                    if (sscanf(name, "tex%d", &unit) != 1 || unit < 0 || unit > 3) { s.failed = 1; break; }
                    vg_text(&s, "layout(set=0,binding=%d) uniform %s %s;", unit+1, type, name);
                } else if (!known) {
                    vke("unknown shader uniform %s", name); s.failed = 1; break;
                }
                p = end+1; continue;
            }
            if (!strcmp(token, "in") || !strcmp(token, "out")) {
                char type[48], name[64];
                const char *end = strchr(p, ';');
                if (!end || sscanf(p+n, " %47s %63[a-zA-Z0-9_]", type, name) != 2) { s.failed = 1; break; }
                if (!frag && !strcmp(token, "in")) {
                    int attr = -1;
                    if (sscanf(name, "v%d", &attr) != 1 || attr < 0 || attr >= 16) { s.failed = 1; break; }
                    program->inputs |= (uint16_t)(1u << attr);
                    vg_text(&s, "layout(location=%d) in %s %s;",attr,type,name);
                } else {
                    int location = vg_varying(name);
                    if (location < 0) { vke("unknown shader varying %s", name); s.failed = 1; break; }
                    vg_text(&s, "layout(location=%d) %s %s %s;", location, token, type, name);
                }
                p = end+1; continue;
            }
            if (!strcmp(token, "main")) vg_text(&s, "_vg_main");
            else vg_text(&s, "%s", token);
            p += n; line_start = 0; continue;
        }
        if (*p == '{') depth++;
        if (*p == '}') depth--;
        vg_text(&s, "%c", *p);
        if (*p == '\n') line_start = 1;
        else if (!isspace((unsigned char)*p)) line_start = 0;
        p++;
    }
    vg_text(&s, "\nvoid main() {\n");
    vg_text(&s, "_vg_main();\n");
    if (!frag) vg_text(&s, "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n");
    vg_text(&s, "}\n");
    if (s.failed) { vke("shader conversion failed"); free(s.data); return NULL; }
    return s.data;
}

static uint32_t vg_prog_new(const char *vs, const char *fs, const char *what)
{
    if (!vg_init()) return 0;
    uint32_t id;
    for (id = 1; id < VG_PROG; id++) if (!vg_prog[id].uniforms) break;
    if (id == VG_PROG) return 0;
    VgProgram *p = &vg_prog[id];
    memset(p, 0, sizeof *p);
    char *v = vg_source(vs, 0, p), *f = vg_source(fs, 1, p);
    uint32_t *words = NULL; size_t nw = 0;
    if (!v || !f) { free(v); free(f); return 0; }
    if (id == 1 && vg_debug)
        fprintf(stderr,"[VKG] vertex source:\n%s\n[VKG] fragment source:\n%s\n",v,f);
    int ok = 1;
    for (int stage = 0; stage < 2 && ok; stage++) {
        const char *src = stage ? f : v;
        ok = nv2a_vk_glsl_to_spirv(src, stage, &words, &nw);
        if (ok) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = nw * sizeof(uint32_t), .pCode = words };
            ok = vg_ok(p_shader_new(w_dev, &ci, NULL, stage ? &p->fs : &p->vs), "game shader module");
            free(words); words = NULL;
        } else {
            vke("%s %s shader failed", what, stage ? "fragment" : "vertex");
            fprintf(stderr, "[VKG] failed source:\n%s\n", src);
        }
    }
    free(v); free(f);
    if (!ok) {
        if (p->vs) p_shader_free(w_dev, p->vs, NULL);
        if (p->fs) p_shader_free(w_dev, p->fs, NULL);
        memset(p, 0, sizeof *p); return 0;
    }
    p->uniforms = calloc(1, vg_uniform_bytes);
    if (!p->uniforms) { p_shader_free(w_dev,p->vs,NULL); p_shader_free(w_dev,p->fs,NULL); memset(p,0,sizeof *p); return 0; }
    vke("game program %u: %s (inputs %04x)", id, what, p->inputs);
    return id;
}

static void vg_prog_free(uint32_t id)
{
    if (!id || id >= VG_PROG || !vg_prog[id].uniforms) return;
    vg_drain();
    for (uint32_t i = 0; i < vg_npipe; i++) if (vg_pipes[i].prog == id) {
        p_pipe_free(w_dev, vg_pipes[i].pipeline, NULL);
        vg_pipes[i].pipeline = VK_NULL_HANDLE; vg_pipes[i].prog = 0;
    }
    p_shader_free(w_dev, vg_prog[id].vs, NULL); p_shader_free(w_dev, vg_prog[id].fs, NULL);
    free(vg_prog[id].uniforms); memset(&vg_prog[id],0,sizeof vg_prog[id]);
    if (vg_current_prog == id) vg_current_prog = 0;
}

static int vg_prog_uses(uint32_t p, int id)
{ return p && p < VG_PROG && id >= 0 && id < NV2A_U_COUNT && vg_prog[p].uses[id]; }
static void vg_prog_use(uint32_t p) { vg_current_prog = p; }
static void vg_uniform_f(int id, const float *v, int n)
{
    if (!vg_current_prog || vg_current_prog >= VG_PROG || !vg_prog[vg_current_prog].uniforms ||
        id < 0 || id >= NV2A_U_COUNT || n < 1) return;
    const Nv2aUniformDesc *d = &nv2a_uniform_desc[id];
    if (d->type == NV2A_UT_SAMPLER || d->type == NV2A_UT_INT) return;
    if (n > d->count) n = d->count;
    static const unsigned sizes[] = { 1, 2, 3, 4, 1, 4 };
    uint8_t *dst = vg_prog[vg_current_prog].uniforms + vg_uniform_offset[id];
    vg_uniform_calls++;
    if (vg_uniform_stride[id] == sizes[d->type]*4u) {
        size_t bytes=(size_t)n*vg_uniform_stride[id];
        if (vg_uniform_dedup && !memcmp(dst,v,bytes)) {vg_uniform_unchanged++;return;}
        memcpy(dst,v,bytes);
        vg_prog[vg_current_prog].revision++;
        return;
    }
    int changed=!vg_uniform_dedup;
    for (int i = 0; i < n; i++) {
        if (d->type == NV2A_UT_MAT2) {
            if(!vg_uniform_dedup || memcmp(dst,v,8) || memcmp(dst+16,v+2,8)) {
                memcpy(dst,v,8);memcpy(dst+16,v+2,8);changed=1;
            }
        } else if(!vg_uniform_dedup || memcmp(dst,v,sizes[d->type]*4)) {
            memcpy(dst,v,sizes[d->type]*4);changed=1;
        }
        dst += vg_uniform_stride[id]; v += sizes[d->type];
    }
    if(changed) vg_prog[vg_current_prog].revision++;else vg_uniform_unchanged++;
}
static void vg_uniform_i(int id, int v)
{
    if (!vg_current_prog || vg_current_prog >= VG_PROG || !vg_prog[vg_current_prog].uniforms ||
        id < 0 || id >= NV2A_U_COUNT || nv2a_uniform_desc[id].type != NV2A_UT_INT) return;
    uint8_t *dst=vg_prog[vg_current_prog].uniforms + vg_uniform_offset[id];
    vg_uniform_calls++;
    if(vg_uniform_dedup && !memcmp(dst,&v,4)){vg_uniform_unchanged++;return;}
    memcpy(dst, &v, 4);
    vg_prog[vg_current_prog].revision++;
}
static int vg_uniform_f_part(int id,int first,const float *v,int n)
{
    if(id<0 || id>=NV2A_U_COUNT || nv2a_uniform_desc[id].type!=NV2A_UT_VEC4) return 0;
    if(!vg_current_prog || vg_current_prog>=VG_PROG || !vg_prog[vg_current_prog].uniforms) return 0;
    if(first<0 || first>=nv2a_uniform_desc[id].count || n<=0) return 1;
    if(n>nv2a_uniform_desc[id].count-first)n=nv2a_uniform_desc[id].count-first;
    uint8_t *dst=vg_prog[vg_current_prog].uniforms+vg_uniform_offset[id]+(uint32_t)first*16;
    vg_uniform_calls++;
    if(vg_uniform_dedup && !memcmp(dst,v,(size_t)n*16)){vg_uniform_unchanged++;return 1;}
    memcpy(dst,v,(size_t)n*16);
    vg_prog[vg_current_prog].revision++;return 1;
}
static void vg_texture(int unit, uint32_t tex)
{ if (unit >= 0 && unit < 4) vg_bound_tex[unit] = tex; }
static void vg_set_state(const Nv2aRenderState *rs) { vg_state = *rs; }
static void vg_invalidate(void) { }

static void vg_vertices(const float *v, uint32_t n, uint32_t stride,
                        const int at[16], uint32_t mask, const float value[16][4])
{
    vg_nverts = 0;
    vg_cached_vb=0;
    if (!vg_begin() || !n || !stride || n > VG_UPLOAD / 4 / stride) return;
    /* The front end supplies vertices BEFORE it creates/binds textures.
     * Texture uploads may rotate an arena slot; defer our copy until draw,
     * when all of that resource work is complete. The pointer is valid until
     * the next vertices call, as required by this immediate draw interface. */
    vg_vertex_data=v;
    vg_vertex_bytes = n * stride * 4;
    vg_attr_stride = stride; vg_attr_mask = 0;
    for (int i = 0; i < 16; i++) {
        vg_attr_at[i] = at[i] >= 0 ? (uint32_t)at[i] : 0;
        if (at[i] >= 0) vg_attr_mask |= 1u << i;
        if (mask & (1u << i)) memcpy(vg_attr_value[i], value[i], 16);
    }
    vg_nverts = n;
}

static uint32_t vg_vbuf_new(int stream)
{
    for(uint32_t i=1;i<VG_VBUFS;i++) if(!vg_vbufs[i].used) {
        vg_vbufs[i].used=1;vg_vbufs[i].stream=stream!=0;return i;
    }
    return 0;
}

static void vg_vbuf_free(uint32_t id)
{
    if(!id || id>=VG_VBUFS || !vg_vbufs[id].used) return;
    VgVertexBuffer *b=&vg_vbufs[id];
    if(b->size) vg_retire_range(b->off,b->size);
    free(b->vertices);free(b->indices);memset(b,0,sizeof *b);
    if(vg_cached_vb==id){vg_cached_vb=0;vg_nverts=0;}
}

static void vg_vbuf_fill(uint32_t id,const float *v,uint32_t vbytes,const void *idx,uint32_t ibytes)
{
    if(!id || id>=VG_VBUFS || !vg_vbufs[id].used) return;
    VgVertexBuffer *b=&vg_vbufs[id];
    if(v && vbytes) {
        if(b->stream) {
            uint8_t *data=realloc(b->vertices,vbytes);
            if(!data){w_failed=1;return;}b->vertices=data;memcpy(data,v,vbytes);
        } else {
            uint32_t off,size;
            if(!vg_range_alloc(vbytes,&off,&size)) return;
            memcpy(vg_static_map+off,v,vbytes);
            if(b->size) vg_retire_range(b->off,b->size);
            b->off=off;b->size=size;
        }
        b->vbytes=vbytes;
    }
    if(idx && ibytes) {
        uint8_t *data=realloc(b->indices,ibytes);
        if(!data){w_failed=1;return;}b->indices=data;memcpy(data,idx,ibytes);b->ibytes=ibytes;
    }
}

static void vg_vbuf_bind(uint32_t id,uint32_t stride,const int at[16],uint32_t mask,const float value[16][4])
{
    if(!id || id>=VG_VBUFS || !vg_vbufs[id].used || !stride){vg_nverts=0;return;}
    VgVertexBuffer *b=&vg_vbufs[id];
    vg_vertices(b->stream?(const float *)b->vertices:(const float *)(vg_static_map+b->off),
        b->vbytes/(stride*4),stride,at,mask,value);
    vg_cached_vb=id;
}

static VkCompareOp vg_compare(uint32_t v)
{ return v >= 0x200 && v <= 0x207 ? (VkCompareOp)(v - 0x200) : VK_COMPARE_OP_ALWAYS; }
static VkBlendFactor vg_blend(uint32_t v)
{
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR; case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA; case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA; case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR; case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkStencilOp vg_stencil(uint32_t v)
{
    switch (v) {
    case 0: return VK_STENCIL_OP_ZERO; case 0x1e01: return VK_STENCIL_OP_REPLACE;
    case 0x1e02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 0x1e03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150a: return VK_STENCIL_OP_INVERT; case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}
static VkPrimitiveTopology vg_topology(int p)
{
    switch (p) {
    case NV2A_PRIM_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NV2A_PRIM_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NV2A_PRIM_LINE_LOOP: case NV2A_PRIM_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NV2A_PRIM_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NV2A_PRIM_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static uint32_t vg_pipeline_bucket(const VgPipeline *key)
{
    /* Four independent lanes shorten the dependency chain. Hash the exact
     * bytes compared below, including normalized unused state. */
    const uint8_t *p = (const uint8_t *)key;
    size_t n = offsetof(VgPipeline, pipeline);
    uint32_t h[4] = {0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du, 0x27d4eb2fu};
    while (n >= 16) {
        for (int i = 0; i < 4; i++) {
            uint32_t word; memcpy(&word, p + 4*i, 4);
            h[i] = (h[i] ^ word) * 0x9e3779b1u;
        }
        p += 16; n -= 16;
    }
    uint32_t hash = h[0] ^ (h[1] << 7 | h[1] >> 25) ^
                    (h[2] << 13 | h[2] >> 19) ^ (h[3] << 19 | h[3] >> 13);
    while (n--) hash = (hash ^ *p++) * 16777619u;
    hash ^= hash >> 16; hash *= 0x85ebca6bu; hash ^= hash >> 13;
    return hash & (VG_PIPE_BUCKETS - 1);
}

static VkPipeline vg_pipeline_find(const VgPipeline *key, uint32_t bucket)
{
    for (uint32_t entry = vg_pipe_head[bucket]; entry; entry = vg_pipe_next[entry - 1]) {
        const VgPipeline *p = &vg_pipes[entry - 1];
        if (p->pipeline && !memcmp(p, key, offsetof(VgPipeline, pipeline))) return p->pipeline;
    }
    return VK_NULL_HANDLE;
}

static void vg_pipeline_remember(const VgPipeline *key, uint32_t bucket)
{
    vg_pipes[vg_npipe] = *key;
    vg_pipe_next[vg_npipe] = vg_pipe_head[bucket];
    vg_pipe_head[bucket] = ++vg_npipe;
}

static VkPipeline vg_pipeline(int prim)
{
    /* Scissor is dynamic. Zero structure padding and irrelevant fields so
     * semantically equal states have the same cache key. */
    VgPipeline key;
    memset(&key, 0, sizeof key);
    key.prog = vg_current_prog; key.prim = (uint32_t)prim;
    key.stride=vg_attr_stride*4;
    for(int i=0;i<16;i++) if(vg_prog[vg_current_prog].inputs&(1u<<i))
        key.attr[i]=(vg_attr_mask&(1u<<i))?vg_attr_at[i]:UINT32_MAX;
    Nv2aRenderState *r = &key.rs;
    r->depth_test = vg_state.depth_test; r->depth_write = vg_state.depth_write; r->depth_func = vg_state.depth_func;
    r->blend_enable = vg_state.blend_enable; r->blend_src = vg_state.blend_src; r->blend_dst = vg_state.blend_dst;
    r->cull_enable = vg_state.cull_enable; r->cull_face = vg_state.cull_face; r->front_face = vg_state.front_face;
    r->color_mask = vg_state.color_mask; r->zclamp = vg_state.zclamp; r->w_buffer = vg_state.w_buffer;
    r->stencil_test = vg_state.stencil_test; r->stencil_func = vg_state.stencil_func;
    r->stencil_ref = vg_state.stencil_ref; r->stencil_rmask = vg_state.stencil_rmask; r->stencil_wmask = vg_state.stencil_wmask;
    memcpy(r->stencil_op, vg_state.stencil_op, sizeof r->stencil_op);
    r->poly_offset = vg_state.poly_offset; r->poly_offset_factor = vg_state.poly_offset_factor; r->poly_offset_units = vg_state.poly_offset_units;
    if (vg_force_nocull) r->cull_enable=0;
    if (vg_force_nodepth) r->depth_test=r->depth_write=0;
    uint32_t bucket = vg_pipeline_bucket(&key);
    VkPipeline found = vg_pipeline_find(&key, bucket);
    if (found) return found;
    if (vg_npipe == VG_PIPES) { vke("game pipeline cache exhausted"); return VK_NULL_HANDLE; }
    VgProgram *p = &vg_prog[vg_current_prog];
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = p->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = p->fs, .pName = "main" } };
    VkVertexInputBindingDescription bindings[2]={{0,key.stride,VK_VERTEX_INPUT_RATE_VERTEX},{1,0,VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attributes[16];uint32_t na=0;
    for(uint32_t i=0;i<16;i++) if(p->inputs&(1u<<i)) {
        int constant=key.attr[i]==UINT32_MAX;
        attributes[na++]=(VkVertexInputAttributeDescription){i,constant?1u:0u,VK_FORMAT_R32G32B32A32_SFLOAT,constant?16*i:key.attr[i]*4};
    }
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=2,.pVertexBindingDescriptions=bindings,
        .vertexAttributeDescriptionCount=na,.pVertexAttributeDescriptions=attributes };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = vg_topology(prim) };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkCullModeFlags cull = !r->cull_enable ? VK_CULL_MODE_NONE : r->cull_face == 0x404 ? VK_CULL_MODE_FRONT_BIT :
        r->cull_face == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = vg_depth_clamp && ((r->zclamp & 0xf0) || r->w_buffer),
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = cull,
        .frontFace = r->front_face == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = r->poly_offset, .depthBiasConstantFactor = r->poly_offset_units,
        .depthBiasSlopeFactor = r->poly_offset_factor, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState stencil = { .failOp = vg_stencil(r->stencil_op[0]), .depthFailOp = vg_stencil(r->stencil_op[1]),
        .passOp = vg_stencil(r->stencil_op[2]), .compareOp = vg_compare(r->stencil_func),
        .compareMask = r->stencil_rmask, .writeMask = r->stencil_wmask, .reference = r->stencil_ref };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = r->depth_test, .depthWriteEnable = r->depth_write, .depthCompareOp = vg_compare(r->depth_func),
        .stencilTestEnable = r->stencil_test, .front = stencil, .back = stencil };
    uint32_t cm = r->color_mask ? r->color_mask : 0x01010101u;
    VkPipelineColorBlendAttachmentState ca = { .blendEnable = r->blend_enable,
        .srcColorBlendFactor = vg_blend(r->blend_src), .dstColorBlendFactor = vg_blend(r->blend_dst),
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = vg_blend(r->blend_src),
        .dstAlphaBlendFactor = vg_blend(r->blend_dst), .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = ((cm & 0x10000) ? VK_COLOR_COMPONENT_R_BIT : 0) |
            ((cm & 0x100) ? VK_COLOR_COMPONENT_G_BIT : 0) | ((cm & 1) ? VK_COLOR_COMPONENT_B_BIT : 0) |
            ((cm & 0x1000000) ? VK_COLOR_COMPONENT_A_BIT : 0) };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ca };
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic };
    int window=vg_current_prog==vg_window_program;
    VkFormat color = window?w_format:VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color,
        .depthAttachmentFormat = window?VK_FORMAT_UNDEFINED:vg_depth_format,
        .stencilAttachmentFormat = window?VK_FORMAT_UNDEFINED:vg_depth_format };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &ri, .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = vg_pipeline_layout };
    if (!vg_ok(p_pipe_new(w_dev, vg_pipeline_cache, 1, &ci, NULL, &key.pipeline), "game pipeline")) return VK_NULL_HANDLE;
    vg_pipeline_remember(&key, bucket);
    return key.pipeline;
}

static VkDescriptorSet vg_descriptors(VgSlot *s,VgTexture *textures[4])
{
    uint32_t ids[4],hash=2166136261u;
    for(int i=0;i<4;i++){ids[i]=(uint32_t)(textures[i]-vg_tex);hash=(hash^ids[i])*16777619u;}
    uint32_t slot=hash&(VG_DESCRIPTOR_HASH-1);
    while(s->cache[slot].generation==s->generation) {
        if(!memcmp(s->cache[slot].tex,ids,sizeof ids)) return s->cache[slot].set;
        slot=(slot+1)&(VG_DESCRIPTOR_HASH-1);
    }
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=s->descriptors,.descriptorSetCount=1,.pSetLayouts=&vg_set_layout};
    VkDescriptorSet set;
    if(!vg_ok(vgAllocateDescriptorSets(w_dev,&ai,&set),"cached draw descriptors")) return VK_NULL_HANDLE;
    s->sets++;
    VkDescriptorBufferInfo ub={s->buffer,0,vg_uniform_bytes};
    VkDescriptorImageInfo images[4];
    VkWriteDescriptorSet writes[5]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
        .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&ub}};
    for(int i=0;i<4;i++) {
        images[i]=(VkDescriptorImageInfo){textures[i]->sampler,textures[i]->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i+1]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=(uint32_t)i+1,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    }
    vgUpdateDescriptorSets(w_dev,5,writes,0,NULL);
    s->cache[slot].generation=s->generation;s->cache[slot].set=set;memcpy(s->cache[slot].tex,ids,sizeof ids);
    return set;
}

static void vg_draw_impl(int prim, uint32_t count, int indexed, int idx32)
{
    if (!vg_nverts || !vg_current_prog || vg_current_prog >= VG_PROG || !vg_prog[vg_current_prog].uniforms || !vg_begin()) return;
    if (!indexed && count > vg_nverts) count = vg_nverts;
    VgVertexBuffer *kept=vg_cached_vb?&vg_vbufs[vg_cached_vb]:NULL;
    uint32_t index_bytes=indexed?count*(idx32?4u:2u):0,index_off=0;
    if(indexed && (!kept || !kept->indices || index_bytes>kept->ibytes)) return;
    VgTexture *textures[4];
    for (int i = 0; i < 4; i++) {
        uint32_t id = vg_bound_tex[i];
        if (!id || id >= VG_TEX || !vg_tex[id].image) id = vg_white;
        textures[i] = &vg_tex[id];
        if (textures[i]->owner == (int)vg_current_rt) {
            vke("refusing render-target feedback without snapshot"); return;
        }
        vg_transition(textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    int persistent=kept && !kept->stream;
    uint32_t need=(persistent?0:vg_vertex_bytes)+index_bytes+vg_uniform_bytes+256+3*vg_alignment;
    VgSlot *s = &vg_slots[vg_slot_index];
    if (need>VG_UPLOAD) {vke("draw upload exceeds arena");w_failed=1;return;}
    if (s->pos>VG_UPLOAD-need || s->sets>=VG_SETS) {
        if (!vg_submit(0) || !vg_begin()) return;
        s=&vg_slots[vg_slot_index];
    }
    VkBuffer vertex_buffer=s->buffer;
    if(persistent) {vertex_buffer=vg_static_buffer;vg_vertex_offset=kept->off;}
    else {
        void *vertices=vg_upload(vg_vertex_bytes,vg_alignment,&vg_vertex_offset);
        if (!vertices) return;
        memcpy(vertices,vg_vertex_data,vg_vertex_bytes);
    }
    if(indexed) {
        void *indices=vg_upload(index_bytes,4,&index_off);
        if(!indices) return;memcpy(indices,kept->indices,index_bytes);
    }
    VkPipeline pipeline = vg_pipeline(prim);
    if (!pipeline || !vg_pass()) return;
    VgProgram *program=&vg_prog[vg_current_prog];
    uint32_t off=program->cache_offset[vg_slot_index];
    uint8_t *uniforms;
    if(program->cache_generation[vg_slot_index]==s->generation && program->cache_revision[vg_slot_index]==program->revision)
        uniforms=s->map+off;
    else {
        uniforms=vg_upload(vg_uniform_bytes,vg_alignment,&off);
        if(!uniforms) return;
        memcpy(uniforms,program->uniforms,vg_uniform_bytes);
        vg_uniform_uploads++;vg_uniform_upload_bytes+=vg_uniform_bytes;
        program->cache_generation[vg_slot_index]=s->generation;
        program->cache_revision[vg_slot_index]=program->revision;
        program->cache_offset[vg_slot_index]=off;
    }
    uint32_t attr_off;void *attrs=vg_upload(256,16,&attr_off);
    if(!attrs) return;memcpy(attrs,vg_attr_value,256);
    VkDescriptorSet set=vg_descriptors(s,textures);
    if(!set) return;
    if(s->bound_pipeline!=pipeline){p_bind_pipeline(vg_cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);s->bound_pipeline=pipeline;}
    vgCmdBindDescriptorSets(vg_cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,vg_pipeline_layout,0,1,&set,1,&off);
    VkBuffer buffers[]={vertex_buffer,s->buffer};VkDeviceSize offsets[]={vg_vertex_offset,attr_off};
    vgCmdBindVertexBuffers(vg_cmd,0,2,buffers,offsets);
    VgTarget *t = &vg_rt[vg_current_rt];
    VkViewport vp = { .x = 0, .y = (float)t->h, .width = (float)t->w,
        .height = -(float)t->h, .minDepth = 0, .maxDepth = 1 };
    VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    if (vg_state.scissor_enable) {
        uint32_t x0=vg_state.scissor[0], y0=vg_state.scissor[1], x1=vg_state.scissor[2], y1=vg_state.scissor[3];
        if (x0 > t->w) x0=t->w; if (y0 > t->h) y0=t->h;
        if (x1 > t->w) x1=t->w; if (y1 > t->h) y1=t->h;
        if (x1 <= x0 || y1 <= y0) return;
        sc = (VkRect2D){ { (int32_t)x0, (int32_t)y0 }, { x1-x0, y1-y0 } };
    }
    p_viewport(vg_cmd,0,1,&vp); p_scissor(vg_cmd,0,1,&sc);
    if(indexed) {
        vgCmdBindIndexBuffer(vg_cmd,s->buffer,index_off,idx32?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16);
        vgCmdDrawIndexed(vg_cmd,count,1,0,0,0);
    } else p_cmd_draw(vg_cmd,count,1,0,0);
    vg_draws++;
    if (vg_draws <= 8 && vg_debug) {
        const float *v=(const float *)((persistent?vg_static_map:s->map)+vg_vertex_offset);
        vke("draw %u: rt %u prog %u n %u stride %u mask %04x v0 %.3f %.3f %.3f %.3f tex %u cull %d face %x depth %d/%x cm %x",
            vg_draws,vg_current_rt,vg_current_prog,count,vg_attr_stride,vg_attr_mask,
            v[0],v[1],v[2],v[3],vg_bound_tex[0],vg_state.cull_enable,vg_state.front_face,vg_state.depth_test,vg_state.depth_func,vg_state.color_mask);
        const float *c=(const float *)uniforms;
        const float *vp=(const float *)(uniforms+vg_uniform_offset[NV2A_U_VPSURFACE]);
        int pos;memcpy(&pos,uniforms+vg_uniform_offset[NV2A_U_POSMODE],4);
        vke("draw constants: c0 %.3f %.3f %.3f %.3f c1 %.3f %.3f %.3f %.3f surface %.1f %.1f mode %d",
            c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7],vp[0],vp[1],pos);
    }
}
static void vg_draw(int prim,uint32_t count) {vg_draw_impl(prim,count,0,0);}
static void vg_draw_indexed(int prim,uint32_t count,int idx32) {vg_draw_impl(prim,count,1,idx32);}

static void vg_clear(unsigned mask, const float rgba[4], float depth, uint32_t stencil, const uint32_t rect[4])
{
    if (!vg_pass()) return;
    VgTarget *t = &vg_rt[vg_current_rt];
    VkClearAttachment a[2]; uint32_t n=0;
    if (mask & 1) {
        a[n] = (VkClearAttachment){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0 };
        memcpy(a[n++].clearValue.color.float32,rgba,16);
    }
    if (mask & 6) a[n++] = (VkClearAttachment){
        .aspectMask = ((mask & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((mask & 4) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
        .clearValue.depthStencil = { depth, stencil } };
    VkClearRect r = { .rect = { {0,0}, {t->w,t->h} }, .baseArrayLayer = 0, .layerCount = 1 };
    if (rect) {
        uint32_t x0=rect[0],y0=rect[1],x1=rect[2],y1=rect[3];
        if (x0 > t->w) x0=t->w; if (y0 > t->h) y0=t->h;
        if (x1 > t->w) x1=t->w; if (y1 > t->h) y1=t->h;
        if (x1 <= x0 || y1 <= y0) return;
        r.rect = (VkRect2D){ { (int32_t)x0,(int32_t)y0 }, { x1-x0,y1-y0 } };
    }
    if (n) vgCmdClearAttachments(vg_cmd,n,a,1,&r);
}

static int vg_read_texture(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int bgra, void *out)
{
    if (!id || id >= VG_TEX || !vg_tex[id].image || !out || !w || !h || !vg_begin()) return 0;
    VgTexture *t = &vg_tex[id];
    if (x > t->w || y > t->h || w > t->w-x || h > t->h-y) return 0;
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; uint8_t *map = NULL;
    if (!vg_buffer((VkDeviceSize)w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&buffer,&memory,&map)) return 0;
    vg_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy cp = { .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageOffset = { (int32_t)x,(int32_t)(t->h-y-h),0 }, .imageExtent = {w,h,1} };
    p_copy_to_buf(vg_cmd,t->image,t->layout,buffer,1,&cp);
    int ok = vg_submit(1);
    if (ok) for (uint32_t row=0;row<h;row++) {
        const uint8_t *s=map+(size_t)(h-1-row)*w*4;
        uint8_t *d=(uint8_t *)out+(size_t)row*w*4;
        if (!bgra) memcpy(d,s,(size_t)w*4);
        else for (uint32_t i=0;i<w;i++) { d[4*i]=s[4*i+2];d[4*i+1]=s[4*i+1];d[4*i+2]=s[4*i];d[4*i+3]=s[4*i+3]; }
    }
    if (!ok) vg_drain();
    p_unmap(w_dev,memory); p_buf_free(w_dev,buffer,NULL);p_mem_free(w_dev,memory,NULL);
    return ok;
}
static int vg_target_read(uint32_t rt,uint32_t x,uint32_t y,uint32_t w,uint32_t h,int bgra,void *out)
{ return rt && rt<VG_RT ? vg_read_texture(vg_rt[rt].color,x,y,w,h,bgra,out) : 0; }
static int vg_tex_read(uint32_t tex,uint32_t w,uint32_t h,void *out)
{ return vg_read_texture(tex,0,0,w,h,1,out); }
static int vg_depth_read(uint32_t rt,uint32_t w,uint32_t h,float *out)
{ (void)rt;(void)w;(void)h;(void)out;return 0; }

static void vg_present_legacy(uint32_t rt, uint32_t pw, uint32_t ph)
{
    (void)pw; (void)ph;
    if (!rt || rt >= VG_RT || !vg_rt[rt].color || !vg_begin() || w_failed) return;
    VgTexture *src = &vg_tex[vg_rt[rt].color];
    if (vg_debug && vg_frames == 90) {
        for (uint32_t i=1;i<VG_RT;i++) if (vg_rt[i].color) {
            uint8_t pixel[4]={0};
            vg_target_read(i,vg_rt[i].w/2,vg_rt[i].h/2,1,1,0,pixel);
            vke("target %u center %u %u %u %u; showing target %u",i,pixel[0],pixel[1],pixel[2],pixel[3],rt);
        }
        if (!vg_begin()) return;
    }
    vg_transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
    /* End game rendering before using the window's command buffer. Queue
     * order and barriers connect the submissions; no CPU pixel read-back. */
    vg_slots[vg_slot_index].end_frame=1;
    if (!vg_submit(0)) return;
    /* GL deliberately leaves the loader's logical flips uncapped until the
     * first 3D scene. FIFO must not turn that fast boot into 60 logo frames
     * per second. Keep rendering the logical flips, but show only the most
     * recent one at display cadence during boot. Gameplay stays paced by
     * the front end once a scene has ever appeared. */
    uint32_t draws=vg_draws-vg_previous_draws;vg_previous_draws=vg_draws;
    if(draws>=100) vg_seen_scene=1;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    double seconds=(double)now.tv_sec+(double)now.tv_nsec/1e9;
    if(!vg_seen_scene && seconds-vg_last_window<1.0/60.0){vg_frames++;return;}
    if (!vg_ok(p_wait(w_dev,1,&w_fence,VK_TRUE,UINT64_MAX),"present buffer wait")) return;
    uint32_t idx=0;
    VkResult r=p_acquire(w_dev,w_sc,UINT64_MAX,w_acq,VK_NULL_HANDLE,&idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vg_ok(r,"game acquire");return; }
    if (idx>=w_nimg || !vg_ok(p_pool_reset(w_dev,w_pool,0),"present reset")) return;
    VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (!vg_ok(p_begin(w_cmd,&bi),"present begin")) return;
    VkImageMemoryBarrier b={ .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=w_layout[idx],.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=w_img[idx],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1} };
    p_barrier(w_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    VkClearColorValue black={ .float32={0,0,0,1} };
    p_clear(w_cmd,w_img[idx],b.newLayout,&black,1,&b.subresourceRange);
    /* Preserve the title's aspect ratio. Texture row zero is the top. */
    uint32_t dw=w_extent.width,dh=(uint32_t)((uint64_t)dw*src->h/src->w);
    if (dh>w_extent.height) {dh=w_extent.height;dw=(uint32_t)((uint64_t)dh*src->w/src->h);}
    int32_t x=(int32_t)(w_extent.width-dw)/2,y=(int32_t)(w_extent.height-dh)/2;
    VkImageBlit blit={ .srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{0,0,0},{(int32_t)src->w,(int32_t)src->h,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{x,y,0},{x+(int32_t)dw,y+(int32_t)dh,1}} };
    /* Serialize the clear and blit writes to the swapchain image. */
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout=b.newLayout;
    p_barrier(w_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&b);
    vgCmdBlitImage(w_cmd,src->image,src->layout,w_img[idx],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=0;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    p_barrier(w_cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&b);
    if (!vg_ok(p_end(w_cmd),"present end") || !vg_ok(p_reset(w_dev,1,&w_fence),"present fence reset")) return;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si={ .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&w_acq,
        .pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&w_cmd,
        .signalSemaphoreCount=1,.pSignalSemaphores=&w_done[idx] };
    VkPresentInfoKHR pi={ .sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&w_done[idx],
        .swapchainCount=1,.pSwapchains=&w_sc,.pImageIndices=&idx };
    SDL_LockMutex(w_q_mu);
    r=p_submit(w_queue,1,&si,w_fence);
    if (r==VK_SUCCESS) {w_layout[idx]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;r=p_present(w_queue,&pi);}
    SDL_UnlockMutex(w_q_mu);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {vg_ok(r,"game present");return;}
    vg_last_window=seconds;
    vg_frames++;vg_window_frames++;
    if (vg_window_frames==1 || vg_window_frames%300==0)
        vke("game flip %u shown (%u window presents): %u draws, %u cached pipelines",vg_frames,vg_window_frames,vg_draws,vg_npipe);
}

/* Window work is isolated from the guest/pushbuffer thread. A front image
 * is immutable while queued or used by the presenter. Reuse after the read
 * is submitted is safe through same-queue ordering and image barriers. */
static int vg_window_init(void)
{
    static const char *vs="#version 450\nout vec4 oT0;\nuniform vec4 vpScale;\nuniform vec4 zRev;\n"
        /* A scaled oversized fullscreen triangle does not bound a letterbox:
         * its diagonal becomes visible and UVs beyond 1 smear the last texel.
         * Use a real rectangle before aspect scaling and pre-rotation. */
        "void main(){const vec2 q[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));"
        "vec2 uv=q[gl_VertexIndex];"
        "oT0=vec4(uv,0,1);vec2 p=(uv*2.0-1.0)*vpScale.xy;"
        "gl_Position=vec4(p.x*zRev.x+p.y*zRev.y,p.x*zRev.z+p.y*zRev.w,0,1);}\n";
    static const char *fs="#version 450\nin vec4 oT0;\nuniform sampler2D tex0;\nout vec4 frag;\n"
        "void main(){frag=vec4(texture(tex0,oT0.xy).rgb,1.0);}\n";
    vg_window_program=vg_prog_new(vs,fs,"pre-rotated window presenter");
    if(!vg_window_program)return 0;
    vg_current_prog=vg_window_program;memset(&vg_state,0,sizeof vg_state);vg_attr_stride=vg_attr_mask=0;
    vg_window_pipeline=vg_pipeline(NV2A_PRIM_TRIANGLES);vg_current_prog=0;
    if(!vg_window_pipeline)return 0;
    if(!vg_buffer(vg_uniform_bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        &vg_window_buffer,&vg_window_memory,&vg_window_map))return 0;
    memset(vg_window_map,0,vg_uniform_bytes);
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,3},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,12}};
    VkDescriptorPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=3,.poolSizeCount=2,.pPoolSizes=sizes};
    if(!vg_ok(vgCreateDescriptorPool(w_dev,&pci,NULL,&vg_window_descriptors),"window descriptors"))return 0;
    VkDescriptorSetLayout layouts[]={vg_set_layout,vg_set_layout,vg_set_layout};VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=vg_window_descriptors,.descriptorSetCount=3,.pSetLayouts=layouts};
    if(!vg_ok(vgAllocateDescriptorSets(w_dev,&ai,sets),"window descriptor sets"))return 0;
    for(int i=0;i<3;i++)vg_display[i].set=sets[i];
    for(uint32_t i=0;i<w_nimg;i++) {
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=w_img[i],
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=w_format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if(!vg_ok(p_view_new(w_dev,&vi,NULL,&w_view[i]),"window image view"))return 0;
    }
    vke("asynchronous pre-rotated presenter: window %ux%u, images %ux%u, transform %u",
        w_window_extent.width,w_window_extent.height,w_extent.width,w_extent.height,(unsigned)w_transform);
    return 1;
}

static int vg_display_target(int index,uint32_t w,uint32_t h)
{
    VgDisplay *d=&vg_display[index];VgTexture *t=&d->texture;
    if(t->image && t->w==w && t->h==h)return 1;
    if(t->image){vg_drain();vg_image_free(t);}
    Nv2aSampler smp={.min_linear=1,.mag_linear=1};
    if(!vg_image(t,w,h,1,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT) || !vg_sampler(t,&smp))return 0;
    VkDescriptorBufferInfo ub={vg_window_buffer,0,vg_uniform_bytes};
    VkDescriptorImageInfo im={t->sampler,t->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[]={
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=d->set,.dstBinding=0,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.pBufferInfo=&ub},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=d->set,.dstBinding=1,.descriptorCount=1,
         .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&im}};
    vgUpdateDescriptorSets(w_dev,2,writes,0,NULL);return 1;
}

static void vg_window_frame(int index)
{
    VgDisplay *d=&vg_display[index];uint32_t image=0;
    if(w_failed || !vg_ok(p_wait(w_dev,1,&w_fence,VK_TRUE,UINT64_MAX),"window buffer wait"))return;
    VkResult r=p_acquire(w_dev,w_sc,1000000000ull,w_acq,VK_NULL_HANDLE,&image);
    if(r==VK_TIMEOUT || r==VK_NOT_READY)return;
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR){vg_ok(r,"window acquire");return;}
    if(image>=w_nimg || !vg_ok(p_pool_reset(w_dev,w_pool,0),"window command reset"))return;
    float aspect=(float)w_window_extent.width/w_window_extent.height;
    float picture=d->aspect;
    float scale[4]={1,1,0,0};if(picture<aspect)scale[0]=picture/aspect;else scale[1]=aspect/picture;
    float rotation[4]={1,0,0,1};
    if(w_transform==VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR){rotation[0]=0;rotation[1]=-1;rotation[2]=1;rotation[3]=0;}
    else if(w_transform==VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR){rotation[0]=-1;rotation[3]=-1;}
    else if(w_transform==VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR){rotation[0]=0;rotation[1]=1;rotation[2]=-1;rotation[3]=0;}
    memcpy(vg_window_map+vg_uniform_offset[NV2A_U_VPSCALE],scale,16);
    memcpy(vg_window_map+vg_uniform_offset[NV2A_U_ZREV],rotation,16);
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(!vg_ok(p_begin(w_cmd,&bi),"window command begin"))return;
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.oldLayout=w_layout[image],.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=w_img[image],
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    p_barrier(w_cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,NULL,0,NULL,1,&b);
    VkRenderingAttachmentInfo ca={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=w_view[image],
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32={0,0,0,1}};
    VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},w_extent},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&ca};
    p_begin_rendering(w_cmd,&ri);
    VkViewport vp={0,0,(float)w_extent.width,(float)w_extent.height,0,1};VkRect2D sc={{0,0},w_extent};
    p_viewport(w_cmd,0,1,&vp);p_scissor(w_cmd,0,1,&sc);p_bind_pipeline(w_cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,vg_window_pipeline);
    uint32_t dynamic=0;
    vgCmdBindDescriptorSets(w_cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,vg_pipeline_layout,0,1,&d->set,1,&dynamic);
    p_cmd_draw(w_cmd,6,1,0,0);p_end_rendering(w_cmd);
    b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=0;
    b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    p_barrier(w_cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&b);
    if(!vg_ok(p_end(w_cmd),"window command end") || !vg_ok(p_reset(w_dev,1,&w_fence),"window fence reset"))return;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&w_acq,.pWaitDstStageMask=&stage,
        .commandBufferCount=1,.pCommandBuffers=&w_cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&w_done[image]};
    VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&w_done[image],
        .swapchainCount=1,.pSwapchains=&w_sc,.pImageIndices=&image};
    SDL_LockMutex(w_q_mu);r=p_submit(w_queue,1,&si,w_fence);
    if(r==VK_SUCCESS){w_layout[image]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;r=p_present(w_queue,&pi);}
    SDL_UnlockMutex(w_q_mu);
    if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR){vg_ok(r,"asynchronous present");return;}
    unsigned count=atomic_fetch_add(&vg_window_frames,1)+1;
    if(count==1 || count%300==0)vke("window present %u: game flip %u",count,d->flip);
}

static void vg_present_dispatch(void)
{
    SDL_LockMutex(w_mu);int index=vg_pending;vg_pending=-1;
    if(index>=0)vg_display[index].state=2;
    SDL_UnlockMutex(w_mu);
    if(index>=0){vg_window_frame(index);SDL_LockMutex(w_mu);vg_display[index].state=0;SDL_UnlockMutex(w_mu);}
    else if(!vg_ready)vke_frame_body();
}

static void vg_present(uint32_t rt,uint32_t pw,uint32_t ph)
{
    (void)pw;(void)ph;
    if(!rt || rt>=VG_RT || !vg_rt[rt].color || !vg_begin() || w_failed)return;
    uint32_t draws=vg_draws-vg_previous_draws;vg_previous_draws=vg_draws;
    if(draws>=100)vg_seen_scene=1;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    double seconds=(double)now.tv_sec+(double)now.tv_nsec/1e9;
    int show=vg_seen_scene || seconds-vg_last_window>=1.0/60.0,index=-1;
    if(show) {
        SDL_LockMutex(w_mu);
        if(vg_pending>=0){index=vg_pending;vg_pending=-1;}
        else for(int i=0;i<3;i++)if(vg_display[i].state==0){index=i;break;}
        if(index>=0)vg_display[index].state=3;
        SDL_UnlockMutex(w_mu);
    }
    if(index>=0) {
        VgTexture *src=&vg_tex[vg_rt[rt].color];
        if(!vg_display_target(index,src->w,src->h) || !vg_begin())return;
        vg_display[index].aspect=nv2a_wide_scene() ? nv2a_wide_aspect() : (float)src->w/src->h;
        VgTexture *dst=&vg_display[index].texture;
        vg_transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
        vg_transition(dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={src->w,src->h,1}};
        vgCmdCopyImage(vg_cmd,src->image,src->layout,dst->image,dst->layout,1,&copy);
        vg_transition(dst,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
    }
    vg_slots[vg_slot_index].end_frame=1;
    if(!vg_submit(0))return;
    vg_frames++;
    if(index>=0) {
        SDL_LockMutex(w_mu);
        if(vg_pending>=0)vg_display[vg_pending].state=0;
        vg_display[index].state=1;vg_display[index].flip=vg_frames;vg_pending=index;
        w_req=1;SDL_CondSignal(w_cv);SDL_UnlockMutex(w_mu);vg_last_window=seconds;
    }
}

static void vg_report(void)
{ vke("game renderer: %u logical flips, %u window presents, %u draws, %u pipelines",vg_frames,vg_window_frames,vg_draws,vg_npipe);
  vke("uniform counters: %llu calls, %llu unchanged, %llu snapshots, %llu uploaded bytes",
      (unsigned long long)vg_uniform_calls,(unsigned long long)vg_uniform_unchanged,
      (unsigned long long)vg_uniform_uploads,(unsigned long long)vg_uniform_upload_bytes); }
static void vg_gpu_ms(double *busy,double *waited)
{if(busy)*busy=vg_gpu_total_ms;if(waited)*waited=vg_wait_total_ms;}

static int vg_init(void)
{
    if (w_failed) return 0;
    if (vg_ready) return !w_failed;
    if (!w_dev || !w_q_mu) return 0;
    vg_depth_clamp = w_depth_clamp;
    vg_force_nocull=vke_switch_on("RECOMP_VK_GAME_NOCULL");
    vg_force_nodepth=vke_switch_on("RECOMP_VK_GAME_NODEPTH");
    vg_debug=vke_switch_on("RECOMP_VK_GAME_DEBUG");
#define VG_LOAD(n) vg##n = (PFN_vk##n)p_gipa(w_inst,"vk" #n); if (!vg##n) { vke("missing vk%s",#n);return 0; }
    VG_FUNCTIONS(VG_LOAD)
#undef VG_LOAD
    PFN_vkGetPhysicalDeviceProperties props=(PFN_vkGetPhysicalDeviceProperties)p_gipa(w_inst,"vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties p;props(w_phys,&p);
    vg_timestamp_period=p.limits.timestampPeriod;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qprops=
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)p_gipa(w_inst,"vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t nq=8;VkQueueFamilyProperties qp[8];qprops(w_phys,&nq,qp);
    if(w_family<nq) vg_timestamp_bits=qp[w_family].timestampValidBits;
    if(vg_timestamp_bits) {
        VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2*VG_SLOTS};
        if(!vg_ok(vgCreateQueryPool(w_dev,&qi,NULL,&vg_queries),"game timestamp queries")) return 0;
    }
    nv2a_gl_performance_backend_gpu(-1);
    vg_alignment=(uint32_t)p.limits.minUniformBufferOffsetAlignment;
    if (vg_alignment<p.limits.minStorageBufferOffsetAlignment) vg_alignment=(uint32_t)p.limits.minStorageBufferOffsetAlignment;
    if (vg_alignment<16) vg_alignment=16;
    if(!vg_buffer(VG_STATIC,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        &vg_static_buffer,&vg_static_memory,&vg_static_map)) return 0;
    vg_free_ranges[0]=(VgRange){0,VG_STATIC};vg_nfree_ranges=1;
    vg_uniform_layout();
    {const char *v=jsrf_getenv("RECOMP_VK_UNIFORM_DEDUP");vg_uniform_dedup=v?atoi(v)!=0:1;}
    if (vg_uniform_bytes>VG_UNIFORM_CAP || vg_uniform_bytes>p.limits.maxUniformBufferRange) return 0;
    VkDescriptorSetLayoutBinding bindings[5]={
        { .binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT } };
    for (int i=0;i<4;i++) bindings[i+1]=(VkDescriptorSetLayoutBinding){ .binding=(uint32_t)i+1,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo li={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=5,.pBindings=bindings };
    if (!vg_ok(vgCreateDescriptorSetLayout(w_dev,&li,NULL,&vg_set_layout),"game descriptor layout")) return 0;
    VkPipelineLayoutCreateInfo pli={ .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&vg_set_layout };
    if (!vg_ok(p_layout_new(w_dev,&pli,NULL,&vg_pipeline_layout),"game pipeline layout")) return 0;
    VkPipelineCacheCreateInfo pci={ .sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    if (!vg_ok(vgCreatePipelineCache(w_dev,&pci,NULL,&vg_pipeline_cache),"game pipeline cache")) return 0;
    for (int i=0;i<VG_SLOTS;i++) {
        VgSlot *s=&vg_slots[i];
        VkCommandPoolCreateInfo ci={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=w_family };
        if (!vg_ok(p_pool_new(w_dev,&ci,NULL,&s->pool),"game pool")) return 0;
        VkCommandBufferAllocateInfo ai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=s->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1 };
        if (!vg_ok(p_cmds(w_dev,&ai,&s->cmd),"game command buffer")) return 0;
        VkFenceCreateInfo fi={ .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT };
        if (!vg_ok(p_fence_new(w_dev,&fi,NULL,&s->fence),"game slot fence")) return 0;
        if (!vg_buffer(VG_UPLOAD,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&s->buffer,&s->memory,&s->map)) return 0;
        VkDescriptorPoolSize sizes[]={ {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,VG_SETS},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*VG_SETS} };
        VkDescriptorPoolCreateInfo di={ .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets=VG_SETS,.poolSizeCount=2,.pPoolSizes=sizes };
        if (!vg_ok(vgCreateDescriptorPool(w_dev,&di,NULL,&s->descriptors),"game descriptors")) return 0;
    }
    for (int i=0;i<16;i++) vg_attr_value[i][3]=1;
    vg_ready=1;
    uint32_t white=0xffffffffu;
    Nv2aSampler smp={0};
    vg_white=vg_tex_new(1,1,&white,&smp);
    if (!vg_white) return 0;
    vke("game renderer ready: %u-byte uniforms, three %u-MiB upload slots",vg_uniform_bytes,VG_UPLOAD>>20);
    vke("game GPU timestamps: %u valid bits, %.3f ns per tick; retained vertex arena %u MiB",vg_timestamp_bits,vg_timestamp_period,VG_STATIC>>20);
    if (vke_switch_on("RECOMP_VK_GAME_TEST") && !vg_self_test()) return 0;
    if(!vg_window_init())return 0;
    return 1;
}

static int vg_self_test(void)
{
    static const char *vs="#version 450\nlayout(location=0) in vec4 v0;\nlayout(location=3) in vec4 v3;\nout vec4 oD0;\nvoid main(){gl_Position=v0;oD0=v3;}\n";
    static const char *fs="#version 450\nuniform vec4 drawTint;\nuniform sampler2D tex0;\nin vec4 oD0;\nout vec4 frag;\nvoid main(){frag=texture(tex0,vec2(0.5))*drawTint*oD0;}\n";
    uint32_t target=vg_target_new(16,16),prog=vg_prog_new(vs,fs,"Vulkan device self-test");
    if (!target || !prog) return 0;
    vg_target_bind(target);vg_prog_use(prog);
    float tint[4]={0.25f,0.5f,0.75f,1};vg_uniform_f(NV2A_U_DRAWTINT,tint,1);
    Nv2aRenderState rs={ .color_mask=0x01010101u };vg_set_state(&rs);
    const float vertices[]={-1,-1,0,1, 3,-1,0,1, -1,3,0,1};
    int at[16];for(int i=0;i<16;i++) at[i]=-1;at[0]=0;
    float values[16][4]={{0}};for(int k=0;k<4;k++)values[3][k]=1;
    const float clear[4]={0,0,0,1};
    vg_clear(7,clear,1,0,NULL);vg_vertices(vertices,3,4,at,1u<<3,values);vg_draw(NV2A_PRIM_TRIANGLES,3);
    uint8_t pixel[4]={0};
    int ok=vg_target_read(target,8,8,1,1,0,pixel);
    vke("game device test: pixel %u %u %u %u, expected 64 128 191 255",pixel[0],pixel[1],pixel[2],pixel[3]);
    ok=ok && pixel[0]==64 && pixel[1]==128 && pixel[2]==191 && pixel[3]==255;
    for (int clockwise=0;clockwise<2;clockwise++) {
        rs.cull_enable=1;rs.cull_face=0x405;rs.front_face=clockwise?0x900:0x901;
        vg_set_state(&rs);vg_clear(7,clear,1,0,NULL);
        vg_vertices(vertices,3,4,at,1u<<3,values);vg_draw(NV2A_PRIM_TRIANGLES,3);
        int read=vg_target_read(target,8,8,1,1,0,pixel);
        vke("game culling test %s: pixel %u %u %u %u",clockwise?"CW culled":"CCW visible",pixel[0],pixel[1],pixel[2],pixel[3]);
        ok=ok && read && pixel[0]==(clockwise?0:64) && pixel[1]==(clockwise?0:128) && pixel[2]==(clockwise?0:191);
    }
    rs.cull_enable=0;vg_set_state(&rs);
    for(int stream=0;stream<2;stream++) {
        uint32_t vb=vg_vbuf_new(stream);
        const uint16_t idx16[]={2,0,1};const uint32_t idx32[]={1,2,0};
        for(int wide=0;wide<2;wide++) {
            vg_vbuf_fill(vb,vertices,sizeof vertices,wide?(const void *)idx32:(const void *)idx16,
                wide?sizeof idx32:sizeof idx16);
            vg_clear(7,clear,1,0,NULL);vg_vbuf_bind(vb,4,at,1u<<3,values);
            vg_draw_indexed(NV2A_PRIM_TRIANGLES,3,wide);
            int read=vg_target_read(target,8,8,1,1,0,pixel);
            vke("game vertex cache test stream %d index %d: pixel %u %u %u %u",stream,wide?32:16,pixel[0],pixel[1],pixel[2],pixel[3]);
            ok=ok && read && pixel[0]==64 && pixel[1]==128 && pixel[2]==191;
        }
        vg_vbuf_free(vb);
    }
    vg_prog_free(prog);vg_target_free(target);memset(&vg_state,0,sizeof vg_state);
    vg_gpu_frame.frame=UINT32_MAX;vg_gpu_frame.us=0;vg_gpu_total_ms=vg_wait_total_ms=0;
    nv2a_gl_performance_backend_gpu(-1);
    return ok;
}

static void vg_stop(void)
{
    if (!vg_ready && !vg_set_layout && !vg_slots[0].pool) return;
    vg_drain();
    for (uint32_t i=0;i<vg_npipe;i++) if (vg_pipes[i].pipeline) p_pipe_free(w_dev,vg_pipes[i].pipeline,NULL);
    for (uint32_t i=1;i<VG_PROG;i++) if (vg_prog[i].uniforms) {
        p_shader_free(w_dev,vg_prog[i].vs,NULL);p_shader_free(w_dev,vg_prog[i].fs,NULL);free(vg_prog[i].uniforms);
    }
    for (uint32_t i=1;i<VG_TEX;i++) vg_image_free(&vg_tex[i]);
    for (uint32_t i=1;i<VG_RT;i++) vg_image_free(&vg_rt[i].depth);
    for(uint32_t i=1;i<VG_VBUFS;i++){free(vg_vbufs[i].vertices);free(vg_vbufs[i].indices);}
    for(int i=0;i<3;i++)vg_image_free(&vg_display[i].texture);
    if(vg_window_descriptors)vgDestroyDescriptorPool(w_dev,vg_window_descriptors,NULL);
    if(vg_window_map)p_unmap(w_dev,vg_window_memory);
    if(vg_window_buffer)p_buf_free(w_dev,vg_window_buffer,NULL);
    if(vg_window_memory)p_mem_free(w_dev,vg_window_memory,NULL);
    vg_window_descriptors=VK_NULL_HANDLE;vg_window_map=NULL;vg_window_buffer=VK_NULL_HANDLE;vg_window_memory=VK_NULL_HANDLE;
    memset(vg_display,0,sizeof vg_display);vg_pending=-1;vg_window_program=0;vg_window_pipeline=VK_NULL_HANDLE;
    for (int i=0;i<VG_SLOTS;i++) {
        VgSlot *s=&vg_slots[i];
        if (s->descriptors) vgDestroyDescriptorPool(w_dev,s->descriptors,NULL);
        if (s->map) p_unmap(w_dev,s->memory);
        if (s->buffer) p_buf_free(w_dev,s->buffer,NULL);
        if (s->memory) p_mem_free(w_dev,s->memory,NULL);
        if (s->fence) p_fence_free(w_dev,s->fence,NULL);
        if (s->pool) p_pool_free(w_dev,s->pool,NULL);
    }
    if (vg_pipeline_cache) vgDestroyPipelineCache(w_dev,vg_pipeline_cache,NULL);
    if (vg_pipeline_layout) p_layout_free(w_dev,vg_pipeline_layout,NULL);
    if (vg_set_layout) vgDestroyDescriptorSetLayout(w_dev,vg_set_layout,NULL);
    if(vg_queries) vgDestroyQueryPool(w_dev,vg_queries,NULL);
    if(vg_static_map) p_unmap(w_dev,vg_static_memory);
    if(vg_static_buffer) p_buf_free(w_dev,vg_static_buffer,NULL);
    if(vg_static_memory) p_mem_free(w_dev,vg_static_memory,NULL);
    vg_queries=VK_NULL_HANDLE;vg_static_map=NULL;vg_static_buffer=VK_NULL_HANDLE;vg_static_memory=VK_NULL_HANDLE;
    vg_pipeline_cache=VK_NULL_HANDLE;vg_pipeline_layout=VK_NULL_HANDLE;vg_set_layout=VK_NULL_HANDLE;
    memset(vg_slots,0,sizeof vg_slots);memset(vg_tex,0,sizeof vg_tex);memset(vg_rt,0,sizeof vg_rt);
    memset(vg_prog,0,sizeof vg_prog);memset(vg_pipes,0,sizeof vg_pipes);
    memset(vg_pipe_head,0,sizeof vg_pipe_head);memset(vg_pipe_next,0,sizeof vg_pipe_next);
    memset(vg_vbufs,0,sizeof vg_vbufs);vg_nfree_ranges=vg_cached_vb=0;
    vg_ready=vg_recording=vg_rendering=0;vg_slot_index=vg_current_rt=vg_current_prog=vg_npipe=0;
}

static const Nv2aGfx vg_gfx={
    .name="Vulkan",.lang=NV2A_LANG_GLSL,.rt_bottom_up=0,.init=vg_init,
    .target_new=vg_target_new,.target_free=vg_target_free,.target_bind=vg_target_bind,
    .target_texture=vg_target_texture,.target_snapshot=vg_target_snapshot,.target_read=vg_target_read,
    .target_read_depth=vg_depth_read,.tex_new=vg_tex_new,.tex_free=vg_tex_free,.tex_read=vg_tex_read,
    .prog_new=vg_prog_new,.prog_free=vg_prog_free,.prog_uses=vg_prog_uses,.prog_use=vg_prog_use,
    .uniform_f=vg_uniform_f,.uniform_i=vg_uniform_i,.texture=vg_texture,.state=vg_set_state,
    .state_invalidate=vg_invalidate,.vertices=vg_vertices,.draw=vg_draw,.clear=vg_clear,
    .present=vg_present,.report=vg_report
    ,.vbuf_new=vg_vbuf_new,.vbuf_free=vg_vbuf_free,.vbuf_fill=vg_vbuf_fill,.vbuf_bind=vg_vbuf_bind,
    .draw_indexed=vg_draw_indexed,.gpu_ms=vg_gpu_ms,.uniform_f_part=vg_uniform_f_part
};

const Nv2aGfx *nv2a_vk_game_gfx(void) { return &vg_gfx; }
