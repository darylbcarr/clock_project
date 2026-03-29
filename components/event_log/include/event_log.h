#pragma once
#include <cstdint>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  // portMUX_TYPE

/**
 * Log categories.  Six categories → bitmask fits in a uint8_t.
 * The enabled mask is persisted in NVS so filter settings survive reboots.
 */
enum class LogCat : uint8_t {
    CLOCK_SENSOR  = 0,  ///< Optical-sensor drift corrections (auto corrections at :00)
    CLOCK_STARTUP = 1,  ///< Boot / SNTP-sync hand alignment, DST jumps
    CLOCK_SET     = 2,  ///< Manual set-time commands from web / console
    CLOCK_TICK    = 3,  ///< Per-minute routine tick (advance, no correction)
    LIGHT_WEB     = 4,  ///< LED changes from the web UI
    LIGHT_MATTER  = 5,  ///< LED changes from Matter (Alexa, HomeKit, etc.)
    _COUNT        = 6,
};

constexpr uint8_t LOG_ALL_MASK  = (1u << static_cast<uint8_t>(LogCat::_COUNT)) - 1u;
constexpr uint8_t LOG_NONE_MASK = 0u;

/** Single log entry — 64 bytes. */
struct LogEntry {
    uint32_t ts;        ///< Unix epoch seconds (0 = pre-SNTP-sync)
    uint32_t uptime_s;  ///< Seconds since boot (always valid)
    uint8_t  cat;       ///< LogCat cast to uint8_t
    char     msg[51];   ///< Null-terminated message (truncated to 50 chars)
};  // 4+4+1+1(pad)+51+3(pad) = 64 bytes

/**
 * Lightweight in-memory ring-buffer event log.
 *
 * Thread-safe via a FreeRTOS critical section (works from ISR too).
 * Capacity: 200 entries (12.8 KB RAM).  Oldest entry is silently overwritten
 * when the buffer fills (round-robin).
 *
 * The per-category enabled mask is persisted in NVS ("clk_cfg" / "log_mask")
 * so that filter preferences survive power cycles.
 */
class EventLog {
public:
    static constexpr int CAPACITY = 200;

    /** Log an event.  No-op if the category is currently disabled. */
    static void log(LogCat cat, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));

    /**
     * Build a JSON string of log entries (newest-first).
     * cat_mask: bitmask of categories to include (LOG_ALL_MASK = all).
     * Returns a malloc'd string — caller must free().  Returns nullptr on OOM.
     */
    static char* build_json(uint8_t cat_mask = LOG_ALL_MASK);

    /** Erase all entries. */
    static void clear();

    /** Number of entries currently in the buffer (0–CAPACITY). */
    static int  count();

    /** Get / set the per-category enabled mask.  set_enabled_mask persists to NVS. */
    static uint8_t get_enabled_mask();
    static void    set_enabled_mask(uint8_t mask);

    /** Load the saved mask from NVS.  Call once after nvs_flash_init(). */
    static void load_config();

private:
    static LogEntry      s_buf_[CAPACITY];
    static int           s_head_;    ///< Next write slot (ring index)
    static int           s_count_;   ///< Entries present (capped at CAPACITY)
    static uint8_t       s_enabled_; ///< Bitmask of active categories
    static portMUX_TYPE  s_mux_;
};
