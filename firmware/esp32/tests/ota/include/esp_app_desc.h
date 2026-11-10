#pragma once
#include <stdint.h>
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
typedef struct {
  uint32_t magic_word, secure_version, reserved1[2];
  char version[32], project_name[32], time[16], date[16], idf_ver[32];
  uint8_t app_elf_sha256[32];
  uint32_t reserved2[20];
} esp_app_desc_t;
_Static_assert(sizeof(esp_app_desc_t) == 256, "IDF app descriptor wire size");
const esp_app_desc_t *esp_app_get_description(void);
