// main/mrrc_cfg.c — Kconfig defaults, NVS overrides.
//
// The provisioning page (mrrc_prov.c) writes into the "cfg" namespace; nothing
// else does.  Reads are cached on first use because the hot paths (WS URI
// construction) touch them, and NVS reads are not free.
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mrrc_cfg.h"

static const char *TAG = "cfg";

static bool cfg_save_once(const char *ssid, const char *pass, const char *host,
                          int port, const char *srv_pass);
#define CFG_NS "cfg"

typedef struct {
    bool loaded;
    char ssid[33];
    char pass[65];
    char host[64];
    int  port;
    char srv_pass[65];
} cfg_t;

static cfg_t s_cfg;

static void load_once(void)
{
    if (s_cfg.loaded) return;
    s_cfg.loaded = true;

    /* Defaults from Kconfig. */
    snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "%s", CONFIG_MRRC_WIFI_SSID);
    snprintf(s_cfg.pass, sizeof s_cfg.pass, "%s", CONFIG_MRRC_WIFI_PASSWORD);
    snprintf(s_cfg.host, sizeof s_cfg.host, "%s", CONFIG_MRRC_SERVER_HOST);
    s_cfg.port = CONFIG_MRRC_SERVER_PORT;
    snprintf(s_cfg.srv_pass, sizeof s_cfg.srv_pass, "%s", CONFIG_MRRC_SERVER_PASSWORD);

    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t n;
    n = sizeof s_cfg.ssid;
    if (nvs_get_str(h, "ssid", s_cfg.ssid, &n) == ESP_OK && s_cfg.ssid[0])
        ESP_LOGI(TAG, "wifi ssid from NVS: \"%s\"", s_cfg.ssid);
    n = sizeof s_cfg.pass;
    (void)nvs_get_str(h, "pass", s_cfg.pass, &n);
    n = sizeof s_cfg.host;
    if (nvs_get_str(h, "host", s_cfg.host, &n) == ESP_OK && s_cfg.host[0])
        ESP_LOGI(TAG, "server host from NVS: \"%s\"", s_cfg.host);
    int32_t p = 0;
    if (nvs_get_i32(h, "port", &p) == ESP_OK && p > 0 && p < 65536) s_cfg.port = (int)p;
    n = sizeof s_cfg.srv_pass;
    (void)nvs_get_str(h, "srv_pass", s_cfg.srv_pass, &n);

    nvs_close(h);
}

const char *cfg_wifi_ssid(void)   { load_once(); return s_cfg.ssid; }
const char *cfg_wifi_pass(void)   { load_once(); return s_cfg.pass; }
const char *cfg_srv_host(void)    { load_once(); return s_cfg.host; }
int         cfg_srv_port(void)    { load_once(); return s_cfg.port; }
const char *cfg_srv_pass(void)    { load_once(); return s_cfg.srv_pass; }
bool        cfg_has_wifi(void)    { load_once(); return s_cfg.ssid[0] != '\0'; }

static void log_nvs_stats(const char *when)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
        ESP_LOGI(TAG, "NVS %s: used=%u free=%u total=%u", when,
                 (unsigned)st.used_entries, (unsigned)st.free_entries,
                 (unsigned)st.total_entries);
}

bool cfg_save(const char *ssid, const char *pass, const char *host,
              int port, const char *srv_pass)
{
    log_nvs_stats("before save");
    if (!cfg_save_once(ssid, pass, host, port, srv_pass)) {
        /* Most likely cause on this board: the Wi-Fi driver writes a
         * sta.pwr_reset entry and an sta.apinfo blob per boot, and 24 KB of NVS
         * fills up after enough flashes.  Setup is the one moment where wiping
         * everything is acceptable — the credentials on the page are about to
         * replace what is here anyway. */
        ESP_LOGW(TAG, "save failed; erasing NVS and retrying once");
        esp_err_t e = nvs_flash_erase();
        if (e == ESP_OK) e = nvs_flash_init();
        ESP_LOGW(TAG, "nvs reinit: %s", esp_err_to_name(e));
        if (e != ESP_OK) return false;
        s_cfg.loaded = false;
        return cfg_save_once(ssid, pass, host, port, srv_pass);
    }
    return true;
}

static bool cfg_save_once(const char *ssid, const char *pass, const char *host,
                          int port, const char *srv_pass)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    bool ok = true;
    if (ssid && ssid[0])      ok &= nvs_set_str(h, "ssid", ssid) == ESP_OK;
    if (pass)                 ok &= nvs_set_str(h, "pass", pass) == ESP_OK;
    if (host && host[0])      ok &= nvs_set_str(h, "host", host) == ESP_OK;
    if (port > 0)             ok &= nvs_set_i32(h, "port", port) == ESP_OK;
    if (srv_pass)             ok &= nvs_set_str(h, "srv_pass", srv_pass) == ESP_OK;
    ok &= nvs_commit(h) == ESP_OK;
    nvs_close(h);

    s_cfg.loaded = false;      /* re-read next time */
    ESP_LOGW(TAG, "saved: ssid=\"%s\" host=\"%s\" port=%d ok=%d",
             ssid ? ssid : "", host ? host : "", port, (int)ok);
    return ok;
}

void cfg_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
    s_cfg.loaded = false;
    ESP_LOGW(TAG, "NVS overrides erased; falling back to Kconfig");
}
