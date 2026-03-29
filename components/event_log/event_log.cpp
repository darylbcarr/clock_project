#include "event_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>

// ── Static storage ────────────────────────────────────────────────────────────

LogEntry     EventLog::s_buf_[EventLog::CAPACITY];
int          EventLog::s_head_    = 0;
int          EventLog::s_count_   = 0;
uint8_t      EventLog::s_enabled_ = LOG_NONE_MASK;
portMUX_TYPE EventLog::s_mux_     = portMUX_INITIALIZER_UNLOCKED;

static constexpr const char* NVS_NS  = "clk_cfg";
static constexpr const char* NVS_KEY = "log_mask";

// Human-readable category names (must match LogCat order).
static const char* const CAT_NAMES[] = {
    "Sensor Adj",   // 0 CLOCK_SENSOR
    "Startup/Sync", // 1 CLOCK_STARTUP
    "Clock Set",    // 2 CLOCK_SET
    "Tick",         // 3 CLOCK_TICK
    "Light Web",    // 4 LIGHT_WEB
    "Light Matter", // 5 LIGHT_MATTER
};
static constexpr int CAT_COUNT = static_cast<int>(LogCat::_COUNT);

// ── log ───────────────────────────────────────────────────────────────────────

void EventLog::log(LogCat cat, const char* fmt, ...)
{
    uint8_t bit = 1u << static_cast<uint8_t>(cat);
    if (!(s_enabled_ & bit)) return;

    LogEntry e;
    e.ts       = static_cast<uint32_t>(time(nullptr));
    e.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1'000'000ULL);
    e.cat      = static_cast<uint8_t>(cat);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_mux_);
    s_buf_[s_head_] = e;
    s_head_ = (s_head_ + 1) % CAPACITY;
    if (s_count_ < CAPACITY) s_count_++;
    portEXIT_CRITICAL(&s_mux_);
}

// ── build_json ────────────────────────────────────────────────────────────────

char* EventLog::build_json(uint8_t cat_mask)
{
    // Snapshot the ring buffer under the critical section.
    portENTER_CRITICAL(&s_mux_);
    int count = s_count_;
    int head  = s_head_;
    // Stack-allocate would be ~12 KB — use heap snapshot instead.
    LogEntry* snap = static_cast<LogEntry*>(malloc(sizeof(LogEntry) * CAPACITY));
    if (snap) memcpy(snap, s_buf_, sizeof(LogEntry) * CAPACITY);
    portEXIT_CRITICAL(&s_mux_);

    if (!snap) return nullptr;

    cJSON* root = cJSON_CreateObject();
    if (!root) { free(snap); return nullptr; }

    cJSON_AddNumberToObject(root, "total",        count);
    cJSON_AddNumberToObject(root, "enabled_mask", s_enabled_);

    cJSON* arr = cJSON_AddArrayToObject(root, "entries");
    if (!arr) { cJSON_Delete(root); free(snap); return nullptr; }

    // s_head_ is the NEXT write slot; most-recent entry is at (head-1) mod CAPACITY.
    int n = (count < CAPACITY) ? count : CAPACITY;
    for (int i = 0; i < n; i++) {
        int idx = ((head - 1 - i) + 2 * CAPACITY) % CAPACITY;
        const LogEntry& e = snap[idx];

        uint8_t bit = 1u << e.cat;
        if (!(cat_mask & bit)) continue;

        cJSON* item = cJSON_CreateObject();
        if (!item) break;

        // Format timestamp string.
        // epoch > year-2000 means SNTP has synced → show calendar date/time.
        // Otherwise show uptime (always available).
        char ts_str[24];
        static constexpr uint32_t EPOCH_2000 = 946684800u;
        if (e.ts > EPOCH_2000) {
            time_t t = static_cast<time_t>(e.ts);
            struct tm tm_buf;
            localtime_r(&t, &tm_buf);
            snprintf(ts_str, sizeof(ts_str), "%02d/%02d %02d:%02d:%02d",
                     tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        } else {
            uint32_t h = e.uptime_s / 3600u;
            uint32_t m = (e.uptime_s % 3600u) / 60u;
            uint32_t s = e.uptime_s % 60u;
            snprintf(ts_str, sizeof(ts_str), "up %lu:%02lu:%02lu",
                     (unsigned long)h, (unsigned long)m, (unsigned long)s);
        }

        const char* cat_name = (e.cat < CAT_COUNT) ? CAT_NAMES[e.cat] : "?";

        cJSON_AddStringToObject(item, "ts_str",   ts_str);
        cJSON_AddNumberToObject(item, "cat",       e.cat);
        cJSON_AddStringToObject(item, "cat_name",  cat_name);
        cJSON_AddStringToObject(item, "msg",       e.msg);

        cJSON_AddItemToArray(arr, item);
    }

    free(snap);
    char* result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result;
}

// ── clear / count ─────────────────────────────────────────────────────────────

void EventLog::clear()
{
    portENTER_CRITICAL(&s_mux_);
    s_head_  = 0;
    s_count_ = 0;
    portEXIT_CRITICAL(&s_mux_);
}

int EventLog::count()
{
    portENTER_CRITICAL(&s_mux_);
    int c = s_count_;
    portEXIT_CRITICAL(&s_mux_);
    return c;
}

// ── enabled mask ─────────────────────────────────────────────────────────────

uint8_t EventLog::get_enabled_mask()
{
    return s_enabled_;
}

void EventLog::set_enabled_mask(uint8_t mask)
{
    s_enabled_ = mask & LOG_ALL_MASK;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, s_enabled_);
        nvs_commit(h);
        nvs_close(h);
    }
}

void EventLog::load_config()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = LOG_NONE_MASK;
        nvs_get_u8(h, NVS_KEY, &v);
        s_enabled_ = v & LOG_ALL_MASK;
        nvs_close(h);
    }
}
