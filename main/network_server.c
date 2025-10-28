/*
 * Network server: connect to Wi-Fi and expose HID actions via HTTP POST endpoints.
 */

#include "network_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "lwip/ip4_addr.h"

#include "hid_actions.h"

#define WIFI_SSID "navy"
#define WIFI_PASS "Whj5201314"
#define WIFI_GATEWAY "192.168.0.1"
#define STATIC_IP_ADDR "192.168.0.201"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "NET_SERVER";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static bool s_static_ip_enabled = false;
static uint16_t s_hid_conn_id = UINT16_MAX;
static httpd_handle_t s_httpd = NULL;

static esp_err_t start_http_server(void);
static esp_err_t stop_http_server(void);

void network_server_set_hid_conn_id(uint16_t conn_id)
{
    s_hid_conn_id = conn_id;
}

static void log_current_ip(void)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK)
    {
        ESP_LOGI(TAG, "Station IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Disconnected from AP, retrying...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP acquired: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t configure_static_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = ipaddr_addr(STATIC_IP_ADDR);
    ip_info.gw.addr = ipaddr_addr(WIFI_GATEWAY);
    ip_info.netmask.addr = ipaddr_addr("255.255.255.0");

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGW(TAG, "Failed to stop DHCP client: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err == ESP_OK)
    {
        s_static_ip_enabled = true;
        ESP_LOGI(TAG, "Static IP configured: %s", STATIC_IP_ADDR);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to set static IP (%s), falling back to DHCP", esp_err_to_name(err));
        esp_netif_dhcpc_start(netif);
    }

    return err;
}

static esp_err_t init_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif)
    {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    configure_static_ip(s_sta_netif);

    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char **out_buf, size_t *out_len)
{
    size_t total_len = req->content_len;
    if (total_len == 0)
    {
        *out_buf = NULL;
        *out_len = 0;
        return ESP_OK;
    }

    char *buf = (char *)malloc(total_len + 1);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < total_len)
    {
        int r = httpd_req_recv(req, buf + received, total_len - received);
        if (r <= 0)
        {
            free(buf);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[total_len] = '\0';

    *out_buf = buf;
    *out_len = total_len;
    return ESP_OK;
}

static esp_err_t respond_json_ok(httpd_req_t *req)
{
    const char *resp = "{\"status\":\"ok\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t respond_error(httpd_req_t *req, int status, const char *message)
{
    char status_str[40];
    snprintf(status_str, sizeof(status_str), "%d %s", status, (status >= 500) ? "Server Error" : "Bad Request");
    httpd_resp_set_status(req, status_str);
    httpd_resp_set_type(req, "text/plain");
    const char *body = message ? message : "error";
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool parse_number_field(const char *json, const char *field, double *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *pos = strstr(json, pattern);
    if (!pos)
    {
        return false;
    }
    pos += strlen(pattern);
    pos = strchr(pos, ':');
    if (!pos)
    {
        return false;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos))
    {
        pos++;
    }
    char *endptr;
    double value = strtod(pos, &endptr);
    if (endptr == pos)
    {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_float_field(const char *json, const char *field, float *out)
{
    double v;
    if (!parse_number_field(json, field, &v))
    {
        return false;
    }
    *out = (float)v;
    return true;
}

static bool parse_uint32_field(const char *json, const char *field, uint32_t *out)
{
    double v;
    if (!parse_number_field(json, field, &v))
    {
        return false;
    }
    if (v < 0)
    {
        v = 0;
    }
    *out = (uint32_t)(v + 0.5);
    return true;
}

static bool ensure_hid_ready(httpd_req_t *req)
{
    if (s_hid_conn_id == UINT16_MAX)
    {
        respond_error(req, 503, "HID not connected");
        return false;
    }
    return true;
}

static esp_err_t handle_touch_tap(httpd_req_t *req)
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK)
    {
        return respond_error(req, 500, "Failed to read body");
    }

    float x = 0.5f, y = 0.5f;
    if (body)
    {
        bool ok = parse_float_field(body, "x", &x) && parse_float_field(body, "y", &y);
        if (!ok)
        {
            free(body);
            return respond_error(req, 400, "Missing x/y");
        }
    }

    hid_touch_tap(s_hid_conn_id, x, y);

    free(body);
    return respond_json_ok(req);
}

static esp_err_t handle_touch_long_press(httpd_req_t *req)
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK)
    {
        return respond_error(req, 500, "Failed to read body");
    }

    float x = 0.0f, y = 0.0f;
    uint32_t duration = 0;
    if (!body)
    {
        return respond_error(req, 400, "Missing body");
    }

    bool ok = parse_float_field(body, "x", &x) && parse_float_field(body, "y", &y) && parse_uint32_field(body, "duration_ms", &duration);
    free(body);

    if (!ok)
    {
        return respond_error(req, 400, "Missing fields");
    }

    hid_touch_long_press(s_hid_conn_id, x, y, duration);
    return respond_json_ok(req);
}

static esp_err_t handle_touch_multi_tap(httpd_req_t *req)
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK)
    {
        return respond_error(req, 500, "Failed to read body");
    }

    if (!body)
    {
        return respond_error(req, 400, "Missing body");
    }

    float coords[10] = {0};
    uint32_t count = 0;
    const char *ptr = strstr(body, "points");
    if (ptr)
    {
        const char *p = strchr(ptr, '[');
        const char *end = strchr(ptr, ']');
        if (p && end && end > p)
        {
            p++;
            while (p < end && count < 5)
            {
                double vx, vy;
                if (!parse_number_field(p, "x", &vx) || !parse_number_field(p, "y", &vy))
                {
                    break;
                }
                coords[count * 2] = (float)vx;
                coords[count * 2 + 1] = (float)vy;
                count++;
                p = strchr(p, '}');
                if (!p)
                {
                    break;
                }
                p++;
            }
        }
    }

    free(body);

    if (count == 0)
    {
        return respond_error(req, 400, "Invalid points");
    }

    if (count > 5)
    {
        count = 5;
    }

    float xs[5];
    float ys[5];
    for (uint32_t i = 0; i < count; ++i)
    {
        xs[i] = coords[i * 2];
        ys[i] = coords[i * 2 + 1];
    }

    hid_touch_multi_tap(s_hid_conn_id, count, xs, ys);
    return respond_json_ok(req);
}

