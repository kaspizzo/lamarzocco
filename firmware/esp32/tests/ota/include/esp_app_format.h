#pragma once
#include <stdint.h>
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_CHIP_ID_ESP32S3 9
typedef struct __attribute__((packed)) {
  uint8_t magic, segment_count, spi_mode, spi_config;
  uint32_t entry_addr;
  uint8_t pins[4];
  uint16_t chip_id;
  uint8_t min_rev;
  uint16_t min_full, max_full;
  uint8_t reserved[4], hash_appended;
} esp_image_header_t;
typedef struct { uint32_t load_addr, data_len; } esp_image_segment_header_t;
_Static_assert(sizeof(esp_image_header_t) == 24, "IDF image header wire size");
