/*
 * SPDX-FileCopyrightText: 2026 esp_lwext4 contributors
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LWEXT4_PORT_ESP_HEAP_H_
#define LWEXT4_PORT_ESP_HEAP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * These are the allocator hooks selected by CONFIG_USE_USER_MALLOC in
 * lwext4's ext4_types.h.
 */
void *ext4_user_malloc(size_t size);
void *ext4_user_calloc(size_t count, size_t size);
void *ext4_user_realloc(void *ptr, size_t size);
void ext4_user_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* LWEXT4_PORT_ESP_HEAP_H_ */