static esp_err_t handle_touch_multi_long_press(httpd_req_t *req)
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK)
    {
        return respond_error(req, 500, "Failed to read body");
    }

    if (!body)
    {
        return respond_error(req, 400, "Missing body");
    }

    float coords[10] = {0};
    uint32_t count = 0;
    const char *ptr = strstr(body, "points");
    if (ptr)
    {
        const char *p = strchr(ptr, '[');
        const char *end = strchr(ptr, ']');
        if (p && end && end > p)
        {
            p++;
            while (p < end && count < 5)
            {
                double vx, vy;
                if (!parse_number_field(p, "x", &vx) || !parse_number_field(p, "y", &vy))
                {
                    break;
                }
                coords[count * 2] = (float)vx;
                coords[count * 2 + 1] = (float)vy;
                count++;
                p = strchr(p, '}');
                if (!p)
                {
                    break;
                }
                p++;
            }
        }
    }

    uint32_t duration = 0;
    parse_uint32_field(body, "duration_ms", &duration);

    free(body);

    if (count == 0)
    {
        return respond_error(req, 400, "Invalid points");
    }

    if (count > 5)
    {
        count = 5;
    }

    float xs[5];
    float ys[5];
    for (uint32_t i = 0; i < count; ++i)
    {
        xs[i] = coords[i * 2];
        ys[i] = coords[i * 2 + 1];
    }

    hid_touch_multi_long_press(s_hid_conn_id, count, xs, ys, duration);
    return respond_json_ok(req);
}
static esp_err_t handle_touch_swipe(httpd_req_t *req)
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, &body, &len);
    if (err != ESP_OK)
    {
        return respond_error(req, 500, "Failed to read body");
    }

    float sx = 0, sy = 0, ex = 0, ey = 0;
    uint32_t duration = 0;
    if (!body)
    {
        return respond_error(req, 400, "Missing body");
    }

    bool ok = parse_float_field(body, "start_x", &sx) && parse_float_field(body, "start_y", &sy) && parse_float_field(body, "end_x", &ex) && parse_float_field(body, "end_y", &ey);
    if (ok)
    {
        if (!parse_uint32_field(body, "duration_ms", &duration))
        {
            duration = 0;
        }
    }
    free(body);

    if (!ok)
    {
        return respond_error(req, 400, "Missing fields");
    }

    hid_touch_swipe(s_hid_conn_id, sx, sy, ex, ey, duration);
    return respond_json_ok(req);
}

