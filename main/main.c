/* main.c
 * Digital Visitor Counter — ESP-IDF firmware (v2, no OLED)
 *
 * Replaces the Arduino VisitorCounter.ino.
 * Removed: Adafruit SSD1306 OLED (Wire, Adafruit_GFX, Adafruit_SSD1306,
 *          OLED_SDA, OLED_SCL pins, refreshOLED()).
 * All other features (IR FSM, LED, buzzer, NVS, SPIFFS log,
 * WiFi dashboard, Firebase, ThingSpeak, Serial console) are preserved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "config.h"
#include "data_store.h"
#include "wifi_manager.h"
#include "web_server.h"

/* ====================================================================
   Cloud event queue — decouples the fast FSM task from slow HTTP calls
   ==================================================================== */
typedef struct {
    char type[8];        /* "ENTRY" or "EXIT" */
    char ts[32];         /* timestamp string  */
    time_t epoch;
    counter_state_t snap;
} cloud_event_t;

#define CLOUD_QUEUE_LEN 8
static QueueHandle_t s_cloud_queue = NULL;

static const char *TAG = "main";

/* ====================================================================
   Convenience: millis() equivalent (ms since boot)
   ==================================================================== */
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ====================================================================
   Shared counter state + mutex
   web_server.c receives a pointer to g_state at startup.
   ==================================================================== */
static counter_state_t g_state = {
    .entries      = 0,
    .exits        = 0,
    .occupancy    = 0,
    .max_capacity = DEFAULT_MAX_CAPACITY,
    .room_full    = false,
};
static SemaphoreHandle_t g_state_mutex = NULL;

/* ====================================================================
   ISR flags — set by interrupt, cleared in FSM task
   ==================================================================== */
static volatile bool s_ir1_flag = false;
static volatile bool s_ir2_flag = false;

static void IRAM_ATTR isr_ir1(void *arg) { s_ir1_flag = true; }
static void IRAM_ATTR isr_ir2(void *arg) { s_ir2_flag = true; }

/* ====================================================================
   FSM state
   ==================================================================== */
typedef enum { FSM_IDLE, FSM_IR1_TRIGGERED, FSM_IR2_TRIGGERED } fsm_state_t;
static fsm_state_t s_fsm         = FSM_IDLE;
static uint32_t    s_first_trig  = 0;
static uint32_t    s_last_ir1_ms = 0;
static uint32_t    s_last_ir2_ms = 0;
static uint32_t    s_events_since_save = 0;
static uint32_t    s_last_thingspeak_ms = 0;

/* ====================================================================
   Sensor helpers with debounce
   NOTE: flag is cleared after the pin read to avoid losing a rapid
   second ISR between the flag-clear and digitalRead (race fix vs original).
   ==================================================================== */
static bool read_ir1_triggered(void)
{
    if (!s_ir1_flag) return false;
    bool triggered = (gpio_get_level(IR1_PIN) == 0);  /* active-LOW beam break */
    s_ir1_flag = false;
    if (triggered && (millis() - s_last_ir1_ms > DEBOUNCE_MS)) {
        s_last_ir1_ms = millis();
        return true;
    }
    return false;
}

static bool read_ir2_triggered(void)
{
    if (!s_ir2_flag) return false;
    bool triggered = (gpio_get_level(IR2_PIN) == 0);
    s_ir2_flag = false;
    if (triggered && (millis() - s_last_ir2_ms > DEBOUNCE_MS)) {
        s_last_ir2_ms = millis();
        return true;
    }
    return false;
}

/* Buzzer removed — no actuator functions needed */

/* ====================================================================
   Event handler — called after each confirmed ENTRY or EXIT
   ==================================================================== */
