// main/mrrc_cfg.h — device configuration with NVS overrides.
//
// Everything the device needs to reach the network and the server.  Values come
// from Kconfig (which is where the build-time defaults and the gitignored
// sdkconfig.defaults.local live), and anything written by the provisioning page
// overrides them at runtime.  That split is what lets a provisioned device work
// without a rebuild.
#pragma once

#include <stdbool.h>

const char *cfg_wifi_ssid(void);   /* "" when nothing is configured */
const char *cfg_wifi_pass(void);
const char *cfg_srv_host(void);
int         cfg_srv_port(void);
const char *cfg_srv_pass(void);

/* True when a station SSID is available from either layer. */
bool cfg_has_wifi(void);

/* Persist what the provisioning page collected.  Returns false only when NVS
 * itself refuses; the values are validated by the caller. */
bool cfg_save(const char *ssid, const char *pass, const char *host,
              int port, const char *srv_pass);

/* Drop the NVS layer so the next boot falls back to Kconfig. */
void cfg_clear(void);
