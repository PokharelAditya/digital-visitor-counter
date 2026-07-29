#pragma once
#include "config.h"   /* pulls in LOG_FILE_PATH, LOG_MAX_BYTES, ENABLE_LOCAL_LOG, etc. */
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stddef.h>

/* ---- NVS: counters ---- */
void storage_init(void);
void storage_load_counters(uint32_t *entries, uint32_t *exits, int *max_capacity);
void storage_save_counters(uint32_t entries, uint32_t exits, int max_capacity);

/* ---- SPIFFS: event log ---- */
void log_init(void);
void log_append(const char *type, const char *timestamp,
                uint32_t entries, uint32_t exits, int32_t occupancy);
void log_clear(void);
/* Writes a JSON array of the most recent max_rows events into out_buf.
   Returns the number of bytes written (excluding NUL terminator). */
int  log_read_recent_json(char *out_buf, size_t buf_size, int max_rows);
