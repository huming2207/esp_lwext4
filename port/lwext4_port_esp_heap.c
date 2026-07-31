/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lwext4_port_esp_heap.h"

#include <stdint.h>

#include <esp_heap_caps.h>

#include <ext4_config.h>

#if !CONFIG_USE_USER_MALLOC
#error "The ESP lwext4 heap port requires CONFIG_USE_USER_MALLOC=1"
#endif

#if (CONFIG_LWEXT4_USE_INTERNAL_SRAM + \
     CONFIG_LWEXT4_USE_PSRAM + \
     CONFIG_LWEXT4_USE_PREFER_PSRAM) != 1
#error "Select exactly one ESP lwext4 heap allocation mode"
#endif

#define LWEXT4_INTERNAL_CAPS (MALLOC_CAP_INTERNAL)
#define LWEXT4_PSRAM_CAPS    (MALLOC_CAP_SPIRAM)

void *ext4_user_malloc(size_t size)
{
#if CONFIG_LWEXT4_USE_INTERNAL_SRAM
    return heap_caps_malloc(size, LWEXT4_INTERNAL_CAPS);
#elif CONFIG_LWEXT4_USE_PSRAM
    return heap_caps_malloc(size, LWEXT4_PSRAM_CAPS);
#else
    return heap_caps_malloc_prefer(size, 2,
                                   LWEXT4_PSRAM_CAPS,
                                   LWEXT4_INTERNAL_CAPS);
#endif
}

void *ext4_user_calloc(size_t count, size_t size)
{
#if CONFIG_LWEXT4_USE_INTERNAL_SRAM
    return heap_caps_calloc(count, size, LWEXT4_INTERNAL_CAPS);
#elif CONFIG_LWEXT4_USE_PSRAM
    return heap_caps_calloc(count, size, LWEXT4_PSRAM_CAPS);
#else
    return heap_caps_calloc_prefer(count, size, 2,
                                   LWEXT4_PSRAM_CAPS,
                                   LWEXT4_INTERNAL_CAPS);
#endif
}

void *ext4_user_realloc(void *ptr, size_t size)
{
#if CONFIG_LWEXT4_USE_INTERNAL_SRAM
    return heap_caps_realloc(ptr, size, LWEXT4_INTERNAL_CAPS);
#elif CONFIG_LWEXT4_USE_PSRAM
    return heap_caps_realloc(ptr, size, LWEXT4_PSRAM_CAPS);
#else
    return heap_caps_realloc_prefer(ptr, size, 2,
                                    LWEXT4_PSRAM_CAPS,
                                    LWEXT4_INTERNAL_CAPS);
#endif
}

void ext4_user_free(void *ptr)
{
    heap_caps_free(ptr);
}
