#include "cloud_session.h"

/*
 * Public cloud session façade.
 *
 * The externally visible API lives in cloud_session.h, while the concrete
 * implementations are split across:
 * - cloud_auth.c
 * - cloud_machine_api.c
 * - cloud_dashboard.c
 */