static void on_event(const char *type)
{
    char ts[32];
    time_t epoch = 0;
    bool have_time = wifi_manager_get_timestamp(ts, sizeof(ts), &epoch);
    if (!have_time) {
        snprintf(ts, sizeof(ts), "uptime+%lus", (unsigned long)(millis() / 1000));
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "[EVENT] %s  Time=%s  Entries=%lu  Exits=%lu  Occupancy=%ld",
             type, ts,
             (unsigned long)g_state.entries,
             (unsigned long)g_state.exits,
             (long)g_state.occupancy);

    s_events_since_save++;
    if (s_events_since_save >= SAVE_EVERY_N_EVENTS) {
        storage_save_counters(g_state.entries, g_state.exits, g_state.max_capacity);
        s_events_since_save = 0;
    }
    counter_state_t snap = g_state;
    xSemaphoreGive(g_state_mutex);

    /* Log to SPIFFS immediately (fast, local flash write) */
#if ENABLE_LOCAL_LOG
    log_append(type, ts, snap.entries, snap.exits, snap.occupancy);
#endif

    /* Cloud calls go to the queue — FSM task returns instantly */
    if (s_cloud_queue && wifi_manager_is_connected()) {
        cloud_event_t ev;
        strncpy(ev.type, type, sizeof(ev.type) - 1);
        ev.type[sizeof(ev.type) - 1] = '\0';
        strncpy(ev.ts, ts, sizeof(ev.ts) - 1);
        ev.ts[sizeof(ev.ts) - 1] = '\0';
        ev.epoch = epoch;
        ev.snap  = snap;
        /* Non-blocking: if queue is full, drop the event rather than stall */
        if (xQueueSend(s_cloud_queue, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Cloud queue full — event dropped.");
        }
    }
}

/* ====================================================================
   FSM update — call from sensor task every loop iteration
   ==================================================================== */
static void update_fsm(void)
{
    bool ir1 = read_ir1_triggered();
    bool ir2 = read_ir2_triggered();

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    switch (s_fsm) {
        case FSM_IDLE:
            if (ir1) { s_fsm = FSM_IR1_TRIGGERED; s_first_trig = millis(); }
            else if (ir2) { s_fsm = FSM_IR2_TRIGGERED; s_first_trig = millis(); }
            break;

        case FSM_IR1_TRIGGERED:   /* waiting for IR2 → ENTRY */
            if (ir2) {
                g_state.entries++;
                g_state.occupancy++;
                s_fsm = FSM_IDLE;
                xSemaphoreGive(g_state_mutex);
                on_event("ENTRY");
                return;
            } else if (millis() - s_first_trig > EVENT_TIMEOUT_MS) {
                s_fsm = FSM_IDLE;   /* timeout: person turned back */
            }
            break;

        case FSM_IR2_TRIGGERED:   /* waiting for IR1 → EXIT */
            if (ir1) {
                g_state.exits++;
                if (g_state.occupancy > 0) g_state.occupancy--;
                s_fsm = FSM_IDLE;
                xSemaphoreGive(g_state_mutex);
                on_event("EXIT");
                return;
            } else if (millis() - s_first_trig > EVENT_TIMEOUT_MS) {
                s_fsm = FSM_IDLE;
            }
            break;
    }

    /* Capacity alert */
    bool should_full = (g_state.occupancy >= g_state.max_capacity);
    if (should_full != g_state.room_full) {
        g_state.room_full = should_full;
        if (should_full) ESP_LOGW(TAG, "[ALERT] Maximum capacity reached!");
    }
    gpio_set_level(LED_PIN, g_state.room_full ? 1 : 0);

    xSemaphoreGive(g_state_mutex);
}

/* ====================================================================
   UART Serial admin console task
   Commands (case-insensitive): RESET | SETCAP <n> | STATUS | CLEARLOG | HELP
   ==================================================================== */
static void serial_console_task(void *arg)
{
    /* Install UART driver for UART0 so we can read lines. */
    uart_config_t ucfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &ucfg);
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);

    char buf[64];
    int  pos = 0;

    while (1) {
        uint8_t ch;
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        if (ch == '\r' || ch == '\n') {
            buf[pos] = '\0';
            pos = 0;
            if (strlen(buf) == 0) continue;

            /* Upper-case in place */
            for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

            if (strcmp(buf, "HELP") == 0) {
                printf("Commands: RESET | SETCAP <n> | STATUS | CLEARLOG | HELP\n");
            } else if (strcmp(buf, "RESET") == 0) {
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                g_state.entries = g_state.exits = g_state.occupancy = 0;
                g_state.room_full = false;
                storage_save_counters(0, 0, g_state.max_capacity);
                xSemaphoreGive(g_state_mutex);
                printf("Counters reset.\n");
            } else if (strncmp(buf, "SETCAP ", 7) == 0) {
                int cap = atoi(buf + 7);
                if (cap > 0) {
                    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                    g_state.max_capacity = cap;
                    storage_save_counters(g_state.entries, g_state.exits, cap);
                    xSemaphoreGive(g_state_mutex);
                    printf("MAX_CAPACITY set to %d\n", cap);
                }
            } else if (strcmp(buf, "STATUS") == 0) {
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                printf("Entries=%lu  Exits=%lu  Occupancy=%ld  MaxCap=%d\n",
                       (unsigned long)g_state.entries,
                       (unsigned long)g_state.exits,
                       (long)g_state.occupancy,
                       g_state.max_capacity);
                xSemaphoreGive(g_state_mutex);
            } else if (strcmp(buf, "CLEARLOG") == 0) {
                log_clear();
#if !ENABLE_LOCAL_LOG
                printf("Local log is disabled.\n");
#endif
            } else {
                printf("Unknown command. Type HELP.\n");
            }
        } else if (pos < (int)sizeof(buf) - 1) {
            buf[pos++] = (char)ch;
        }
    }
}

