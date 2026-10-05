/* PS5 Vulkan - where a VideoOut swapchain's images go (PS5_Mesa's
 * src/vulkan/wsi/wsi_common_videoout_scale.h), checked on the host.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tools/test-videoout-wsi.sh with the fork's wsi directory on
 * the include path. */
#include <stdio.h>
#include <stdlib.h>

#include "wsi_common_videoout_scale.h"

static int failures;

static void check_target(unsigned width, unsigned height, unsigned want_width, unsigned want_height)
{
    const VkExtent2D got = wsi_videoout_target_extent((VkExtent2D){width, height});
    if (got.width != want_width || got.height != want_height)
    {
        fprintf(stderr, "target of %ux%u: %ux%u, want %ux%u\n", width, height, got.width,
                got.height, want_width, want_height);
        failures++;
    }
}

static void check_rect(unsigned width, unsigned height, unsigned target_width,
                       unsigned target_height, int x, int y, unsigned want_width,
                       unsigned want_height, int covers)
{
    const VkExtent2D target = {target_width, target_height};
    const VkRect2D got = wsi_videoout_fit_rect((VkExtent2D){width, height}, target);
    if (got.offset.x != x || got.offset.y != y || got.extent.width != want_width ||
        got.extent.height != want_height)
    {
        fprintf(stderr, "%ux%u in %ux%u: %ux%u at (%d, %d), want %ux%u at (%d, %d)\n", width,
                height, target_width, target_height, got.extent.width, got.extent.height,
                got.offset.x, got.offset.y, want_width, want_height, x, y);
        failures++;
    }
    if (wsi_videoout_rect_covers(got, target) != (covers != 0))
    {
        fprintf(stderr, "%ux%u in %ux%u: covers %d, want %d\n", width, height, target_width,
                target_height, wsi_videoout_rect_covers(got, target), covers);
        failures++;
    }
    /* Inside the target, and centred to the pixel. */
    if (got.offset.x < 0 || got.offset.y < 0 || got.offset.x + got.extent.width > target_width ||
        got.offset.y + got.extent.height > target_height)
    {
        fprintf(stderr, "%ux%u in %ux%u: outside the target\n", width, height, target_width,
                target_height);
        failures++;
    }
}

int main(void)
{
    /* The taken sizes are their own framebuffers. */
    if (!wsi_videoout_size_taken((VkExtent2D){1920, 1080}) ||
        !wsi_videoout_size_taken((VkExtent2D){3840, 2160}))
    {
        fprintf(stderr, "1920x1080 and 3840x2160 are taken sizes\n");
        failures++;
    }
    /* The sizes VideoOut refused (0x80290005) are not. */
    if (wsi_videoout_size_taken((VkExtent2D){1280, 720}) ||
        wsi_videoout_size_taken((VkExtent2D){1440, 960}) ||
        wsi_videoout_size_taken((VkExtent2D){1440, 1080}))
    {
        fprintf(stderr, "1280x720, 1440x960 and 1440x1080 are refused sizes\n");
        failures++;
    }
    check_target(1920, 1080, 1920, 1080);
    check_target(3840, 2160, 3840, 2160);
    check_target(1280, 720, 1920, 1080);
    check_target(1440, 960, 1920, 1080);
    check_target(1440, 1080, 1920, 1080);
    check_target(800, 600, 1920, 1080);
    check_target(1, 1, 1920, 1080);
    check_target(1920, 1200, 3840, 2160);
    check_target(2560, 1440, 3840, 2160);
    check_target(1921, 1080, 3840, 2160);
    check_target(1920, 1081, 3840, 2160);

    /* 16:9 fills the target. */
    check_rect(1280, 720, 1920, 1080, 0, 0, 1920, 1080, 1);
    check_rect(1920, 1080, 1920, 1080, 0, 0, 1920, 1080, 1);
    check_rect(2560, 1440, 3840, 2160, 0, 0, 3840, 2160, 1);
    /* 3:2, 4:3 and 5:4 pillarboxed, centred. */
    check_rect(1440, 960, 1920, 1080, 150, 0, 1620, 1080, 0);
    check_rect(1440, 1080, 1920, 1080, 240, 0, 1440, 1080, 0);
    check_rect(800, 600, 1920, 1080, 240, 0, 1440, 1080, 0);
    check_rect(1280, 1024, 1920, 1080, 285, 0, 1350, 1080, 0);
    check_rect(1920, 1200, 3840, 2160, 192, 0, 3456, 2160, 0);
    /* Wider than 16:9 letterboxed. */
    check_rect(1920, 800, 1920, 1080, 0, 140, 1920, 800, 0);
    check_rect(2560, 1080, 1920, 1080, 0, 135, 1920, 810, 0);
    /* Degenerate sizes stay inside. */
    check_rect(1, 1, 1920, 1080, 420, 0, 1080, 1080, 0);
    check_rect(1, 1000, 1920, 1080, 959, 0, 1, 1080, 0);

    if (failures)
    {
        fprintf(stderr, "test_videoout_scale: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_videoout_scale: pass\n");
    return EXIT_SUCCESS;
}
