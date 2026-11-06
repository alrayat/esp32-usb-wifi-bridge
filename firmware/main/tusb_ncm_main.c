/*
 * USB NCM <-> WiFi bridge with a small WiFi setup web page.
 *
 * Based on the ESP-IDF example "tusb_ncm" (Unlicense OR CC0-1.0).
 *
 * What is new compared to the original example:
 *  - The ESP32 has its own address on the USB link: 192.168.77.1
 *  - A tiny web server there lets you scan WiFi networks and type a password.
 *  - The chosen WiFi name/password is saved in flash (NVS) and used at boot.
 *    If nothing is saved, the values from menuconfig are used.
 *  - Traffic for 192.168.77.1 stays inside the ESP32. Everything else is
 *    bridged between USB and WiFi exactly like the original example.
 *
 * On the phone: open http://192.168.77.1 in Chrome
 * (the phone needs the address 192.168.77.2/24 on eth0, see the phone script).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"

#include "esp_netif.h"
#include "esp_http_server.h"

#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "USB_NCM";

/* Address of the ESP32 on the USB link (web page). */
static const uint8_t WEB_IP[4] = {192, 168, 77, 1};

static esp_netif_t *s_web_netif = NULL;
static SemaphoreHandle_t s_tx_lock = NULL;      /* only one task sends to USB at a time */
static volatile bool s_reconnect = true;        /* false while we scan or change WiFi on purpose */
static bool s_is_wifi_connected = false;        /* used by the USB receive callback */

/* ------------------------------------------------------------------ */
/* USB <-> WiFi bridge                                                 */
/* ------------------------------------------------------------------ */

static void usb_device_set_link_state(bool link_up)
{
    tud_network_link_state(0, link_up);
}

/* Frames arriving from the phone over USB. */
static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    bool *is_wifi_connected = ctx;
    const uint8_t *p = buffer;
    bool to_web = false;
    bool to_wifi = true;

    if (len >= 14) {
        uint16_t type = ((uint16_t)p[12] << 8) | p[13];
        if (type == 0x0806 && len >= 42) {
            /* ARP: answer questions about 192.168.77.1 (also forwarded, harmless) */
            if (memcmp(p + 38, WEB_IP, 4) == 0) {
                to_web = true;
            }
        } else if (type == 0x0800 && len >= 34) {
            /* IPv4 addressed to 192.168.77.1: keep it inside the ESP32 */
            if (memcmp(p + 30, WEB_IP, 4) == 0) {
                to_web = true;
                to_wifi = false;
            }
        }
    }

    if (to_web && s_web_netif) {
        /* lwip keeps the buffer until it is done with it, so it gets its own
         * copy. web_free_rx() frees it. */
        uint8_t *copy = malloc(len);
        if (copy) {
            memcpy(copy, buffer, len);
            esp_netif_receive(s_web_netif, copy, len, copy);
        }
    }
    if (to_wifi && *is_wifi_connected) {
        esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
    }
    return ESP_OK;
}

/* Called by TinyUSB when a buffer was sent.
 * Buffers from the web server have bit 0 set (they are malloc'd copies),
 * buffers from WiFi are WiFi receive buffers. */
static void usb_tx_free(void *eb, void *ctx)
{
    if ((uintptr_t)eb & 1) {
        free((void *)((uintptr_t)eb & ~(uintptr_t)1));
    } else {
        esp_wifi_internal_free_rx_buffer(eb);
    }
}

/* Frames arriving from WiFi, going to the phone. */
static esp_err_t pkt_wifi2usb(void *buffer, uint16_t len, void *eb)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t r = tinyusb_net_send_sync(buffer, len, eb, portMAX_DELAY);
    xSemaphoreGive(s_tx_lock);
    if (r != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    return ESP_OK;
}

/* Make the phone see "cable unplugged / plugged" so it asks for an IP again. */
static void usb_link_bounce(void)
{
    usb_device_set_link_state(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_device_set_link_state(true);
}

/* ------------------------------------------------------------------ */
/* Network interface for the web page (inside the ESP32)               */
/* ------------------------------------------------------------------ */

typedef struct {
    esp_netif_driver_base_t base;
} web_driver_t;

static web_driver_t s_web_driver;

/* lwip wants to send a frame to the phone */
static esp_err_t web_transmit(void *h, void *buffer, size_t len)
{
    if (len == 0 || len > 1600) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);

    esp_err_t r = ESP_FAIL;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        r = tinyusb_net_send_sync(copy, len, (void *)((uintptr_t)copy | 1), pdMS_TO_TICKS(200));
        xSemaphoreGive(s_tx_lock);
    }
    if (r != ESP_OK) {
        free(copy);
    }
    return ESP_OK;
}

