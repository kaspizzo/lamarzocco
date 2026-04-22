#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LM_CTRL_CLOUD_HOST "lion.lamarzocco.io"
#define LM_CTRL_CLOUD_PORT 443
#define LM_CTRL_CLOUD_AUTH_INIT_PATH "/api/customer-app/auth/init"
#define LM_CTRL_CLOUD_AUTH_SIGNIN_PATH "/api/customer-app/auth/signin"
#define LM_CTRL_CLOUD_THINGS_PATH "/api/customer-app/things"
#define LM_CTRL_CLOUD_WS_URI "wss://lion.lamarzocco.io/ws/connect"
#define LM_CTRL_CLOUD_WS_DEST_PREFIX "/ws/sn/"
#define LM_CTRL_CLOUD_WS_DEST_SUFFIX "/dashboard"
#define LM_CTRL_CLOUD_MAX_FLEET 8
#define LM_CTRL_INSTALLATION_ID_LEN 37
#define LM_CTRL_PRIVATE_KEY_DER_MAX 256
#define LM_CTRL_CLOUD_SECRET_LEN 32
#define LM_CTRL_CLOUD_WS_HEADER_BUFFER_LEN 768
#define LM_CTRL_CLOUD_WS_TOKEN_LEN 1024
#define LM_CTRL_CLOUD_WS_FRAME_MAX 8192
#define LM_CTRL_CLOUD_WS_SUBSCRIPTION_ID "lm-dashboard"
#define LM_CTRL_CLOUD_COMMAND_UPDATE_MAX 8
#define LM_CTRL_CLOUD_ACCESS_TOKEN_CACHE_FALLBACK_US (30LL * 1000LL * 1000LL)
#define LM_CTRL_CLOUD_ACCESS_TOKEN_EXP_SAFETY_MS (60LL * 1000LL)
#define LM_CTRL_CLOUD_PROVISIONING_SCHEMA_VERSION 1

typedef struct {
  uint8_t schema_version;
  uint8_t reserved0;
  uint16_t private_key_der_len;
  char installation_id[LM_CTRL_INSTALLATION_ID_LEN];
  uint8_t secret[LM_CTRL_CLOUD_SECRET_LEN];
  uint8_t private_key_der[LM_CTRL_PRIVATE_KEY_DER_MAX];
} lm_ctrl_cloud_provisioning_blob_t;

#ifdef __cplusplus
}
#endif
