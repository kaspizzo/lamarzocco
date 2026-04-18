#include "setup_portal_page.h"
#include "test_httpd.h"
#include "test_support.h"

#include <string.h>

static void init_view(lm_ctrl_setup_portal_view_t *view) {
  memset(view, 0, sizeof(*view));
  strcpy(view->status_html, "ready");
  strcpy(view->local_url_html, "http://controller.local");
  strcpy(view->hostname_html, "lm-controller");
  strcpy(view->ssid_html, "MyWifi");
  view->info.language = CTRL_LANGUAGE_EN;
  ctrl_state_init((ctrl_state_t *)&view->presets); /* placate default preset names below */
}

static int test_history_target_maps_known_sections(void) {
  ASSERT_STREQ("/#controller-section", lm_ctrl_setup_portal_history_target("/controller"));
  ASSERT_STREQ("/#cloud-section", lm_ctrl_setup_portal_history_target("/cloud-machine"));
  ASSERT_TRUE(lm_ctrl_setup_portal_history_target("/unknown") == NULL);
  return 0;
}

static int test_rendered_page_escapes_fleet_and_preset_values(void) {
  lm_ctrl_setup_portal_view_t view = {0};
  httpd_req_t *req = test_httpd_request_create();
  const char *body;

  ASSERT_TRUE(req != NULL);
  strcpy(view.status_html, "ready");
  strcpy(view.local_url_html, "http://controller.local");
  strcpy(view.hostname_html, "lm-controller");
  strcpy(view.ssid_html, "MyWifi");
  view.info.language = CTRL_LANGUAGE_EN;
  view.info.has_cloud_credentials = true;
  view.fleet_count = 1;
  strcpy(view.fleet[0].serial, "LM<123>");
  strcpy(view.fleet[0].name, "Micra <Kitchen> & \"Bar\"");
  strcpy(view.fleet[0].model, "Mini & More");
  strcpy(view.presets[0].name, "Preset <One> & \"Two\"");
  view.dashboard_feature_mask = 0;

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_setup_portal_send_page(req, &view, "/#cloud-section"));
  ASSERT_STREQ("text/html; charset=utf-8", test_httpd_request_type(req));
  body = test_httpd_request_body(req);

  ASSERT_CONTAINS(body, "Micra &lt;Kitchen&gt; &amp; &quot;Bar&quot;");
  ASSERT_CONTAINS(body, "LM&lt;123&gt;");
  ASSERT_CONTAINS(body, "Preset &lt;One&gt; &amp; &quot;Two&quot;");
  ASSERT_NOT_CONTAINS(body, "Micra <Kitchen>");
  ASSERT_NOT_CONTAINS(body, "Preset <One>");
  ASSERT_CONTAINS(body, "history.replaceState(null,'','/#cloud-section');");
  ASSERT_NOT_CONTAINS(body, "action=\"/bbw\"");

  test_httpd_request_destroy(req);
  return 0;
}

static int test_rendered_page_shows_bbw_section_when_feature_is_present(void) {
  lm_ctrl_setup_portal_view_t view = {0};
  httpd_req_t *req = test_httpd_request_create();
  const char *body;

  ASSERT_TRUE(req != NULL);
  strcpy(view.status_html, "ready");
  strcpy(view.local_url_html, "http://controller.local");
  strcpy(view.hostname_html, "lm-controller");
  strcpy(view.ssid_html, "MyWifi");
  view.info.language = CTRL_LANGUAGE_EN;
  view.dashboard_feature_mask = LM_CTRL_MACHINE_FEATURE_BBW;
  view.dashboard_loaded_mask = LM_CTRL_MACHINE_FIELD_BBW_MODE | LM_CTRL_MACHINE_FIELD_BBW_DOSE_1 | LM_CTRL_MACHINE_FIELD_BBW_DOSE_2;
  view.dashboard_values.bbw_mode = CTRL_BBW_MODE_CONTINUOUS;
  view.dashboard_values.bbw_dose_1_g = 30.0f;
  view.dashboard_values.bbw_dose_2_g = 35.0f;

  ASSERT_EQ_INT(ESP_OK, lm_ctrl_setup_portal_send_page(req, &view, NULL));
  body = test_httpd_request_body(req);
  ASSERT_CONTAINS(body, "action=\"/bbw\"");
  ASSERT_CONTAINS(body, ">Continuous</option>");

  test_httpd_request_destroy(req);
  return 0;
}

int run_setup_portal_page_tests(void) {
  RUN_TEST(test_history_target_maps_known_sections);
  RUN_TEST(test_rendered_page_escapes_fleet_and_preset_values);
  RUN_TEST(test_rendered_page_shows_bbw_section_when_feature_is_present);
  return 0;
}