/* lwip is done with a received frame: free our copy */
static void web_free_rx(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t web_post_attach(esp_netif_t *netif, esp_netif_iodriver_handle h)
{
    web_driver_t *d = (web_driver_t *)h;
    d->base.netif = netif;
    esp_netif_driver_ifconfig_t cfg = {
        .handle = d,
        .transmit = web_transmit,
        .driver_free_rx_buffer = web_free_rx,
    };
    return esp_netif_set_driver_config(netif, &cfg);
}

static esp_err_t start_web_netif(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to init esp_netif");

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_set_ip4_addr(&ip_info.ip, WEB_IP[0], WEB_IP[1], WEB_IP[2], WEB_IP[3]);
    esp_netif_set_ip4_addr(&ip_info.gw, WEB_IP[0], WEB_IP[1], WEB_IP[2], WEB_IP[3]);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "USB_WEB",
        .if_desc = "usb_web",
        .route_prio = 5,
    };
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_web_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_web_netif, ESP_FAIL, TAG, "Failed to create web netif");

    s_web_driver.base.post_attach = web_post_attach;
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_web_netif, &s_web_driver), TAG, "Failed to attach web netif");

    uint8_t mac[6] = {0x02, 0x77, 0x77, 0x00, 0x00, 0x01};
    esp_netif_set_mac(s_web_netif, mac);

    esp_netif_action_start(s_web_netif, 0, 0, NULL);
    esp_netif_action_connected(s_web_netif, 0, 0, NULL);
    esp_netif_set_ip_info(s_web_netif, &ip_info);

    esp_netif_ip_info_t now = {0};
    esp_netif_get_ip_info(s_web_netif, &now);
    ESP_LOGI(TAG, "Web UI netif IP: " IPSTR, IP2STR(&now.ip));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WiFi settings (saved in NVS)                                        */
/* ------------------------------------------------------------------ */

static void load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    /* defaults from menuconfig */
    strlcpy(ssid, CONFIG_ESP_WIFI_SSID, ssid_len);
    strlcpy(pass, CONFIG_ESP_WIFI_PASSWORD, pass_len);

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_s[33] = "";
        char tmp_p[65] = "";
        size_t ns = sizeof(tmp_s);
        size_t np = sizeof(tmp_p);
        if (nvs_get_str(nvs, "ssid", tmp_s, &ns) == ESP_OK && tmp_s[0]) {
            strlcpy(ssid, tmp_s, ssid_len);
            if (nvs_get_str(nvs, "pass", tmp_p, &np) != ESP_OK) {
                tmp_p[0] = 0;
            }
            strlcpy(pass, tmp_p, pass_len);
        }
        nvs_close(nvs);
    }
}

static void apply_wifi(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    s_reconnect = false;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_reconnect = true;
    esp_wifi_connect();
}

/* ------------------------------------------------------------------ */
/* Web page                                                            */
/* ------------------------------------------------------------------ */

static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32 WiFi</title>"
"<style>body{font-family:sans-serif;max-width:480px;margin:auto;padding:16px}"
"button,input{font-size:16px;padding:10px;margin:4px 0;width:100%;box-sizing:border-box}"
".net{padding:10px;border:1px solid #ccc;border-radius:6px;margin:4px 0}"
"#st{padding:8px;background:#eee;border-radius:6px;margin-bottom:8px}</style></head><body>"
"<h2>ESP32 WiFi setup</h2><div id='st'>checking...</div>"
"<button onclick='scan()'>Scan networks</button><div id='list'></div>"
"<input id='ssid' placeholder='WiFi name'>"
"<input id='pass' type='password' placeholder='Password (empty if open)'>"
"<button onclick='save()'>Save and connect</button><div id='msg'></div>"
"<script>"
"async function st(){try{let r=await fetch('/status');let j=await r.json();"
"document.getElementById('st').textContent=j.connected?('Connected to '+j.ssid+' ('+j.rssi+' dBm)'):('Not connected. Saved network: '+j.ssid);}catch(e){}}"
"async function scan(){let l=document.getElementById('list');l.textContent='Scanning... internet pauses for a few seconds';"
"try{let r=await fetch('/scan');let a=await r.json();l.textContent='';"
"a.forEach(function(n){let d=document.createElement('div');d.className='net';"
"d.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.open?', open':'')+')';"
"d.onclick=function(){document.getElementById('ssid').value=n.ssid;};l.appendChild(d);});"
"if(!a.length){l.textContent='No networks found';}}catch(e){l.textContent='Scan failed, try again';}}"
"async function save(){let s=document.getElementById('ssid').value;let p=document.getElementById('pass').value;"
"if(!s){alert('Choose or type a WiFi name');return;}"
"let m=document.getElementById('msg');m.textContent='Saving...';"
"try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});m.textContent=await r.text();}"
"catch(e){m.textContent='Sent. The connection may restart, wait a few seconds.';}setTimeout(st,6000);}"
"st();setInterval(st,6000);"
"</script></body></html>";

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 3 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            out[o++] = '?';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

