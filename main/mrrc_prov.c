// main/mrrc_prov.c — first-time setup with no network of any kind.
//
// The device has no keyboard, no NFC reader and no Bluetooth, so the only way a
// phone can hand it credentials is for the device to become the access point:
//
//   device  : SoftAP "MRRC-Passport-XXXX" + HTTP on 192.168.4.1
//   phone   : joins that AP, opens http://192.168.4.1, fills the form
//   device  : stores to NVS, reboots into station mode
//
// The passive NTAG213 on the case cannot carry data back, but it can carry the
// URL, so tapping it can open the page once the phone has joined the AP.
//
// This runs only when there are no credentials at all, which means nothing else
// has claimed the radio yet: the Wi-Fi/TLS stack that the audio path needs is
// not up, so the ~15 KB this costs is affordable.
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "mrrc_cfg.h"
#include "mrrc_prov.h"
#include "mrrc_ui.h"

static const char *TAG = "prov";

#define PROV_AP_PASS "mrrcconfig"      /* 8 chars minimum for WPA2 */

static const char *PAGE =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>MRRC Passport setup</title>"
"<style>body{font:16px system-ui;margin:1.5em;background:#111;color:#eee}"
"label{display:block;margin:.9em 0 .3em}input{width:100%;padding:.5em;font:inherit;"
"background:#222;color:#eee;border:1px solid #444;border-radius:6px}"
"button{margin-top:1.4em;width:100%;padding:.8em;font:inherit;background:#e87a00;"
"border:0;border-radius:6px;color:#111;font-weight:600}</style>"
"<h2>MRRC Passport</h2>"
"<form method=post action=/save>"
"<label>Wi-Fi SSID</label><input name=ssid value='%s' required>"
"<label>Wi-Fi password</label><input name=pass type=password value='%s'>"
"<label>Server host</label><input name=host value='%s'>"
"<label>Server port</label><input name=port value='%d'>"
"<label>Server password</label><input name=srv_pass type=password value='%s'>"
"<button>Save and reboot</button></form>";

static char s_page[2048];

/* Pull one form field out of the urlencoded body.  Values are copied into a
 * fixed buffer: this page is on a device with 40 KB of contiguous heap and must
 * not be the thing that fragments it. */
static void field(const char *body, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    const size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || p[-1] == '&') && p[klen] == '=') break;
        p += klen;
    }
    if (!p) return;
    p += klen + 1;

    size_t n = 0;
    while (*p && *p != '&' && n + 1 < out_len) {
        char c = *p++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && p[0] && p[1]) {
            const char hex[3] = { p[0], p[1], 0 };
            c = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

static esp_err_t root_get(httpd_req_t *req)
{
    snprintf(s_page, sizeof s_page, PAGE, cfg_wifi_ssid(), cfg_wifi_pass(),
             cfg_srv_host(), cfg_srv_port(), cfg_srv_pass());
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req)
{
    static char body[512];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
    }
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    body[got] = '\0';

    char ssid[33], pass[65], host[64], spass[65], port[8];
    field(body, "ssid", ssid, sizeof ssid);
    field(body, "pass", pass, sizeof pass);
    field(body, "host", host, sizeof host);
    field(body, "port", port, sizeof port);
    field(body, "srv_pass", spass, sizeof spass);

    if (!ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    const int p = port[0] ? atoi(port) : 0;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!cfg_save(ssid, pass, host, p, spass)) {
        return httpd_resp_send(req, "<h3>Save failed (NVS)</h3><p>See the serial log.</p>", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send(req, "<h3>Saved. Rebooting…</h3>", HTTPD_RESP_USE_STRLEN);
    ESP_LOGW(TAG, "credentials saved (ssid=\"%s\"); restarting", ssid);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

void mrrc_prov_run(void)
{
    ESP_LOGW(TAG, "no credentials: starting setup AP");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    /* Unique SSID so two devices on a bench do not collide. */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = { 0 };
    /* Written straight into the field: a 33-byte staging buffer let the compiler
     * warn about truncation into the 32-byte field, and the name is 18 bytes. */
    const int name_len = snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid,
                                  "MRRC-Passport-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)name_len;
    snprintf((char *)ap.ap.password, sizeof ap.ap.password, "%s", PROV_AP_PASS);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd failed; reboot in 10 s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);

    const char *name = (const char *)ap.ap.ssid;
    ESP_LOGW(TAG, "setup: join \"%s\" (pass %s) then open http://192.168.4.1",
             name, PROV_AP_PASS);

    /* Show it on the panel too: the operator has no other way to read the SSID. */
    mrrc_ui_setup_hint(name, PROV_AP_PASS, "192.168.4.1");

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));   /* reboots from save_post */
}
