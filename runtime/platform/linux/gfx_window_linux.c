// The SDL3 window and the Vulkan swapchain on Linux: the counterpart of runtime/gfx_window.c.
//
// Windows creates the window with CreateWindowExW and the surface with vkCreateWin32SurfaceKHR; here SDL3 owns the
// window, the input devices and the surface (SDL_Vulkan_CreateSurface knows whether the video driver is X11, Wayland or
// something else), and this file owns the swapchain, the blit of the game's image into it and the present.
//
// First slice: one window, one swapchain, a straight blit of the game image (the Windows file also does FSR, the menu
// overlay, flight thumbnails and frame capture; those follow once the game renders).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform.h"
#include "vk.h"

#define MAX_SC_IMAGES 8
#define MAX_FRAMES 3

static SDL_Window *g_win;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_sc;
static VkFormat g_sc_fmt;
static VkExtent2D g_sc_ext;
static uint32_t g_sc_n;
static VkImage g_sc_img[MAX_SC_IMAGES];
static VkImageView g_sc_view[MAX_SC_IMAGES];
static VkCommandPool g_pool;
static VkCommandBuffer g_cb[MAX_FRAMES];
static VkFence g_fence[MAX_FRAMES];
static VkSemaphore g_acquire[MAX_FRAMES];
static VkSemaphore g_render_done[MAX_SC_IMAGES];
static unsigned g_frame;
static int g_flip_on;
static uint32_t g_flip_idx;
static int g_started;
static PlatMutex g_pl;            // swapchain and present state
static int g_dirty = 1;           // the swapchain is stale (first frame, resize)
static double g_rate_hz = 60.0;
static int g_focused = 1;

// SDL's video and Vulkan loader must be up before any thread asks SDL a question: the prewarm thread calls vk_init
// before the main thread reaches gfx_window_start. A constructor runs on the main thread before main().
__attribute__((constructor)) static void sdl_early_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gfx: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return;
    }
    if (!SDL_Vulkan_LoadLibrary(NULL)) fprintf(stderr, "gfx: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
}

// ---- queue lock: submissions and presents share gfx_queue

static PlatMutex g_ql;

void vk_queue_lock(void) { plat_mutex_lock(&g_ql); }
void vk_queue_unlock(void) { plat_mutex_unlock(&g_ql); }

// ---- window

VkNativeWindow gfx_window_handle(void) { return (VkNativeWindow)g_win; }

int gfx_window_focused(void) { return g_focused; }
unsigned gfx_window_focus_messages(void) { return 0; }
double gfx_window_refresh_hz(void) { return g_rate_hz; }

static void cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    const char *value = cfg_value("TOTK_WINDOW_SIZE");
    if (!value || sscanf(value, "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320) {
        const CfgMeta *meta = cfg_meta("WINDOW_SIZE");
        if (!meta || !meta->def || sscanf(meta->def, "%dx%d", &x, &y) != 2) { x = 1280; y = 720; }
    }
    *w = x;
    *h = y;
}

void gfx_vsync_bind(uint64_t event) { (void)event; }
void gfx_vsync_bind_counter(volatile long *counter) { (void)counter; }

// ---- swapchain

static int create_swapchain(void) {
    VkCtx *c = vk_ctx();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, g_surface, &caps) != VK_SUCCESS) return 0;
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                            : (VkExtent2D){(uint32_t)ww, (uint32_t)wh};
    if (!ext.width || !ext.height) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, fmts);
    if (!nf) return 0;
    VkSurfaceFormatKHR sf = fmts[0];
    int found = 0;
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            found = 1;
        }
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            found = 1;
        }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SC_IMAGES) count = MAX_SC_IMAGES;

    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_surface,
        .minImageCount = count,
        .imageFormat = sf.format,
        .imageColorSpace = sf.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(c->device, &ci, NULL, &g_sc);
    if (r != VK_SUCCESS) {
        rt_log("gfx: vkCreateSwapchainKHR failed (%d)", (int)r);
        g_sc = old;
        vk_queue_unlock();
        return 0;
    }
    for (uint32_t i = 0; i < g_sc_n; i++) {
        if (g_render_done[i]) vkDestroySemaphore(c->device, g_render_done[i], NULL);
        if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
        g_render_done[i] = VK_NULL_HANDLE;
        g_sc_view[i] = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(c->device, old, NULL);
    g_sc_fmt = sf.format;
    g_sc_ext = ext;
    g_sc_n = MAX_SC_IMAGES;
    vkGetSwapchainImagesKHR(c->device, g_sc, &g_sc_n, g_sc_img);
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_sc_n; i++) {
        vkCreateSemaphore(c->device, &semi, NULL, &g_render_done[i]);
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = g_sc_img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &g_sc_view[i]) != VK_SUCCESS) g_sc_view[i] = VK_NULL_HANDLE;
    }
    vk_queue_unlock();
    g_dirty = 0;
    return 1;
}

