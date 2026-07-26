#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1U << 0)
#define MALLOC_CAP_INTERNAL (1U << 1)
#define MALLOC_CAP_DMA (1U << 2)
#define MALLOC_CAP_SPIRAM (1U << 3)

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t test_heap_caps_get_last_realloc_caps(void);
