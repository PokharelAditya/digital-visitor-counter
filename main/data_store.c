/* data_store.c
 * NVS (counters) + SPIFFS (CSV event log) for the Digital Visitor Counter.
 */

#include "data_store.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"

static const char *TAG = "data_store";
#define NVS_NAMESPACE "visitor"

/* ====================================================================
   NVS — counters (entries, exits, max_capacity)
   ==================================================================== */

void storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and re-initialised.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void storage_load_counters(uint32_t *entries, uint32_t *exits, int *max_capacity)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* First boot — defaults already set by caller */
        return;
    }
    nvs_get_u32(h, "entries", entries);
    nvs_get_u32(h, "exits",   exits);
    int32_t cap = (int32_t)(*max_capacity);
    nvs_get_i32(h, "maxcap",  &cap);
    *max_capacity = (int)cap;
    nvs_close(h);

    ESP_LOGI(TAG, "Loaded from NVS -> entries=%lu  exits=%lu  maxcap=%d",
             (unsigned long)*entries, (unsigned long)*exits, *max_capacity);
}

void storage_save_counters(uint32_t entries, uint32_t exits, int max_capacity)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed — counters NOT saved.");
        return;
    }
    nvs_set_u32(h, "entries", entries);
    nvs_set_u32(h, "exits",   exits);
    nvs_set_i32(h, "maxcap",  (int32_t)max_capacity);
    nvs_commit(h);
    nvs_close(h);
}

/* ====================================================================
   SPIFFS — CSV event log
   Format: timestamp,type,entries,exits,occupancy\n
   ==================================================================== */

#if ENABLE_LOCAL_LOG

static bool s_spiffs_ok = false;

void log_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,   /* auto-detect spiffs partition */
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed (%s) — local logging disabled.", esp_err_to_name(err));
        return;
    }
    s_spiffs_ok = true;

    /* Create header row if the file doesn't exist yet. */
    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (!f) {
        f = fopen(LOG_FILE_PATH, "w");
        if (f) {
            fprintf(f, "timestamp,type,entries,exits,occupancy\n");
            fclose(f);
        }
    } else {
        /* Log already exists — report its size. */
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        ESP_LOGI(TAG, "Log file exists, size = %ld bytes", sz);
    }
}

/* Trim helper: if file exceeds LOG_MAX_BYTES, keep only the newer half. */
static void trim_log_if_needed(void)
{
    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= (long)LOG_MAX_BYTES) { fclose(f); return; }

    /* Read entire file into a heap buffer. */
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t read = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read] = '\0';

    /* Find the newline at the midpoint. */
    char *mid = buf + read / 2;
    char *cut = strchr(mid, '\n');
    if (!cut || (cut + 1) >= (buf + read)) { free(buf); return; }

    /* Re-write with header + second half. */
    FILE *out = fopen(LOG_FILE_PATH, "w");
    if (out) {
        fprintf(out, "timestamp,type,entries,exits,occupancy\n");
        fwrite(cut + 1, 1, (size_t)((buf + read) - (cut + 1)), out);
        fclose(out);
        ESP_LOGI(TAG, "Log trimmed (was %ld bytes).", sz);
    }
    free(buf);
}

void log_append(const char *type, const char *timestamp,
                uint32_t entries, uint32_t exits, int32_t occupancy)
{
    if (!s_spiffs_ok) return;
    trim_log_if_needed();

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (!f) { ESP_LOGE(TAG, "Failed to open log for append."); return; }
    fprintf(f, "%s,%s,%lu,%lu,%ld\n",
            timestamp, type,
            (unsigned long)entries, (unsigned long)exits, (long)occupancy);
    fclose(f);
}

void log_clear(void)
{
    if (!s_spiffs_ok) return;
    FILE *f = fopen(LOG_FILE_PATH, "w");
    if (f) {
        fprintf(f, "timestamp,type,entries,exits,occupancy\n");
        fclose(f);
    }
    ESP_LOGI(TAG, "Local log cleared.");
}

/* Build a JSON array of the most recent max_rows entries.
   Returns bytes written (not counting NUL). */
int log_read_recent_json(char *out_buf, size_t buf_size, int max_rows)
{
    if (!s_spiffs_ok || !out_buf || buf_size < 3) {
        if (out_buf) { out_buf[0] = '['; out_buf[1] = ']'; out_buf[2] = '\0'; }
        return 2;
    }

    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (!f) {
        snprintf(out_buf, buf_size, "[]");
        return 2;
    }

    /* Ring-buffer of line pointers into a heap block. */
    const int CAP = 256;
    char **lines = calloc(CAP, sizeof(char *));
    if (!lines) { fclose(f); snprintf(out_buf, buf_size, "[]"); return 2; }

    char line_buf[160];
    /* Skip header */
    fgets(line_buf, sizeof(line_buf), f);

    int count = 0;
    while (fgets(line_buf, sizeof(line_buf), f)) {
        /* strip trailing newline */
        size_t l = strlen(line_buf);
        while (l > 0 && (line_buf[l-1] == '\n' || line_buf[l-1] == '\r')) line_buf[--l] = '\0';
        if (l == 0) continue;
        int idx = count % CAP;
        free(lines[idx]);
        lines[idx] = strdup(line_buf);
        count++;
    }
    fclose(f);

    int total  = (count < CAP) ? count : CAP;
    int start  = (count > max_rows) ? count - max_rows : 0;
    if (start < count - total) start = count - total;

    size_t pos = 0;
    out_buf[pos++] = '[';
    bool first = true;

    for (int i = start; i < count && pos < buf_size - 2; i++) {
        char *ln = lines[i % CAP];
        if (!ln) continue;

        /* Parse: timestamp,type,entries,exits,occupancy */
        char *p1 = strchr(ln, ',');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, ',');
        if (!p2) continue;
        char *p3 = strchr(p2 + 1, ',');
        if (!p3) continue;
        char *p4 = strchr(p3 + 1, ',');
        if (!p4) continue;

        *p1 = *p2 = *p3 = *p4 = '\0';
        const char *ts  = ln;
        const char *typ = p1 + 1;
        const char *ent = p2 + 1;
        const char *ex  = p3 + 1;
        const char *occ = p4 + 1;

        if (!first) out_buf[pos++] = ',';
        first = false;

        int written = snprintf(out_buf + pos, buf_size - pos,
            "{\"timestamp\":\"%s\",\"type\":\"%s\","
            "\"entries\":%s,\"exits\":%s,\"occupancy\":%s}",
            ts, typ, ent, ex, occ);
        if (written > 0) pos += (size_t)written;
    }

    if (pos < buf_size - 1) out_buf[pos++] = ']';
    out_buf[pos] = '\0';

    for (int i = 0; i < CAP; i++) free(lines[i]);
    free(lines);
    return (int)pos;
}

#else  /* ENABLE_LOCAL_LOG == 0 — provide empty stubs */

void log_init(void)  {}
void log_append(const char *t, const char *ts, uint32_t e, uint32_t x, int32_t o)
    { (void)t; (void)ts; (void)e; (void)x; (void)o; }
void log_clear(void) {}
int  log_read_recent_json(char *b, size_t s, int r)
    { if (b && s >= 3) { b[0]='['; b[1]=']'; b[2]='\0'; } return 2; (void)r; }

#endif /* ENABLE_LOCAL_LOG */
