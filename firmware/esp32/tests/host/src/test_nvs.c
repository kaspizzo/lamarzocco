#include "nvs.h"
#include "test_nvs.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NVS_MAX_HANDLES 8
#define TEST_NVS_MAX_ENTRIES 64

typedef enum {
  TEST_NVS_TYPE_NONE = 0,
  TEST_NVS_TYPE_STR,
  TEST_NVS_TYPE_BLOB,
  TEST_NVS_TYPE_U8,
  TEST_NVS_TYPE_U32,
} test_nvs_type_t;

typedef struct {
  bool used;
  char namespace_name[32];
  int mode;
} test_nvs_handle_slot_t;

typedef struct {
  bool used;
  char namespace_name[32];
  char key[32];
  test_nvs_type_t type;
  uint8_t value_u8;
  uint32_t value_u32;
  void *blob;
  size_t blob_size;
} test_nvs_entry_t;

static test_nvs_handle_slot_t s_handles[TEST_NVS_MAX_HANDLES];
static test_nvs_entry_t s_entries[TEST_NVS_MAX_ENTRIES];

static test_nvs_handle_slot_t *lookup_handle(nvs_handle_t handle) {
  if (handle <= 0 || handle > TEST_NVS_MAX_HANDLES) {
    return NULL;
  }

  if (!s_handles[handle - 1].used) {
    return NULL;
  }

  return &s_handles[handle - 1];
}

static test_nvs_entry_t *find_entry(const char *namespace_name, const char *key) {
  for (size_t i = 0; i < TEST_NVS_MAX_ENTRIES; ++i) {
    if (!s_entries[i].used) {
      continue;
    }
    if (strcmp(s_entries[i].namespace_name, namespace_name) == 0 &&
        strcmp(s_entries[i].key, key) == 0) {
      return &s_entries[i];
    }
  }

  return NULL;
}

static test_nvs_entry_t *find_or_create_entry(const char *namespace_name, const char *key) {
  test_nvs_entry_t *entry = find_entry(namespace_name, key);

  if (entry != NULL) {
    return entry;
  }

  for (size_t i = 0; i < TEST_NVS_MAX_ENTRIES; ++i) {
    if (s_entries[i].used) {
      continue;
    }

    s_entries[i].used = true;
    strncpy(s_entries[i].namespace_name, namespace_name, sizeof(s_entries[i].namespace_name) - 1);
    strncpy(s_entries[i].key, key, sizeof(s_entries[i].key) - 1);
    return &s_entries[i];
  }

  return NULL;
}

static void clear_entry(test_nvs_entry_t *entry) {
  if (entry == NULL) {
    return;
  }

  free(entry->blob);
  memset(entry, 0, sizeof(*entry));
}

void test_nvs_reset(void) {
  for (size_t i = 0; i < TEST_NVS_MAX_ENTRIES; ++i) {
    clear_entry(&s_entries[i]);
  }
  memset(s_handles, 0, sizeof(s_handles));
}

esp_err_t test_nvs_seed_blob(const char *namespace_name, const char *key, const void *value, size_t length) {
  test_nvs_entry_t *entry;
  void *copy;

  if (namespace_name == NULL || key == NULL || (value == NULL && length != 0)) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_or_create_entry(namespace_name, key);
  if (entry == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(entry->blob);
  entry->blob = NULL;
  entry->blob_size = 0;
  entry->type = TEST_NVS_TYPE_BLOB;

  if (length == 0) {
    return ESP_OK;
  }

  copy = malloc(length);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }

  memcpy(copy, value, length);
  entry->blob = copy;
  entry->blob_size = length;
  return ESP_OK;
}

