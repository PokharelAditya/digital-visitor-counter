/* wifi_manager.c
 * Wi-Fi station connection + SNTP time sync for the Digital Visitor Counter.
 */

#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool               s_time_synced      = false;

/* ---- Event handler ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected — retrying...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected! Dashboard: http://" IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ---- Public API ---- */

void wifi_manager_init(void)
{
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGI(TAG, "No SSID configured — running offline.");
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to \"%s\"...", WIFI_SSID);

    /* Wait up to 15 s for connection. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi connection timed-out — continuing offline.");
    }
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_sync_ntp(void)
{
    if (!wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "No WiFi — NTP skipped; timestamps will use uptime.");
        return;
    }

    /* Set timezone (POSIX string). Nepal = UTC+5:45, no DST. */
    setenv("TZ", "NPT-5:45", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();

    ESP_LOGI(TAG, "Waiting for NTP sync...");
    for (int i = 0; i < 20; i++) {
        time_t now = 0;
        time(&now);
        if (now > 1700000000L) {   /* sanity: past Nov 2023 */
            s_time_synced = true;
            struct tm ti;
            localtime_r(&now, &ti);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
            ESP_LOGI(TAG, "Time synced: %s", buf);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGW(TAG, "NTP sync failed — timestamps will use uptime.");
}

bool wifi_manager_get_timestamp(char *out_buf, size_t buf_len, time_t *epoch_out)
{
    time_t now = 0;
    time(&now);
    if (!s_time_synced || now < 1700000000L) return false;

    struct tm ti;
    localtime_r(&now, &ti);
    strftime(out_buf, buf_len, "%Y-%m-%d %H:%M:%S", &ti);
    if (epoch_out) *epoch_out = now;
    return true;
}
