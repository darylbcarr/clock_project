/**
 * @file console_commands.cpp
 * @brief Simple UART line-oriented command shell
 *
 * No esp_console / argtable3 dependency.  Uses only driver/uart.h which is
 * part of the core ESP-IDF build and never needs a managed component.
 */

#include "console_commands.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "console";

// ── UART configuration ────────────────────────────────────────────────────────
static constexpr uart_port_t  CONSOLE_UART    = UART_NUM_0;
static constexpr int          CONSOLE_BAUD    = 115200;
static constexpr int          CONSOLE_RX_BUF  = 512;
static constexpr int          CONSOLE_LINE_MAX = 128;

// ── Module-level clock pointer ────────────────────────────────────────────────
static ClockManager* s_clock = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void uart_puts(const char* s)
{
    uart_write_bytes(CONSOLE_UART, s, strlen(s));
}

static void prompt()
{
    uart_puts("\r\nclock> ");
}

// Read one '\n'-terminated line from UART into buf (max len-1 chars).
// Echos characters back.  Returns number of chars in buf (excluding '\0').
static int read_line(char* buf, int len)
{
    int pos = 0;
    buf[0]  = '\0';

    while (pos < len - 1) {
        uint8_t c;
        int n = uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY);
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            uart_write_bytes(CONSOLE_UART, "\r\n", 2);
            break;
        }
        if (c == 0x7F || c == 0x08) {  // backspace / DEL
            if (pos > 0) {
                --pos;
                uart_puts("\b \b");
            }
            continue;
        }
        // Echo printable chars only
        if (c >= 0x20 && c < 0x7F) {
            uart_write_bytes(CONSOLE_UART, (char*)&c, 1);
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return pos;
}

// ── Command handlers ──────────────────────────────────────────────────────────

static void do_help()
{
    uart_puts(
        "\r\nAvailable commands:\r\n"
        "  calibrate               Measure dark baseline, set sensor threshold\r\n"
        "  measure                 Print average sensor ADC reading\r\n"
        "  set-offset <seconds>    Sensor-to-hour offset in seconds\r\n"
        "  set-time [<minute>]     Move hand to match SNTP time (0-59)\r\n"
        "  microstep <n> [fwd|bwd] Fine-adjust hand by N half-steps\r\n"
        "  advance                 Force one test minute advance\r\n"
        "  status                  Print full system status\r\n"
        "  time [<fmt>]            Print time (optional strftime format)\r\n"
        "  help                    Show this list\r\n"
    );
}

// Tokenise buf in-place.  argv[] points into buf.  Returns argc.
static int tokenise(char* buf, char* argv[], int max_argc)
{
    int argc = 0;
    char* p  = buf;
    while (*p && argc < max_argc) {
        // skip spaces
        while (*p == ' ') ++p;
        if (!*p) break;
        argv[argc++] = p;
        // find end of token
        while (*p && *p != ' ') ++p;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void dispatch(char* line)
{
    if (!line || line[0] == '\0') return;

    char* argv[8];
    int   argc = tokenise(line, argv, 8);
    if (argc == 0) return;

    const char* cmd = argv[0];

    // ── calibrate ─────────────────────────────────────────────────────────────
    if (strcmp(cmd, "calibrate") == 0) {
        s_clock->cmd_calibrate_sensor();
        return;
    }

    // ── measure ───────────────────────────────────────────────────────────────
    if (strcmp(cmd, "measure") == 0) {
        int avg = s_clock->cmd_measure_sensor_average();
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "Sensor avg: %d\r\n", avg);
        uart_puts(tmp);
        return;
    }

    // ── set-offset <seconds> ─────────────────────────────────────────────────
    if (strcmp(cmd, "set-offset") == 0) {
        if (argc < 2) { uart_puts("Usage: set-offset <seconds>\r\n"); return; }
        int sec = atoi(argv[1]);
        s_clock->cmd_set_sensor_offset(sec);
        return;
    }

    // ── set-time [<minute>] ───────────────────────────────────────────────────
    if (strcmp(cmd, "set-time") == 0) {
        int cur_min = (argc >= 2) ? atoi(argv[1]) : -1;
        s_clock->cmd_set_time(cur_min);
        return;
    }

    // ── microstep <n> [fwd|bwd] ───────────────────────────────────────────────
    if (strcmp(cmd, "microstep") == 0) {
        if (argc < 2) { uart_puts("Usage: microstep <steps> [fwd|bwd]\r\n"); return; }
        int  steps   = atoi(argv[1]);
        bool forward = (argc < 3) || (strcmp(argv[2], "bwd") != 0);
        s_clock->cmd_microstep(steps, forward);
        return;
    }

    // ── advance ───────────────────────────────────────────────────────────────
    if (strcmp(cmd, "advance") == 0) {
        s_clock->cmd_test_advance();
        return;
    }

    // ── status ────────────────────────────────────────────────────────────────
    if (strcmp(cmd, "status") == 0) {
        s_clock->cmd_status();
        return;
    }

    // ── time [<fmt>] ──────────────────────────────────────────────────────────
    if (strcmp(cmd, "time") == 0) {
        const char* fmt = (argc >= 2) ? argv[1] : "%Y-%m-%dT%H:%M:%S";
        std::string t   = s_clock->format_time(fmt);
        uart_puts(t.c_str());
        uart_puts("\r\n");
        return;
    }

    // ── help ──────────────────────────────────────────────────────────────────
    if (strcmp(cmd, "help") == 0) {
        do_help();
        return;
    }

    uart_puts("Unknown command. Type 'help' for a list.\r\n");
}

// ── UART shell task ───────────────────────────────────────────────────────────

static void console_task(void* /*arg*/)
{
    char line[CONSOLE_LINE_MAX];

    uart_puts("\r\n=== Clock console ready. Type 'help' for commands. ===\r\n");
    prompt();

    while (true) {
        read_line(line, sizeof(line));
        dispatch(line);
        prompt();
    }
}

// ── Public entry point ────────────────────────────────────────────────────────

void console_start(ClockManager* clock_mgr)
{
    s_clock = clock_mgr;

    // Install UART driver with RX ring buffer; no TX buffer (blocking writes).
    uart_config_t cfg = {};
    cfg.baud_rate  = CONSOLE_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART, &cfg));
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUF, 0, 0, nullptr, 0));

    xTaskCreate(console_task, "console", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "UART console started at %d baud", CONSOLE_BAUD);
}
