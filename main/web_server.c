/* web_server.c
 * ESP-IDF HTTP server, Firebase HTTPS push, and ThingSpeak upload
 * for the Digital Visitor Counter.
 */

#include "web_server.h"
#include "data_store.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"   /* required for esp_crt_bundle_attach() */

static const char *TAG = "web_server";

/* Pointer to the shared counter state (owned by main.c). */
static counter_state_t *s_state = NULL;

/* ====================================================================
   Dashboard HTML — same look as the original, served from flash
   ==================================================================== */
static const char DASHBOARD_HTML[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Visitor Counter Dashboard</title>"
"<style>"
"body{font-family:Arial,Helvetica,sans-serif;background:#0f172a;color:#e2e8f0;"
"     display:flex;flex-direction:column;align-items:center;padding:30px;margin:0;}"
"h1{color:#38bdf8;margin-bottom:4px;}"
".sub{color:#94a3b8;margin-bottom:24px;font-size:14px;}"
".cards{display:flex;gap:16px;flex-wrap:wrap;justify-content:center;}"
".card{background:#1e293b;border-radius:14px;padding:22px 30px;min-width:140px;"
"      text-align:center;box-shadow:0 4px 14px rgba(0,0,0,.35);}"
".card .label{font-size:13px;color:#94a3b8;text-transform:uppercase;letter-spacing:1px;}"
".card .value{font-size:38px;font-weight:bold;margin-top:6px;}"
".in .value{color:#4ade80;}.out .value{color:#f87171;}.occ .value{color:#38bdf8;}"
".status{margin-top:22px;padding:10px 22px;border-radius:20px;font-weight:bold;}"
".status.ok{background:#14532d;color:#4ade80;}"
".status.full{background:#7f1d1d;color:#fca5a5;}"
".bar-wrap{width:280px;background:#1e293b;border-radius:10px;height:18px;margin-top:20px;overflow:hidden;}"
".bar{height:100%;background:linear-gradient(90deg,#38bdf8,#f87171);width:0%;transition:width .4s;}"
".btnrow{display:flex;gap:10px;margin-top:26px;flex-wrap:wrap;justify-content:center;}"
"button{background:#334155;color:#e2e8f0;border:none;padding:10px 22px;"
"       border-radius:8px;cursor:pointer;font-size:14px;}"
"button:hover{background:#475569;}"
"table{margin-top:30px;border-collapse:collapse;width:min(600px,92vw);font-size:13px;}"
"th,td{padding:8px 10px;text-align:left;border-bottom:1px solid #1e293b;}"
"th{color:#94a3b8;text-transform:uppercase;font-size:11px;letter-spacing:.5px;}"
"td.type-ENTRY{color:#4ade80;font-weight:bold;}"
"td.type-EXIT{color:#f87171;font-weight:bold;}"
".logtitle{margin-top:10px;color:#94a3b8;font-size:13px;}"
"</style></head><body>"
"<h1>Digital Visitor Counter</h1>"
"<div class=\"sub\">Live occupancy dashboard &mdash; ESP32</div>"
"<div class=\"cards\">"
"  <div class=\"card in\"><div class=\"label\">Entries</div><div class=\"value\" id=\"entries\">--</div></div>"
"  <div class=\"card out\"><div class=\"label\">Exits</div><div class=\"value\" id=\"exits\">--</div></div>"
"  <div class=\"card occ\"><div class=\"label\">Occupancy</div><div class=\"value\" id=\"occupancy\">--</div></div>"
"</div>"
"<div class=\"bar-wrap\"><div class=\"bar\" id=\"bar\"></div></div>"
"<div class=\"status ok\" id=\"status\">OK</div>"
"<div class=\"btnrow\">"
"  <button onclick=\"resetCounters()\">Reset Counters</button>"
"  <button onclick=\"window.location='/export'\">Download Log (CSV)</button>"
"</div>"
"<div class=\"logtitle\">Recent entry / exit log</div>"
"<table><thead><tr><th>Time</th><th>Type</th><th>Occupancy</th></tr></thead>"
"<tbody id=\"logbody\"><tr><td colspan=\"3\">Loading...</td></tr></tbody></table>"
"<script>"
"async function refresh(){"
"  try{"
"    const r=await fetch('/status');const d=await r.json();"
"    document.getElementById('entries').textContent=d.entries;"
"    document.getElementById('exits').textContent=d.exits;"
"    document.getElementById('occupancy').textContent=d.occupancy;"
"    const pct=Math.min(100,(d.occupancy/d.maxCapacity)*100);"
"    document.getElementById('bar').style.width=pct+'%';"
"    const st=document.getElementById('status');"
"    if(d.full){st.textContent='ROOM FULL';st.className='status full';}"
"    else{st.textContent='OK ('+d.occupancy+' / '+d.maxCapacity+')';st.className='status ok';}"
"  }catch(e){console.log('fetch failed',e);}"
"}"
"async function refreshLog(){"
"  try{"
"    const r=await fetch('/log');const rows=await r.json();"
"    const body=document.getElementById('logbody');"
"    if(!rows.length){body.innerHTML='<tr><td colspan=\"3\">No events yet.</td></tr>';return;}"
"    body.innerHTML=rows.slice().reverse().map(e=>"
"      `<tr><td>${e.timestamp}</td><td class=\"type-${e.type}\">${e.type}</td><td>${e.occupancy}</td></tr>`"
"    ).join('');"
"  }catch(e){console.log('log fetch failed',e);}"
"}"
"function resetCounters(){"
"  if(confirm('Reset entry/exit counters?'))fetch('/reset').then(()=>{refresh();refreshLog();});"
"}"
"setInterval(refresh,1000);setInterval(refreshLog,3000);refresh();refreshLog();"
"</script></body></html>";

/* ====================================================================
   HTTP handler helpers
   ==================================================================== */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"entries\":%lu,\"exits\":%lu,\"occupancy\":%ld,"
        "\"maxCapacity\":%d,\"full\":%s}",
        (unsigned long)s_state->entries,
        (unsigned long)s_state->exits,
        (long)s_state->occupancy,
        s_state->max_capacity,
        s_state->room_full ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t handler_reset(httpd_req_t *req)
{
    /* NOTE: caller (main.c) owns the mutex; a simple write here is
       safe because this handler runs on the HTTP server task and the
       FSM task only reads room_full / occupancy atomically. For a
       production build, guard with a mutex. */
    s_state->entries   = 0;
    s_state->exits     = 0;
    s_state->occupancy = 0;
    s_state->room_full = false;
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t handler_log(httpd_req_t *req)
{
    const size_t BUF = 8192;
    char *buf = malloc(BUF);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    log_read_recent_json(buf, BUF, LOG_JSON_MAX_ROWS);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static esp_err_t handler_export(httpd_req_t *req)
{
#if ENABLE_LOCAL_LOG
    FILE *f = fopen(LOG_FILE_PATH, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"visitor_log.csv\"");
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        httpd_resp_sendstr_chunk(req, line);
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0); /* end chunked response */
#else
    return httpd_resp_send_404(req);
#endif
}

static esp_err_t handler_clearlog(httpd_req_t *req)
{
#if ENABLE_LOCAL_LOG
    log_clear();
    return httpd_resp_sendstr(req, "OK");
#else
    return httpd_resp_send_404(req);
#endif
}

/* ====================================================================
   Web server startup
   ==================================================================== */

void web_server_start(counter_state_t *state)
{
    s_state = state;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server.");
        return;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET, .handler = handler_root    },
        { .uri = "/status",  .method = HTTP_GET, .handler = handler_status  },
        { .uri = "/reset",   .method = HTTP_GET, .handler = handler_reset   },
        { .uri = "/log",     .method = HTTP_GET, .handler = handler_log     },
        { .uri = "/export",  .method = HTTP_GET, .handler = handler_export  },
        { .uri = "/clearlog",.method = HTTP_GET, .handler = handler_clearlog},
    };
    for (int i = 0; i < 6; i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web server started.");
}

/* ====================================================================
   Firebase — HTTPS REST (POST = new event, PUT = status snapshot)
   ==================================================================== */
#if ENABLE_FIREBASE

static bool firebase_request(const char *method, const char *path, const char *json_body)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s.json?auth=%s",
             FIREBASE_HOST, path, FIREBASE_AUTH);

    esp_http_client_config_t cfg = {
        .url                     = url,
        .transport_type          = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,  /* matches original setInsecure() */
        .crt_bundle_attach       = esp_crt_bundle_attach,
        .timeout_ms              = 4000,   /* fail fast on no-internet networks */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_method(client,
        strcmp(method, "PUT") == 0 ? HTTP_METHOD_PUT : HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int code      = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || code >= 400) {
        ESP_LOGW(TAG, "[Firebase] %s %s -> HTTP %d  err=%s",
                 method, path, code, esp_err_to_name(err));
        return false;
    }
    return true;
}