#define MAX_SCAN 20

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t *recs = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));
    char *json = malloc(MAX_SCAN * 160 + 8);
    if (!recs || !json) {
        free(recs);
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    /* stop the automatic reconnect, scan, then connect again */
    s_reconnect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));

    uint16_t num = 0;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        num = MAX_SCAN;
        if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
            num = 0;
        }
    }

    s_reconnect = true;
    esp_wifi_connect();

    size_t pos = 0;
    json[pos++] = '[';
    int first = 1;
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        char esc[80];
        json_escape((const char *)recs[i].ssid, esc, sizeof(esc));
        pos += snprintf(json + pos, 160, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                        first ? "" : ",", esc, recs[i].rssi,
                        recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        first = 0;
    }
    json[pos++] = ']';
    json[pos] = 0;

    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(recs);
    free(json);
    return r;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap;
    wifi_config_t cfg = {0};
    char name[33] = "";
    char esc[80];
    char out[200];

    bool connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    if (connected) {
        memcpy(name, ap.ssid, 32);
    } else {
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        memcpy(name, cfg.sta.ssid, 32);
    }
    name[32] = 0;
    json_escape(name, esc, sizeof(esc));

    snprintf(out, sizeof(out), "{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d}",
             connected ? "true" : "false", esc, connected ? ap.rssi : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 400) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char body[404];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[128] = "";
    char pass[200] = "";
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    url_decode(ssid);
    url_decode(pass);

    if (strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(pass) > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Name must be 1-32 characters, password at most 64");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "New WiFi saved: %s", ssid);

    httpd_resp_sendstr(req, "Saved. Connecting to the new network, internet may pause for a few seconds.");
    apply_wifi(ssid, pass);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the web server");
        return;
    }

    httpd_uri_t u_index = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};
    httpd_uri_t u_scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL};
    httpd_uri_t u_status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL};
    httpd_uri_t u_save = {.uri = "/save", .method = HTTP_POST, .handler = save_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &u_index);
    httpd_register_uri_handler(server, &u_scan);
    httpd_register_uri_handler(server, &u_status);
    httpd_register_uri_handler(server, &u_save);

    ESP_LOGI(TAG, "Web server started: http://192.168.77.1");
}

/* ------------------------------------------------------------------ */
/* WiFi                                                                */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    bool *is_connected = arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi STA disconnected");
        *is_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        if (s_reconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi STA connected");
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, pkt_wifi2usb);
        *is_connected = true;
        /* let the phone ask for an IP address again */
        usb_link_bounce();
    }
}

static esp_err_t start_wifi(bool *is_connected)
{
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "Failed to initialize WiFi library");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, is_connected),
                        TAG, "Failed to register handler for wifi events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set WiFi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi library");

    char ssid[33];
    char pass[65];
    load_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Failed to set WiFi config");
    return esp_wifi_connect();
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* NVS stores PHY calibration data and our WiFi name/password */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_tx_lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(s_tx_lock, ESP_FAIL, err, TAG, "Failed to create mutex");
    ESP_GOTO_ON_ERROR(esp_event_loop_create_default(), err, TAG, "Cannot initialize event loop");

    ESP_LOGI(TAG, "USB NCM device initialization");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_GOTO_ON_ERROR(tinyusb_driver_install(&tusb_cfg), err, TAG, "Failed to install TinyUSB driver");

    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = usb_tx_free,
        .user_context = &s_is_wifi_connected
    };
    esp_read_mac(net_config.mac_addr, ESP_MAC_WIFI_STA);
    uint8_t *mac = net_config.mac_addr;
    ESP_LOGI(TAG, "Network interface HW address: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_GOTO_ON_ERROR(tinyusb_net_init(&net_config), err, TAG, "Failed to initialize TinyUSB NCM device class");

    /* Link stays up so the web page works even when WiFi is not connected. */
    usb_device_set_link_state(true);

    ESP_LOGI(TAG, "Web page network initialization");
    ESP_GOTO_ON_ERROR(start_web_netif(), err, TAG, "Failed to start the web network interface");
    start_webserver();

    ESP_LOGI(TAG, "WiFi initialization");
    ESP_GOTO_ON_ERROR(start_wifi(&s_is_wifi_connected), err, TAG, "Failed to init and start WiFi");

    ESP_LOGI(TAG, "USB NCM and WiFi initialized and started");
    return;

err:
    ESP_LOGE(TAG, "USB-WiFi bridge failed!");
}
