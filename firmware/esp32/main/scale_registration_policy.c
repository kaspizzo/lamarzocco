#include "scale_registration_policy.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>

lm_scale_registration_t lm_scale_registration_classify(const char *response, const char *name) {
  if (!response || !name || !name[0]) return LM_SCALE_REGISTRATION_UNKNOWN;
  cJSON *scales = cJSON_ParseWithOpts(response, NULL, true);
  lm_scale_registration_t result = LM_SCALE_REGISTRATION_MISSING;
  if (!cJSON_IsArray(scales)) result = LM_SCALE_REGISTRATION_UNKNOWN;
  else {
    cJSON *entry;
    bool own = false, other = false, invalid = false;
    cJSON_ArrayForEach(entry, scales) {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(entry, "name");
      if (!cJSON_IsString(n) || !n->valuestring[0]) invalid = true;
      else if (!strcmp(n->valuestring, name)) own = true;
      else other = true;
    }
    if (invalid) result = LM_SCALE_REGISTRATION_UNKNOWN;
    else if (own) result = LM_SCALE_REGISTRATION_PRESENT;
    else if (other) result = LM_SCALE_REGISTRATION_OTHER;
  }
  cJSON_Delete(scales);
  return result;
}
uint32_t lm_scale_registration_retry_ms(unsigned failures) {
  return 30000U << (failures < 3 ? failures : 3);
}