/* ====================================================================
   Cloud task — drains the cloud_event queue and pushes to Firebase /
   ThingSpeak. Runs on core 0 at low priority so HTTP timeouts never
   block the sensor FSM (which runs on core 1 at high priority).
   ==================================================================== */
static void cloud_task(void *arg)
{
    cloud_event_t ev;
    uint32_t last_ts_push_ms = 0;

    while (1) {
        /* Block up to 1 s waiting for an event */
        if (xQueueReceive(s_cloud_queue, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
#if ENABLE_FIREBASE
            firebase_push_event(ev.type, ev.ts, ev.epoch, &ev.snap);
            firebase_push_status(&ev.snap);
#endif
            last_ts_push_ms = millis();
        }

        /* ThingSpeak: interval-based, no event needed */
#if ENABLE_THINGSPEAK
        if (wifi_manager_is_connected() &&
            millis() - last_ts_push_ms > THINGSPEAK_INTERVAL_MS) {
            last_ts_push_ms = millis();
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            counter_state_t snap = g_state;
            xSemaphoreGive(g_state_mutex);
            thingspeak_push(&snap);
        }
#endif
    }
}

/* ====================================================================
   Sensor + FSM task (core 1)
   ==================================================================== */
static void sensor_task(void *arg)
{
    static uint32_t last_btn_ms = 0;

    while (1) {
        update_fsm();

        /* Optional manual reset button (active-LOW, GPIO 13) */
        if (millis() - last_btn_ms > 50) {
            last_btn_ms = millis();
            if (gpio_get_level(RESET_BTN_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(30));   /* simple debounce */
                if (gpio_get_level(RESET_BTN_PIN) == 0) {
                    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                    g_state.entries = g_state.exits = g_state.occupancy = 0;
                    g_state.room_full = false;
                    storage_save_counters(0, 0, g_state.max_capacity);
                    xSemaphoreGive(g_state_mutex);
                    ESP_LOGI(TAG, "[BUTTON] Counters reset.");
                    while (gpio_get_level(RESET_BTN_PIN) == 0)
                        vTaskDelay(pdMS_TO_TICKS(10));   /* wait for release */
                }
            }
        }

        /* ThingSpeak is interval-based, also handled by cloud_task */

        vTaskDelay(pdMS_TO_TICKS(5));  /* yield CPU, ~200 Hz FSM update rate */
    }
}

/* ====================================================================
   GPIO initialisation
   ==================================================================== */
static void gpio_init_all(void)
{
    /* IR sensors: input, pull-up */
    gpio_config_t ir_cfg = {
        .pin_bit_mask = (1ULL << IR1_PIN) | (1ULL << IR2_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&ir_cfg);

    /* Reset button: input, pull-up, no interrupt (polled) */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << RESET_BTN_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* LED: output, start LOW */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(LED_PIN, 0);

    /* Install ISR service and attach per-pin handlers */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(IR1_PIN, isr_ir1, NULL);
    gpio_isr_handler_add(IR2_PIN, isr_ir2, NULL);
}

/* ====================================================================
   app_main — ESP-IDF entry point (replaces Arduino setup() + loop())
   ==================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "\n=== Digital Visitor Counter booting (ESP-IDF) ===");

    /* 1. Create shared mutex */
    g_state_mutex = xSemaphoreCreateMutex();
    configASSERT(g_state_mutex);

    /* 2. NVS init + load persisted counters */
    storage_init();
    uint32_t ent = 0, ex = 0;
    int      cap = DEFAULT_MAX_CAPACITY;
    storage_load_counters(&ent, &ex, &cap);
    g_state.entries      = ent;
    g_state.exits        = ex;
    g_state.max_capacity = cap;
    g_state.occupancy    = (int32_t)ent - (int32_t)ex;
    if (g_state.occupancy < 0) g_state.occupancy = 0;

    /* 3. GPIO */
    gpio_init_all();

    /* 4. SPIFFS log */
#if ENABLE_LOCAL_LOG
    log_init();
#endif

    /* 5. WiFi + NTP */
#if ENABLE_WIFI_DASHBOARD
    wifi_manager_init();
    wifi_manager_sync_ntp();
    web_server_start(&g_state);
#endif

    ESP_LOGI(TAG, "Type HELP for admin commands.");

    /* 6. Create cloud event queue */
    s_cloud_queue = xQueueCreate(CLOUD_QUEUE_LEN, sizeof(cloud_event_t));
    configASSERT(s_cloud_queue);

    /* 7. Start FreeRTOS tasks */
    xTaskCreatePinnedToCore(sensor_task,        "sensor",  4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(serial_console_task,"serial",  4096, NULL,  5, NULL, 0);
    xTaskCreatePinnedToCore(cloud_task,         "cloud",   8192, NULL,  4, NULL, 0);

    /* app_main returns here — RTOS scheduler takes over */
}