static esp_err_t handle_key_action(httpd_req_t *req, void (*action)(uint16_t conn_id))
{
    if (!ensure_hid_ready(req))
    {
        return ESP_OK;
    }
    action(s_hid_conn_id);
    return respond_json_ok(req);
}

static esp_err_t handle_volume_up(httpd_req_t *req) { return handle_key_action(req, hid_press_volume_up); }
static esp_err_t handle_volume_down(httpd_req_t *req) { return handle_key_action(req, hid_press_volume_down); }
static esp_err_t handle_home(httpd_req_t *req) { return handle_key_action(req, hid_press_home); }
static esp_err_t handle_back(httpd_req_t *req) { return handle_key_action(req, hid_press_back); }
static esp_err_t handle_power(httpd_req_t *req) { return handle_key_action(req, hid_press_power); }

static void register_http_handlers(httpd_handle_t server)
{
    const httpd_uri_t tap_uri = {
        .uri = "/touch/tap",
        .method = HTTP_POST,
        .handler = handle_touch_tap,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &tap_uri);

    const httpd_uri_t long_press_uri = {
        .uri = "/touch/long_press",
        .method = HTTP_POST,
        .handler = handle_touch_long_press,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &long_press_uri);

    const httpd_uri_t swipe_uri = {
        .uri = "/touch/swipe",
        .method = HTTP_POST,
        .handler = handle_touch_swipe,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &swipe_uri);

    const httpd_uri_t multi_tap_uri = {
        .uri = "/touch/multi_tap",
        .method = HTTP_POST,
        .handler = handle_touch_multi_tap,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &multi_tap_uri);

    const httpd_uri_t multi_press_uri = {
        .uri = "/touch/multi_long_press",
        .method = HTTP_POST,
        .handler = handle_touch_multi_long_press,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &multi_press_uri);

    const httpd_uri_t volume_up_uri = {
        .uri = "/key/volume_up",
        .method = HTTP_POST,
        .handler = handle_volume_up,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &volume_up_uri);

    const httpd_uri_t volume_down_uri = {
        .uri = "/key/volume_down",
        .method = HTTP_POST,
        .handler = handle_volume_down,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &volume_down_uri);

    const httpd_uri_t home_uri = {
        .uri = "/key/home",
        .method = HTTP_POST,
        .handler = handle_home,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &home_uri);

    const httpd_uri_t back_uri = {
        .uri = "/key/back",
        .method = HTTP_POST,
        .handler = handle_back,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &back_uri);

    const httpd_uri_t power_uri = {
        .uri = "/key/power",
        .method = HTTP_POST,
        .handler = handle_power,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &power_uri);
}

static esp_err_t start_http_server(void)
{
    if (s_httpd)
    {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port = 80;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    register_http_handlers(s_httpd);
    ESP_LOGI(TAG, "HTTP server started on port %u", config.server_port);
    return ESP_OK;
}

static esp_err_t stop_http_server(void)
{
    if (s_httpd)
    {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    return ESP_OK;
}

esp_err_t network_server_start(void)
{
    ESP_ERROR_CHECK(init_wifi());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (!(bits & WIFI_CONNECTED_BIT))
    {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        return ESP_FAIL;
    }

    if (s_static_ip_enabled)
    {
        log_current_ip();
    }

    ESP_ERROR_CHECK(start_http_server());
    return ESP_OK;
}


