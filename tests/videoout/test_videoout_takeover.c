/* PS5 Vulkan - which swapchain has VideoOut's one display, on RADV's host model.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Counter-Strike 1.6 under Zink: hl.exe's wined3d makes an OpenGL context on a
 * hidden 10x10 window (a 111x1 client area) to read the driver's caps, which
 * makes a swapchain on its own display-plane surface, and never presents.
 * The game's window, made next, must get the display. A swapchain that has
 * presented keeps it: a later window's swapchain, or the displaced one made
 * again, is refused with VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, while the one
 * presenting can still be replaced through oldSwapchain.
 * tools/test-videoout-wsi.sh runs it against the host build of the pinned
 * PS5_Mesa. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define FRAMES 4

#define CHECK(call)                                                                                \
    do                                                                                             \
    {                                                                                              \
        const VkResult check_result = (call);                                                      \
        if (check_result != VK_SUCCESS)                                                            \
        {                                                                                          \
            fprintf(stderr, "%s:%d: %s: %d\n", __FILE__, __LINE__, #call, check_result);           \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define EXPECT(call, want)                                                                         \
    do                                                                                             \
    {                                                                                              \
        const VkResult expect_result = (call);                                                     \
        if (expect_result != (want))                                                               \
        {                                                                                          \
            fprintf(stderr, "%s:%d: %s: %d, want %d\n", __FILE__, __LINE__, #call, expect_result,  \
                    (want));                                                                       \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

struct context
{
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkDisplayModeKHR mode;
    VkExtent2D mode_extent;
    VkCommandPool pool;
};

static void make_context(struct context *c)
{
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_DISPLAY_EXTENSION_NAME};
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "test_videoout_takeover",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    CHECK(vkCreateInstance(&instance_info, NULL, &c->instance));
    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(c->instance, &count, &c->physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0)
    {
        fprintf(stderr, "no physical device: %d\n", result);
        exit(EXIT_FAILURE);
    }
    VkDisplayPropertiesKHR display;
    count = 1;
    result = vkGetPhysicalDeviceDisplayPropertiesKHR(c->physical, &count, &display);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0)
    {
        fprintf(stderr, "no display: %d\n", result);
        exit(EXIT_FAILURE);
    }
    VkDisplayModePropertiesKHR modes[8];
    count = 8;
    result = vkGetDisplayModePropertiesKHR(c->physical, display.display, &count, modes);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0)
    {
        fprintf(stderr, "no display mode: %d\n", result);
        exit(EXIT_FAILURE);
    }
    c->mode = modes[0].displayMode;
    c->mode_extent = modes[0].parameters.visibleRegion;

    VkQueueFamilyProperties families[8];
    count = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, families);
    c->family = UINT32_MAX;
    for (uint32_t i = 0; i < count && c->family == UINT32_MAX; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            c->family = i;
    if (c->family == UINT32_MAX)
    {
        fprintf(stderr, "no graphics queue\n");
        exit(EXIT_FAILURE);
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                       VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = device_extensions,
    };
    CHECK(vkCreateDevice(c->physical, &device_info, NULL, &c->device));
    vkGetDeviceQueue(c->device, c->family, 0, &c->queue);
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->family,
    };
    CHECK(vkCreateCommandPool(c->device, &pool_info, NULL, &c->pool));
}

/* The surface win32u's PS5 driver makes for every window: the display's first mode. */
static VkSurfaceKHR window_surface(const struct context *c)
{
    const VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = c->mode,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = c->mode_extent,
    };
    VkSurfaceKHR surface;
    CHECK(vkCreateDisplayPlaneSurfaceKHR(c->instance, &info, NULL, &surface));
    VkBool32 supported = VK_FALSE;
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, surface, &supported));
    if (!supported)
    {
        fprintf(stderr, "the graphics queue cannot present\n");
        exit(EXIT_FAILURE);
    }
    return surface;
}

static VkResult make_swapchain(const struct context *c, VkSurfaceKHR surface, uint32_t width,
                               uint32_t height, VkSwapchainKHR old, VkSwapchainKHR *swapchain)
{
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = 3,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {width, height},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    *swapchain = VK_NULL_HANDLE;
    return vkCreateSwapchainKHR(c->device, &info, NULL, swapchain);
}

static VkResult acquire(const struct context *c, VkSwapchainKHR swapchain, uint32_t *index)
{
    VkFence acquired;
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(c->device, &fence_info, NULL, &acquired));
    const VkResult result =
        vkAcquireNextImageKHR(c->device, swapchain, UINT64_C(5000000000), VK_NULL_HANDLE, acquired, index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        CHECK(vkWaitForFences(c->device, 1, &acquired, VK_TRUE, UINT64_C(5000000000)));
    vkDestroyFence(c->device, acquired, NULL);
    return result;
}

