/* PS5 Vulkan - VideoOut swapchains of every size, on RADV's host model.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Makes a display-plane surface as Prospero Win's win32u does (VK_KHR_display,
 * the display's first mode) and presents frames through swapchains of the
 * sizes a Direct3D game switches between, each made after the last is
 * destroyed as DXVK does, then through oldSwapchain. Every create, acquire and
 * present must succeed. tools/test-videoout-wsi.sh runs it against the host
 * build of the pinned PS5_Mesa with MESA_VK_VIDEOOUT_HOST_LOG set and checks
 * what the backend registered and flipped from its log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define FRAMES 8

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

struct context
{
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkCommandPool pool;
};

static void make_context(struct context *c)
{
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_DISPLAY_EXTENSION_NAME};
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "test_videoout_swapchain",
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

    /* The surface win32u's PS5 driver makes: the display's first mode. */
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
    const VkDisplaySurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = modes[0].displayMode,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = modes[0].parameters.visibleRegion,
    };
    CHECK(vkCreateDisplayPlaneSurfaceKHR(c->instance, &surface_info, NULL, &c->surface));
    printf("mode %ux%u at %u mHz\n", modes[0].parameters.visibleRegion.width,
           modes[0].parameters.visibleRegion.height, modes[0].parameters.refreshRate);

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
    VkBool32 supported = VK_FALSE;
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, c->surface, &supported));
    if (!supported)
    {
        fprintf(stderr, "the graphics queue cannot present\n");
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

static void check_capabilities(const struct context *c)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->physical, c->surface, &caps));
    if (caps.minImageExtent.width != 1 || caps.minImageExtent.height != 1 ||
        caps.maxImageExtent.width < 1920 || caps.maxImageExtent.height < 1080)
    {
        fprintf(stderr, "the surface takes %ux%u to %ux%u, not every size up to the mode's\n",
                caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width,
                caps.maxImageExtent.height);
        exit(EXIT_FAILURE);
    }
}

/* A swapchain of the size, presenting FRAMES frames each cleared to a colour. */
static VkSwapchainKHR present_at(const struct context *c, uint32_t width, uint32_t height,
                                 VkSwapchainKHR old)
{
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = c->surface,
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
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(c->device, &info, NULL, &swapchain));
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(c->device, old, NULL);

    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(c->device, swapchain, &image_count, NULL));
    VkImage images[8];
    if (image_count > 8)
        image_count = 8;
    CHECK(vkGetSwapchainImagesKHR(c->device, swapchain, &image_count, images));

    VkSemaphore acquired, rendered;
    const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(vkCreateSemaphore(c->device, &semaphore_info, NULL, &acquired));
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
        CHECK(vkAcquireNextImageKHR(c->device, swapchain, UINT64_C(5000000000), acquired,
                                    VK_NULL_HANDLE, &index));
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
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait_stage,
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
    vkDestroySemaphore(c->device, acquired, NULL);
    printf("presented %u frames at %ux%u through %u images\n", FRAMES, width, height, image_count);
    return swapchain;
}

int main(void)
{
    /* GTA IV's display menu (1920x1080, 1280x720, 1440x960, back to
     * 1920x1080), then 4:3, a size above 1080p, and 4K. */
    static const VkExtent2D sizes[] = {
        {1920, 1080}, {1280, 720},  {1440, 960},  {1920, 1080},
        {800, 600},   {2560, 1440}, {3840, 2160}, {1920, 1080},
    };
    struct context c;
    memset(&c, 0, sizeof(c));
    make_context(&c);
    check_capabilities(&c);

    /* As DXVK resets: the swapchain destroyed before the next is made. */
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        VkSwapchainKHR swapchain = present_at(&c, sizes[i].width, sizes[i].height, VK_NULL_HANDLE);
        vkDestroySwapchainKHR(c.device, swapchain, NULL);
    }
    /* Through oldSwapchain. */
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        swapchain = present_at(&c, sizes[i].width, sizes[i].height, swapchain);
    vkDestroySwapchainKHR(c.device, swapchain, NULL);

    vkDestroyCommandPool(c.device, c.pool, NULL);
    vkDestroyDevice(c.device, NULL);
    vkDestroySurfaceKHR(c.instance, c.surface, NULL);
    vkDestroyInstance(c.instance, NULL);
    printf("test_videoout_swapchain: pass\n");
    return EXIT_SUCCESS;
}
