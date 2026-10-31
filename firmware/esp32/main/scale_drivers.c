#include "scale_driver.h"
#include <string.h>

extern const lm_scale_driver_t lm_scale_acaia_legacy_driver;
extern const lm_scale_driver_t lm_scale_acaia_2021_driver;
extern const lm_scale_driver_t lm_scale_bookoo_mini_driver;
extern const lm_scale_driver_t lm_scale_bookoo_ultra_driver;

const lm_scale_driver_t *lm_scale_driver_find(const char *id) {
  const lm_scale_driver_t *drivers[] = {
    &lm_scale_acaia_legacy_driver, &lm_scale_acaia_2021_driver,
    &lm_scale_bookoo_mini_driver, &lm_scale_bookoo_ultra_driver,
  };
  if (!id) return NULL;
  for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); ++i)
    if (strcmp(id, drivers[i]->id) == 0) return drivers[i];
  return NULL;
}