// ---- present

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = s0,
                               .srcAccessMask = a0,
                               .dstStageMask = s1,
                               .dstAccessMask = a1,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                            .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &dep);
}

static unsigned long long n_blit_ok, n_blit_ood, n_blit_fail, n_flip_ok, n_flip_skip;

// TOTK_PRESENT_LOG=1: a line every 120 presents. A window that stays black while the frame dumps show content means the
// blit or the present is being dropped, and this says which.
static void present_stats(void) {
    static unsigned long long next;
    if (!getenv("TOTK_PRESENT_LOG")) return;
    if (n_blit_ok + n_blit_ood + n_blit_fail < next) return;
    next = n_blit_ok + n_blit_ood + n_blit_fail + 120;
    rt_log("gfx: presents: blit ok %llu, out-of-date %llu, dropped %llu; flips %llu, skipped %llu", n_blit_ok, n_blit_ood,
           n_blit_fail, n_flip_ok, n_flip_skip);
}

void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    // The window comes up on the first present. The backend presents through blit + flip rather than vk_present_image,
    // so this, not that, is where gfx_window_start has to be: without it every present did nothing (on the BC-250 the
    // game ran through 681 presents, world clock and all, with no window ever created).
    if (!g_started) gfx_window_start();
    VkCtx *c = vk_ctx();
    if (!g_started) return;
    plat_mutex_lock(&g_pl);
    if (g_dirty && !create_swapchain()) {
        plat_mutex_unlock(&g_pl);
        return;
    }
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(c->device, g_sc, UINT64_MAX, g_acquire[g_frame], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        n_blit_ood++;
        create_swapchain();
        plat_mutex_unlock(&g_pl);
        present_stats();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        n_blit_fail++;
        plat_mutex_unlock(&g_pl);
        present_stats();
        return;
    }
    vkWaitForFences(c->device, 1, &g_fence[g_frame], VK_TRUE, UINT64_MAX);
    vkResetFences(c->device, 1, &g_fence[g_frame]);
    vkResetCommandBuffer(g_cb[g_frame], 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb[g_frame], &bi);
    VkImageLayout src_layout = layout;
    image_barrier(g_cb[g_frame], src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(g_cb[g_frame], g_sc_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {0};
    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    if (cw <= 0) cw = w;
    if (ch <= 0) ch = h;
    blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = (VkOffset3D){cx, cy, 0};
    blit.srcOffsets[1] = (VkOffset3D){cx + cw, cy + ch, 1};
    blit.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = (VkOffset3D){0, 0, 0};
    blit.dstOffsets[1] = (VkOffset3D){(int32_t)g_sc_ext.width, (int32_t)g_sc_ext.height, 1};
    static int logged_fmt;
    if (!logged_fmt++) rt_log("gfx: blit %dx%d format %d -> swapchain %ux%u format %d", w, h, (int)fmt, g_sc_ext.width,
                              g_sc_ext.height, (int)g_sc_fmt);
    // TOTK_BLIT_DUMP=1: write the image the blit reads to /tmp/blit_src.ppm, once. A red swapchain clear proves the
    // present path works; this proves what the blit was given.
    if (getenv("TOTK_BLIT_DUMP")) {
        static int done;
        if (!done) {
            done = 1;
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO,
                                           .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                    VMA_ALLOCATION_CREATE_MAPPED_BIT};
            VkBuffer db = VK_NULL_HANDLE;
            VmaAllocation da = NULL;
            VmaAllocationInfo di = {0};
            if (vmaCreateBuffer(c->vma, &bci, &aci, &db, &da, &di) == VK_SUCCESS) {
                VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                        .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
                vkCmdCopyImageToBuffer(g_cb[g_frame], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, db, 1, &cp);
                image_barrier(g_cb[g_frame], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout,
                              VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT);
                vkEndCommandBuffer(g_cb[g_frame]);
                VkSubmitInfo si1 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                    .pCommandBuffers = &g_cb[g_frame]};
                vk_queue_lock();
                vkQueueSubmit(c->gfx_queue, 1, &si1, g_fence[g_frame]);
                vk_queue_unlock();
                vkWaitForFences(c->device, 1, &g_fence[g_frame], VK_TRUE, UINT64_MAX);
                vkResetFences(c->device, 1, &g_fence[g_frame]);
                vkResetCommandBuffer(g_cb[g_frame], 0);
                vkBeginCommandBuffer(g_cb[g_frame], &bi);
                image_barrier(g_cb[g_frame], src, src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                image_barrier(g_cb[g_frame], g_sc_img[idx], VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                              VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                if (di.pMappedData) {
                    FILE *f = fopen("/tmp/blit_src.ppm", "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", w, h);
                        const uint8_t *px = di.pMappedData;
                        for (long i = 0; i < (long)w * h; i++) fwrite(px + i * 4, 1, 3, f);   // drop alpha
                        fclose(f);
                        rt_log("gfx: blit dump written to /tmp/blit_src.ppm (%dx%d)", w, h);
                    }
                }
                vmaDestroyBuffer(c->vma, db, da);
            }
        }
    }
    // TOTK_PRESENT_TEST=1: clear the swapchain image instead of blitting, to tell a broken blit from a broken present.
    if (getenv("TOTK_PRESENT_TEST")) {
        static const VkClearColorValue red = {{1.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(g_cb[g_frame], g_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &r);
        rt_log("gfx: present test: swapchain image %u cleared red", idx);
    } else
    vkCmdBlitImage(g_cb[g_frame], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_sc_img[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(g_cb[g_frame], g_sc_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_NONE, 0);
    vkEndCommandBuffer(g_cb[g_frame]);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_2_BLIT_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_acquire[g_frame],
                           .pWaitDstStageMask = &wait,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &g_cb[g_frame],
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &g_render_done[idx]};
    vk_queue_lock();
    if (vkQueueSubmit(c->gfx_queue, 1, &submit, g_fence[g_frame]) != VK_SUCCESS) {
        vk_queue_unlock();
        plat_mutex_unlock(&g_pl);
        return;
    }
    vk_queue_unlock();
    g_flip_on = 1;
    g_flip_idx = idx;
    n_blit_ok++;
    plat_mutex_unlock(&g_pl);
    present_stats();
}

// The window's event loop. SDL answers the compositor's pings from here -- without it KDE marks the window "not
// responding" -- and the focus messages go to the guest the way the Windows window procedure sends them.
static void gfx_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            rt_log("gfx: window closed; exiting");
            exit(0);
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            oe_set_focus(1);
            oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            oe_set_focus(0);
            oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
            break;
        default: break;
        }
    }
}

