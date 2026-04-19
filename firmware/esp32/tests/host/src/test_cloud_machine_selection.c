#include "cloud_machine_selection.h"
#include "test_support.h"

static int test_find_machine_by_serial_returns_matching_machine(void) {
  const lm_ctrl_cloud_machine_t fleet[] = {
    {.serial = "A1", .name = "First"},
    {.serial = "B2", .name = "Second"},
  };
  lm_ctrl_cloud_machine_t machine = {0};

  ASSERT_TRUE(lm_ctrl_cloud_find_machine_by_serial("B2", fleet, 2, &machine));
  ASSERT_STREQ("B2", machine.serial);
  ASSERT_STREQ("Second", machine.name);
  ASSERT_FALSE(lm_ctrl_cloud_find_machine_by_serial("Z9", fleet, 2, &machine));
  return 0;
}

static int test_resolve_effective_machine_prefers_explicit_selection(void) {
  const lm_ctrl_cloud_machine_t selected_machine = {
    .serial = "B2",
    .name = "Selected",
  };
  const lm_ctrl_cloud_machine_t fleet[] = {
    {.serial = "A1", .name = "Only"},
  };
  lm_ctrl_cloud_machine_t resolved_machine = {0};
  bool auto_selected = false;

  ASSERT_TRUE(
    lm_ctrl_cloud_resolve_effective_machine_selection(
      &selected_machine,
      true,
      fleet,
      1,
      &resolved_machine,
      &auto_selected
    )
  );
  ASSERT_STREQ("B2", resolved_machine.serial);
  ASSERT_FALSE(auto_selected);
  return 0;
}

static int test_resolve_effective_machine_auto_selects_single_fleet_entry(void) {
  const lm_ctrl_cloud_machine_t fleet[] = {
    {.serial = "A1", .name = "Only"},
  };
  lm_ctrl_cloud_machine_t resolved_machine = {0};
  bool auto_selected = false;

  ASSERT_TRUE(
    lm_ctrl_cloud_resolve_effective_machine_selection(
      NULL,
      false,
      fleet,
      1,
      &resolved_machine,
      &auto_selected
    )
  );
  ASSERT_STREQ("A1", resolved_machine.serial);
  ASSERT_TRUE(auto_selected);

  ASSERT_FALSE(
    lm_ctrl_cloud_resolve_effective_machine_selection(
      NULL,
      false,
      fleet,
      0,
      &resolved_machine,
      &auto_selected
    )
  );
  return 0;
}

int run_cloud_machine_selection_tests(void) {
  RUN_TEST(test_find_machine_by_serial_returns_matching_machine);
  RUN_TEST(test_resolve_effective_machine_prefers_explicit_selection);
  RUN_TEST(test_resolve_effective_machine_auto_selects_single_fleet_entry);
  return 0;
}