void firebase_push_event(const char *type, const char *timestamp,
                         time_t epoch, const counter_state_t *state)
{
    char body[256];
    snprintf(body, sizeof(body),
        "{\"type\":\"%s\",\"timestamp\":\"%s\",\"epoch\":%ld,"
        "\"entries\":%lu,\"exits\":%lu,\"occupancy\":%ld}",
        type, timestamp, (long)epoch,
        (unsigned long)state->entries,
        (unsigned long)state->exits,
        (long)state->occupancy);
    firebase_request("POST", "/logs", body);
}

void firebase_push_status(const counter_state_t *state)
{
    char body[256];
    snprintf(body, sizeof(body),
        "{\"entries\":%lu,\"exits\":%lu,\"occupancy\":%ld,"
        "\"maxCapacity\":%d,\"full\":%s,\"lastUpdate\":%ld}",
        (unsigned long)state->entries,
        (unsigned long)state->exits,
        (long)state->occupancy,
        state->max_capacity,
        state->room_full ? "true" : "false",
        (long)time(NULL));
    firebase_request("PUT", "/status", body);
}

#else
void firebase_push_event(const char *t, const char *ts, time_t e, const counter_state_t *s)
    { (void)t;(void)ts;(void)e;(void)s; }
void firebase_push_status(const counter_state_t *s) { (void)s; }
#endif /* ENABLE_FIREBASE */

/* ====================================================================
   ThingSpeak — plain HTTP GET
   ==================================================================== */
#if ENABLE_THINGSPEAK

void thingspeak_push(const counter_state_t *state)
{
    char url[256];
    snprintf(url, sizeof(url),
        "http://api.thingspeak.com/update?api_key=%s"
        "&field1=%lu&field2=%lu&field3=%ld",
        THINGSPEAK_API_KEY,
        (unsigned long)state->entries,
        (unsigned long)state->exits,
        (long)state->occupancy);

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000 };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "ThingSpeak upload -> HTTP %d", code);
    esp_http_client_cleanup(client);
}

#else
void thingspeak_push(const counter_state_t *s) { (void)s; }
#endif /* ENABLE_THINGSPEAK */