void vk_present_flip(void) {
    if (!g_started) return;
    gfx_pump_events();   // per frame: pings, focus, quit
    if (!g_flip_on) { n_flip_skip++; present_stats(); return; }
    static unsigned long long presents, last_log;
    uint64_t t = (uint64_t)(plat_now_ns() / 1000000);
    VkCtx *c = vk_ctx();
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_render_done[g_flip_idx],
                           .swapchainCount = 1,
                           .pSwapchains = &g_sc,
                           .pImageIndices = &g_flip_idx};
    vk_queue_lock();
    VkResult r = vkQueuePresentKHR(c->gfx_queue, &pi);
    if (r == VK_SUCCESS) n_flip_ok++;
    if (n_flip_ok + n_flip_skip <= 3 || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR))
        rt_log("gfx: present %llu: result %d", n_flip_ok + n_flip_skip, (int)r);
    vk_queue_unlock();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_dirty = 1;
    presents++;
    if (presents == 1)
        rt_logf("gfx: first present (swapchain %ux%u, %s)\n", g_sc_ext.width, g_sc_ext.height, r == VK_SUCCESS ? "ok" : "result != success");
    if (presents - last_log >= 120) {
        static uint64_t t0;
        if (!t0) t0 = t;
        if (t > t0) rt_logf("gfx: %llu presents, %.1f fps\n", presents, (double)presents * 1000.0 / (double)(t - t0));
        last_log = presents;
    }
    g_flip_on = 0;
    g_frame = (g_frame + 1) % MAX_FRAMES;
}

void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    // The Windows implementation starts the window from present_image; the Linux one has to do the same or nothing ever
    // creates the window and swapchain (the guest's first present is what asks for them).
    gfx_window_start();
    vk_present_blit(src, layout, fmt, w, h, crop);
    vk_present_flip();
}

