#include <stdio.h>

int run_controller_state_tests(void);
int run_cloud_api_tests(void);
int run_cloud_machine_selection_tests(void);
int run_machine_link_policy_tests(void);
int run_setup_portal_page_tests(void);
int run_setup_portal_route_tests(void);

int main(void) {
  fprintf(stderr, "Running host-side firmware tests\n");

  if (run_controller_state_tests() != 0) {
    return 1;
  }
  if (run_cloud_api_tests() != 0) {
    return 1;
  }
  if (run_cloud_machine_selection_tests() != 0) {
    return 1;
  }
  if (run_machine_link_policy_tests() != 0) {
    return 1;
  }
  if (run_setup_portal_page_tests() != 0) {
    return 1;
  }
  if (run_setup_portal_route_tests() != 0) {
    return 1;
  }

  fprintf(stderr, "All host-side firmware tests passed\n");
  return 0;
}
