#pragma once

/**
 * @file ota_manager.h
 * @brief Over-the-air firmware update via GitHub Releases
 *
 * Checks a version.json file at a fixed URL once at boot (after WiFi connects)
 * and then every CHECK_INTERVAL_H hours.  If the reported version differs from
 * the running firmware, it downloads and installs the new binary via
 * esp_https_ota and restarts.
 *
 * version.json format (hosted on GitHub, updated by CI on each release):
 *   {
 *     "version": "1.2.0",
 *     "url": "https://github.com/OWNER/REPO/releases/download/v1.2.0/clock_project.bin"
 *   }
 *
 * The VERSION_CHECK_URL below must be updated to match your GitHub repository.
 */

#include <functional>
#include "esp_err.h"

class Display;

class OtaManager {
public:
    // ── Update this to your repo ──────────────────────────────────────────────
    static constexpr const char* VERSION_CHECK_URL =
        "https://raw.githubusercontent.com/darylbcarr/clock_project/main/version.json";

    /**
     * @brief Launch the background OTA check task.
     *
     * @param display          Used to show download progress on the OLED.
     * @param is_connected_fn  Returns true when WiFi is up and routable.
     */
    void start(Display& display, std::function<bool()> is_connected_fn);

    /**
     * @brief Trigger an immediate version check (e.g. from console or menu).
     *        Safe to call from any task — runs inline on the caller's stack.
     *        Returns ESP_OK if no update was needed or update succeeded.
     */
    esp_err_t check_now();

    /** @brief Return the running firmware version string (from PROJECT_VER). */
    static const char* running_version();

private:
    static void  ota_task(void* arg);
    esp_err_t    do_check_and_update();
    bool         fetch_version_info(char* out_ver,  size_t ver_len,
                                    char* out_url,  size_t url_len);

    Display*              display_         = nullptr;
    std::function<bool()> is_connected_fn_ = nullptr;

    static constexpr int  BOOT_DELAY_S     = 60;      // wait after boot before first check
    static constexpr int  CHECK_INTERVAL_S = 24*3600; // recheck every 24 h
    static constexpr int  HTTP_TIMEOUT_MS  = 15000;
    static constexpr int  VER_BUF_LEN      = 32;
    static constexpr int  URL_BUF_LEN      = 256;
};
