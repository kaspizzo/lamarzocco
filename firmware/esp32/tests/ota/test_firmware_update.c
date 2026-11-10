#include "test_httpd.h"
#include "test_support.h"
#include <stdint.h>
static int receive_fragment(httpd_req_t *, char *, size_t);
#define httpd_req_recv receive_fragment
#include "../../main/firmware_update.c"
#undef httpd_req_recv

static esp_partition_t running = {.address=0x30000,.size=0x480000,.label="ota_0",.subtype=0x10};
static esp_partition_t target = {.address=0x4b0000,.size=0x480000,.label="ota_1"};
static esp_app_desc_t app;
static esp_ota_img_states_t boot_state;
static esp_err_t end_result, write_result, select_result, mark_result;
static unsigned begins, ends, aborts, selections, reboots, marks;
static int64_t fake_time;
static size_t written, disconnect_at;
static uint8_t flash[8192];
static bool same_slot;
static unsigned guard_starts, guard_stops, hard_reboots;
static void (*guard_callback)(void *);
void esp_restart(void) { ++hard_reboots; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
  guard_callback=args->callback; *timer=(void *)1; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us) { ++guard_starts; return us==120000000?ESP_OK:ESP_FAIL; }
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { ++guard_stops; return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t timer) { return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) { return same_slot ? &running : &target; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) { *s=boot_state; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { ++marks; return mark_result; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) { ++begins; *h=1; return p == &target ? ESP_OK : ESP_FAIL; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *b, size_t n) {
  if (write_result != ESP_OK || written+n > sizeof(flash)) return ESP_FAIL;
  memcpy(flash+written,b,n); written+=n; return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) { ++ends; return end_result; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { ++aborts; return ESP_OK; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d) { *d=app; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { ++selections; return select_result; }
esp_err_t lm_ctrl_wifi_schedule_reboot(void) { ++reboots; return ESP_OK; }
int64_t esp_timer_get_time(void) { return fake_time; }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "test error"; }
static int receive_fragment(httpd_req_t *r, char *b, size_t n) {
  if (r->request_body_offset >= disconnect_at) return -1;
  if (n > 17) n=17; /* Includes app descriptor/header fragments. */
  return httpd_req_recv(r,b,n);
}
static void reset(void) {
  begins=ends=aborts=selections=reboots=marks=0;
  written=0; disconnect_at=SIZE_MAX; same_slot=false;
  end_result=write_result=select_result=mark_result=ESP_OK;
  memset(&app,0,sizeof(app));
  app.magic_word=ESP_APP_DESC_MAGIC_WORD;
  strcpy(app.project_name,"lamarzocco_knob_controller");
  strcpy(app.version,"test-version");
  boot_state=ESP_OTA_IMG_PENDING_VERIFY;
  fake_time=1; lm_ctrl_firmware_boot_ready();
  fake_time=10000001; lm_ctrl_firmware_boot_tick();
}
static httpd_req_t *request(void) {
  httpd_req_t *r=test_httpd_request_create();
  r->request_body=calloc(1,1000); r->request_body_size=r->content_len=1000;
  esp_image_header_t h={.magic=ESP_IMAGE_HEADER_MAGIC,.chip_id=ESP_CHIP_ID_ESP32S3,.hash_appended=1};
  memcpy(r->request_body,&h,sizeof(h));
  memcpy(r->request_body+32,&app,sizeof(app));
  memcpy(r->request_body+DESC_END,lm_ctrl_firmware_update_abi,16);
  test_httpd_request_set_header(r,"Content-Type","application/octet-stream");
  return r;
}
static int valid_fragmented_upload(void) {
  reset(); httpd_req_t *r=request();
  ASSERT_EQ_INT(ESP_OK,lm_ctrl_firmware_upload(r));
  ASSERT_EQ_INT(1000,written); ASSERT_TRUE(!memcmp(flash,r->request_body,1000));
  ASSERT_EQ_INT(1,begins); ASSERT_EQ_INT(1,ends); ASSERT_EQ_INT(0,aborts);
  ASSERT_EQ_INT(1,selections); ASSERT_EQ_INT(1,reboots);
  ASSERT_CONTAINS(test_httpd_request_body(r),"\"restarting\":true");
  httpd_req_t *second=request();
  ASSERT_EQ_INT(ESP_FAIL,lm_ctrl_firmware_upload(second)); ASSERT_EQ_INT(1,begins);
  test_httpd_request_destroy(second);
  test_httpd_request_destroy(r); return 0;
}
static int incompatible_images_never_erase(void) {
  const size_t corrupt[] = {0,12,23,32,80,DESC_END};
  for (unsigned i=0;i<sizeof(corrupt)/sizeof(corrupt[0]);++i) {
    reset(); httpd_req_t *r=request(); r->request_body[corrupt[i]]^=0x40;
    ASSERT_EQ_INT(ESP_FAIL,lm_ctrl_firmware_upload(r)); ASSERT_EQ_INT(0,begins);
    ASSERT_EQ_INT(0,selections); ASSERT_EQ_INT(0,reboots); test_httpd_request_destroy(r);
  }
  return 0;
}
static int failed_stream_or_digest_never_selects_boot(void) {
  for (unsigned i=0;i<3;++i) {
    reset(); httpd_req_t *r=request();
    if (i==0) disconnect_at=400;
    if (i==1) write_result=ESP_FAIL;
    if (i==2) end_result=ESP_FAIL;
    ASSERT_EQ_INT(ESP_FAIL,lm_ctrl_firmware_upload(r));
    ASSERT_EQ_INT(0,selections); ASSERT_EQ_INT(0,reboots);
    ASSERT_EQ_INT(i==2?0:1,aborts); test_httpd_request_destroy(r);
  }
  return 0;
}
static int startup_size_and_active_slot_guards(void) {
  for (unsigned i=0;i<3;++i) {
    reset(); httpd_req_t *r=request();
    if (i==0) { fake_time=1; lm_ctrl_firmware_boot_ready(); }
    if (i==1) r->content_len=target.size+1;
    if (i==2) same_slot=true;
    ASSERT_EQ_INT(ESP_FAIL,lm_ctrl_firmware_upload(r));
    ASSERT_EQ_INT(0,begins); ASSERT_EQ_INT(0,r->request_body_offset);
    test_httpd_request_destroy(r);
  }
  return 0;
}
static int boot_confirmation_requires_health_window_and_success(void) {
  reset(); fake_time=1; lm_ctrl_firmware_boot_ready(); marks=0;
  fake_time=9999999; lm_ctrl_firmware_boot_tick(); ASSERT_EQ_INT(0,marks);
  mark_result=ESP_FAIL; fake_time=10000001; lm_ctrl_firmware_boot_tick();
  ASSERT_EQ_INT(1,marks); ASSERT_FALSE(s_boot_confirmed);
  mark_result=ESP_OK; fake_time=15000001; lm_ctrl_firmware_boot_tick();
  ASSERT_EQ_INT(2,marks); ASSERT_TRUE(s_boot_confirmed); return 0;
}
static int pending_startup_deadline_reboots_unconfirmed_image(void) {
  reset(); s_boot_guard=NULL; guard_starts=guard_stops=hard_reboots=0;
  ASSERT_EQ_INT(ESP_OK,lm_ctrl_firmware_boot_begin()); ASSERT_EQ_INT(1,guard_starts);
  lm_ctrl_firmware_boot_ready(); guard_callback(NULL); ASSERT_EQ_INT(1,hard_reboots);
  fake_time+=10000000; lm_ctrl_firmware_boot_tick(); ASSERT_EQ_INT(1,guard_stops);
  guard_callback(NULL); ASSERT_EQ_INT(1,hard_reboots);
  boot_state=ESP_OTA_IMG_VALID;
  ASSERT_EQ_INT(ESP_OK,lm_ctrl_firmware_boot_begin()); ASSERT_EQ_INT(1,guard_starts);
  return 0;
}
int main(void) {
  RUN_TEST(valid_fragmented_upload);
  RUN_TEST(incompatible_images_never_erase);
  RUN_TEST(failed_stream_or_digest_never_selects_boot);
  RUN_TEST(startup_size_and_active_slot_guards);
  RUN_TEST(boot_confirmation_requires_health_window_and_success);
  RUN_TEST(pending_startup_deadline_reboots_unconfirmed_image);
  return 0;
}
