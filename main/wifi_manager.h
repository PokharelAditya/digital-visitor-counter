#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Connects to Wi-Fi (non-blocking after this call; use wifi_manager_is_connected()).
   Does nothing if WIFI_SSID is blank. */
void wifi_manager_init(void);

/* Returns true when the station has an IP address. */
bool wifi_manager_is_connected(void);

/* Blocks until NTP syncs (up to ~6 s). Call after wifi_manager_init(). */
void wifi_manager_sync_ntp(void);

/* Fills out_buf with "YYYY-MM-DD HH:MM:SS" and sets *epoch_out.
   Returns false if real time is not yet available (falls back to uptime). */
bool wifi_manager_get_timestamp(char *out_buf, size_t buf_len, time_t *epoch_out);