void vk_present_capture(int on) { (void)on; }
int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt) {
    (void)px; (void)w; (void)h; (void)fmt;
    return 0;
}
void vk_thumbs_enable(int n) { (void)n; }
void vk_thumbs_frame(uint64_t frame) { (void)frame; }
int vk_thumbs_read(uint64_t frame, uint8_t *rgba) { (void)frame; (void)rgba; return 0; }
int probe_window(void) { return 0; }

// ---- lifecycle

void gfx_window_start(void) {
    plat_mutex_lock(&g_pl);
    if (g_started) {
        plat_mutex_unlock(&g_pl);
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) rt_fatal("gfx: SDL_Init(SDL_INIT_VIDEO) failed: %s", SDL_GetError());
    // SDL answers the presentation-support question through its own Vulkan loader; without this it has none.
    if (!SDL_Vulkan_LoadLibrary(NULL)) rt_fatal("gfx: SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
    int w, h;
    cfg_window_size(&w, &h);
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    g_win = SDL_CreateWindow("TotK Recomp", w, h, flags);
    if (!g_win) rt_fatal("gfx: SDL_CreateWindow failed: %s", SDL_GetError());
    // The device picker asks SDL whether a queue family can present, so the video driver must already own the window.
    if (!vk_init()) rt_fatal("gfx: no usable Vulkan device (see the vk: lines above)");
    VkCtx *c = vk_ctx();
    if (!SDL_Vulkan_CreateSurface(g_win, c->instance, NULL, &g_surface))
        rt_fatal("gfx: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    vkCreateCommandPool(c->device, &pci, NULL, &g_pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = MAX_FRAMES};
    vkAllocateCommandBuffers(c->device, &ai, g_cb);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < MAX_FRAMES; i++) {
        vkCreateFence(c->device, &fi, NULL, &g_fence[i]);
        vkCreateSemaphore(c->device, &semi, NULL, &g_acquire[i]);
    }
    g_started = 1;
    // The game waits on the applet system event before anything else, and on Windows the window procedure's
    // WM_ACTIVATEAPP hands it the focus message. Without that the guest parks in WaitSystemEvent for ever and the boot
    // freezes on the loading screen.
    oe_set_focus(1);
    oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
    if (!create_swapchain()) rt_fatal("gfx: swapchain creation failed");
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode && mode->refresh_rate > 1) g_rate_hz = mode->refresh_rate;
    rt_log("gfx: window %dx%d, swapchain %ux%u format %d, %.0f Hz, driver %s", w, h, g_sc_ext.width, g_sc_ext.height,
           (int)g_sc_fmt, g_rate_hz, SDL_GetCurrentVideoDriver());
    plat_mutex_unlock(&g_pl);
}

void gfx_window_stop(void) {
    plat_mutex_lock(&g_pl);
    if (g_started) {
        VkCtx *c = vk_ctx();
        vkDeviceWaitIdle(c->device);
        for (int i = 0; i < MAX_FRAMES; i++) {
            if (g_fence[i]) vkDestroyFence(c->device, g_fence[i], NULL);
            if (g_acquire[i]) vkDestroySemaphore(c->device, g_acquire[i], NULL);
        }
        for (uint32_t i = 0; i < g_sc_n; i++) {
            if (g_render_done[i]) vkDestroySemaphore(c->device, g_render_done[i], NULL);
            if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
        }
        if (g_pool) vkDestroyCommandPool(c->device, g_pool, NULL);
        if (g_sc) vkDestroySwapchainKHR(c->device, g_sc, NULL);
        if (g_surface) vkDestroySurfaceKHR(c->instance, g_surface, NULL);
        g_sc = VK_NULL_HANDLE;
        g_surface = VK_NULL_HANDLE;
        g_started = 0;
    }
    plat_mutex_unlock(&g_pl);
}

void gfx_window_release_vk(void) { gfx_window_stop(); }

void gfx_window_resize(int w, int h) {
    if (g_win) SDL_SetWindowSize(g_win, w, h);
    g_dirty = 1;
}

void gfx_window_apply_settings(void) {}
void gfx_window_request_close(void) {}
int gfx_window_monitor_size(int *w, int *h) {
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode) return 0;
    *w = mode->w;
    *h = mode->h;
    return 1;
}
const char *gfx_window_present_mode(void) { return "fifo"; }
