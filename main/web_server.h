#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Shared counter state — written by main.c FSM task,
   read by web-server handlers.  Protected by g_state_mutex (in main.c). */
typedef struct {
    uint32_t entries;
    uint32_t exits;
    int32_t  occupancy;
    int      max_capacity;
    bool     room_full;
} counter_state_t;

/* Starts the ESP-IDF HTTP server.  Pass a pointer to the global state. */
void web_server_start(counter_state_t *state);

/* Firebase REST calls (HTTPS POST / PUT). */
void firebase_push_event(const char *type, const char *timestamp,
                         time_t epoch, const counter_state_t *state);
void firebase_push_status(const counter_state_t *state);

/* ThingSpeak upload (plain HTTP GET). */
void thingspeak_push(const counter_state_t *state);