/* FRAMES frames, each image cleared to a colour. */
static void present_frames(const struct context *c, VkSwapchainKHR swapchain)
{
    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(c->device, swapchain, &image_count, NULL));
    VkImage images[8];
    if (image_count > 8)
        image_count = 8;
    CHECK(vkGetSwapchainImagesKHR(c->device, swapchain, &image_count, images));
    VkSemaphore rendered;
    const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(vkCreateSemaphore(c->device, &semaphore_info, NULL, &rendered));
    VkFence done;
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(c->device, &fence_info, NULL, &done));
    VkCommandBuffer cmd;
    const VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(c->device, &allocate, &cmd));
    for (unsigned frame = 0; frame < FRAMES; frame++)
    {
        uint32_t index;
        CHECK(acquire(c, swapchain, &index));
        CHECK(vkResetCommandBuffer(cmd, 0));
        const VkCommandBufferBeginInfo begin = {.sType =
                                                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        const VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[index],
            .subresourceRange = range,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
        const VkClearColorValue colour = {.float32 = {frame & 1 ? 1.0f : 0.0f, 0.5f, 0.25f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
                             &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                             &barrier);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &rendered,
        };
        CHECK(vkQueueSubmit(c->queue, 1, &submit, done));
        const VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &rendered,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &index,
        };
        CHECK(vkQueuePresentKHR(c->queue, &present));
        CHECK(vkWaitForFences(c->device, 1, &done, VK_TRUE, UINT64_C(5000000000)));
        CHECK(vkResetFences(c->device, 1, &done));
    }
    CHECK(vkQueueWaitIdle(c->queue));
    vkFreeCommandBuffers(c->device, c->pool, 1, &cmd);
    vkDestroyFence(c->device, done, NULL);
    vkDestroySemaphore(c->device, rendered, NULL);
}

int main(void)
{
    struct context c;
    memset(&c, 0, sizeof(c));
    make_context(&c);
    const VkSurfaceKHR probe_surface = window_surface(&c);
    const VkSurfaceKHR game_surface = window_surface(&c);
    const VkSurfaceKHR later_surface = window_surface(&c);
    VkSwapchainKHR probe, game, later, again;
    uint32_t index;

    /* wined3d's caps context: a swapchain on the hidden window, an image
     * acquired for its first draw, and no present. */
    CHECK(make_swapchain(&c, probe_surface, 111, 1, VK_NULL_HANDLE, &probe));
    CHECK(acquire(&c, probe, &index));
    printf("probe: 111x1 swapchain, image %u acquired, never presented\n", index);

    /* The game's window gets the display; the probe's swapchain is out of date. */
    CHECK(make_swapchain(&c, game_surface, 1920, 1080, VK_NULL_HANDLE, &game));
    present_frames(&c, game);
    EXPECT(acquire(&c, probe, &index), VK_ERROR_OUT_OF_DATE_KHR);
    printf("game: 1920x1080 swapchain took the display and presented %u frames\n", FRAMES);

    /* A swapchain that presented keeps the display: neither a later window's
     * nor the probe's made again takes it. */
    EXPECT(make_swapchain(&c, later_surface, 1920, 1080, VK_NULL_HANDLE, &later),
           VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
    EXPECT(make_swapchain(&c, probe_surface, 111, 1, probe, &again),
           VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
    present_frames(&c, game);
    printf("game: kept the display from a later window and from the probe made again\n");

    /* The game can still replace its own swapchain, and once it is destroyed
     * another window takes the display. */
    CHECK(make_swapchain(&c, game_surface, 1280, 720, game, &again));
    vkDestroySwapchainKHR(c.device, game, NULL);
    game = again;
    present_frames(&c, game);
    vkDestroySwapchainKHR(c.device, game, NULL);
    CHECK(make_swapchain(&c, later_surface, 1920, 1080, VK_NULL_HANDLE, &later));
    present_frames(&c, later);
    printf("game: replaced through oldSwapchain; after it was destroyed a later window presented\n");

    vkDestroySwapchainKHR(c.device, later, NULL);
    vkDestroySwapchainKHR(c.device, probe, NULL);
    vkDestroySurfaceKHR(c.instance, later_surface, NULL);
    vkDestroySurfaceKHR(c.instance, game_surface, NULL);
    vkDestroySurfaceKHR(c.instance, probe_surface, NULL);
    vkDestroyCommandPool(c.device, c.pool, NULL);
    vkDestroyDevice(c.device, NULL);
    vkDestroyInstance(c.instance, NULL);
    printf("test_videoout_takeover: pass\n");
    return EXIT_SUCCESS;
}