esp_err_t test_nvs_seed_str(const char *namespace_name, const char *key, const char *value) {
  if (value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return test_nvs_seed_blob(namespace_name, key, value, strlen(value) + 1U);
}

esp_err_t test_nvs_seed_u8(const char *namespace_name, const char *key, uint8_t value) {
  test_nvs_entry_t *entry;

  if (namespace_name == NULL || key == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_or_create_entry(namespace_name, key);
  if (entry == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(entry->blob);
  entry->blob = NULL;
  entry->blob_size = 0;
  entry->type = TEST_NVS_TYPE_U8;
  entry->value_u8 = value;
  return ESP_OK;
}

esp_err_t test_nvs_seed_u32(const char *namespace_name, const char *key, uint32_t value) {
  test_nvs_entry_t *entry;

  if (namespace_name == NULL || key == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_or_create_entry(namespace_name, key);
  if (entry == NULL) {
    return ESP_ERR_NO_MEM;
  }

  free(entry->blob);
  entry->blob = NULL;
  entry->blob_size = 0;
  entry->type = TEST_NVS_TYPE_U32;
  entry->value_u8 = 0;
  entry->value_u32 = value;
  return ESP_OK;
}

bool test_nvs_has_key(const char *namespace_name, const char *key) {
  return namespace_name != NULL && key != NULL && find_entry(namespace_name, key) != NULL;
}

esp_err_t nvs_open(const char *namespace_name, int open_mode, nvs_handle_t *out_handle) {
  if (namespace_name == NULL || out_handle == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < TEST_NVS_MAX_HANDLES; ++i) {
    if (s_handles[i].used) {
      continue;
    }

    s_handles[i].used = true;
    s_handles[i].mode = open_mode;
    strncpy(s_handles[i].namespace_name, namespace_name, sizeof(s_handles[i].namespace_name) - 1);
    *out_handle = (nvs_handle_t)(i + 1);
    return ESP_OK;
  }

  return ESP_ERR_NO_MEM;
}

void nvs_close(nvs_handle_t handle) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot != NULL) {
    memset(slot, 0, sizeof(*slot));
  }
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);
  test_nvs_entry_t *entry;

  if (slot == NULL || key == NULL || length == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_entry(slot->namespace_name, key);
  if (entry == NULL || entry->type != TEST_NVS_TYPE_BLOB) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  if (out_value == NULL) {
    *length = entry->blob_size;
    return ESP_OK;
  }
  if (*length < entry->blob_size) {
    return ESP_ERR_INVALID_SIZE;
  }

  memcpy(out_value, entry->blob, entry->blob_size);
  *length = entry->blob_size;
  return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);
  test_nvs_entry_t *entry;

  if (slot == NULL || key == NULL || length == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_entry(slot->namespace_name, key);
  if (entry == NULL || entry->type != TEST_NVS_TYPE_BLOB) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  if (out_value == NULL) {
    *length = entry->blob_size;
    return ESP_OK;
  }
  if (*length < entry->blob_size) {
    *length = entry->blob_size;
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  memcpy(out_value, entry->blob, entry->blob_size);
  *length = entry->blob_size;
  return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot == NULL || slot->mode != NVS_READWRITE) {
    return ESP_ERR_INVALID_STATE;
  }
  if (value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  return test_nvs_seed_str(slot->namespace_name, key, value);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot == NULL || slot->mode != NVS_READWRITE) {
    return ESP_ERR_INVALID_STATE;
  }

  return test_nvs_seed_blob(slot->namespace_name, key, value, length);
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);
  test_nvs_entry_t *entry;

  if (slot == NULL || key == NULL || out_value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_entry(slot->namespace_name, key);
  if (entry == NULL || entry->type != TEST_NVS_TYPE_U8) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  *out_value = entry->value_u8;
  return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot == NULL || slot->mode != NVS_READWRITE) {
    return ESP_ERR_INVALID_STATE;
  }

  return test_nvs_seed_u8(slot->namespace_name, key, value);
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);
  test_nvs_entry_t *entry;

  if (slot == NULL || key == NULL || out_value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_entry(slot->namespace_name, key);
  if (entry == NULL || entry->type != TEST_NVS_TYPE_U32) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  *out_value = entry->value_u32;
  return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot == NULL || slot->mode != NVS_READWRITE) {
    return ESP_ERR_INVALID_STATE;
  }

  return test_nvs_seed_u32(slot->namespace_name, key, value);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);
  test_nvs_entry_t *entry;

  if (slot == NULL || slot->mode != NVS_READWRITE || key == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  entry = find_entry(slot->namespace_name, key);
  if (entry == NULL) {
    return ESP_ERR_NVS_NOT_FOUND;
  }

  clear_entry(entry);
  return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
  test_nvs_handle_slot_t *slot = lookup_handle(handle);

  if (slot == NULL || slot->mode != NVS_READWRITE) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < TEST_NVS_MAX_ENTRIES; ++i) {
    if (!s_entries[i].used) {
      continue;
    }
    if (strcmp(s_entries[i].namespace_name, slot->namespace_name) == 0) {
      clear_entry(&s_entries[i]);
    }
  }

  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  return lookup_handle(handle) != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}
