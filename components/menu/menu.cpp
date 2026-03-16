/**
 * @file menu.cpp
 * @brief Full menu system wired to ClockManager and Networking
 *
 * Menu structure:
 *   Main
 *   ├── Clock
 *   │   ├── Set Time          (set-time using current SNTP)
 *   │   ├── Set Clock         (manual HH:MM entry via encoder)
 *   │   ├── Microstep Fwd     (single microstep forward)
 *   │   ├── Microstep Bwd     (single microstep backward)
 *   │   ├── Advance 1 Min     (test advance)
 *   │   ├── Set Sensor Offset (adjust via encoder)
 *   │   └── Calibrate Sensor  (run dark baseline)
 *   ├── Status
 *   │   ├── Clock Status      (displayed min, sensor, offsets)
 *   │   ├── Network Status    (IP, SSID, RSSI, geo)
 *   │   └── Time & Sync       (local time, SNTP state, TZ)
 *   ├── Network
 *   │   ├── Net Info          (full net-status dump)
 *   │   └── Sync Status       (SNTP sync detail)
 *   ├── System
 *   │   ├── Uptime
 *   │   └── About
 *   └── Lights (placeholder for WS2812B)
 *       ├── Color
 *       └── Brightness
 *
 * Display blanks after 5 minutes of inactivity.
 * Any encoder event (rotation or button) wakes it.
 */

#include "menu.h"
#include "display.h"
#include "clock_manager.h"
#include "networking.h"
#include "led_manager.h"
#include "config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static const char* TAG = "Menu";

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string center_str(const std::string& s, int width = 16) {
    int len = static_cast<int>(s.size());
    if (len >= width) return s.substr(0, width);
    int left = (width - len) / 2;
    return std::string(left, ' ') + s;
}

// ── MenuItem ─────────────────────────────────────────────────────────────────

MenuItem::MenuItem(const std::string& name)
    : name_(name), parent_(nullptr), callback_(nullptr) {}

MenuItem::MenuItem(const std::string& name, Callback callback)
    : name_(name), parent_(nullptr), callback_(callback) {}

void MenuItem::addChild(std::unique_ptr<MenuItem> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
}

void MenuItem::execute() const {
    if (callback_) {
        callback_();
    } else {
        ESP_LOGI(TAG, "No callback for: %s", name_.c_str());
    }
}

// ── Menu core ─────────────────────────────────────────────────────────────────

Menu::Menu(Display& display)
    : display_(display)
{
    action_sem_   = xSemaphoreCreateBinary();
    xSemaphoreGive(action_sem_);   // start available
    action_queue_ = xQueueCreate(1, sizeof(MenuItem::Callback*));
    xTaskCreate(action_task_fn, "menu_act", 4096, this, 3, &action_task_handle_);
}

void Menu::next() {
    if (!current_menu_) return;
    if (current_selection_ < current_menu_->getChildren().size() - 1) {
        current_selection_++;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_    = 1;
    }
}

void Menu::previous() {
    if (!current_menu_) return;
    if (current_selection_ > 0) {
        current_selection_--;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_    = 1;
    }
}

void Menu::select() {
    if (!current_menu_) return;
    const auto& children = current_menu_->getChildren();
    if (children.empty()) return;
    auto* selected = children[current_selection_].get();
    if (selected->hasChildren()) {
        current_menu_      = selected;
        current_selection_ = 0;
        display_start_     = 0;
        h_scroll_offset_   = 0;
        h_scroll_dir_      = 1;
        ESP_LOGI(TAG, "Entering: %s", selected->getName().c_str());
    } else {
        ESP_LOGI(TAG, "Execute: %s", selected->getName().c_str());
        selected->execute();
    }
}

void Menu::back() {
    if (!current_menu_ || !current_menu_->getParent()) return;
    current_menu_      = current_menu_->getParent();
    current_selection_ = 0;
    display_start_     = 0;
    h_scroll_offset_   = 0;
    h_scroll_dir_      = 1;
}

void Menu::updateDisplayStart() {
    if (current_selection_ < display_start_) {
        display_start_ = current_selection_;
    } else if (current_selection_ >= display_start_ + MAX_VISIBLE_ITEMS) {
        display_start_ = current_selection_ - MAX_VISIBLE_ITEMS + 1;
    }
}

std::vector<std::string> Menu::getVisibleItems() const {
    std::vector<std::string> items;
    if (!current_menu_) return items;
    const auto& children = current_menu_->getChildren();
    size_t start = display_start_;
    size_t end   = std::min(start + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = start; i < end; i++)
        items.push_back(children[i]->getName());
    while (items.size() < MAX_VISIBLE_ITEMS)
        items.emplace_back("");
    return items;
}

void Menu::render() {
    if (blanked_ || !current_menu_) return;

    const auto& children = current_menu_->getChildren();
    std::vector<std::string> lines;
    lines.reserve(1 + MAX_VISIBLE_ITEMS);

    // Line 0: centred title
    lines.push_back(center_str(current_menu_->getName()));

    // Lines 1..MAX_VISIBLE_ITEMS: items (with h-scroll on selected item)
    size_t end = std::min(display_start_ + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = display_start_; i < end; i++) {
        const std::string& name = children[i]->getName();
        if (i == current_selection_ && (int)name.length() > 16) {
            int max_off = (int)name.length() - 16;
            int off = std::min(h_scroll_offset_, max_off);
            lines.push_back(name.substr(off, 16));
        } else {
            lines.push_back(name);
        }
    }
    while ((int)lines.size() < 1 + (int)MAX_VISIBLE_ITEMS)
        lines.emplace_back("");

    // highlight_line is +1 to account for the title row
    int highlight_line = (current_selection_ >= display_start_)
                         ? (int)(current_selection_ - display_start_) + 1
                         : -1;

    display_.writeLines(lines, highlight_line);
}

void Menu::render_scrolled(bool going_next) {
    if (blanked_) return;
    display_.startHardwareScroll(
        going_next ? ScrollMode::HARDWARE_UP : ScrollMode::HARDWARE_DOWN, 7);
    vTaskDelay(pdMS_TO_TICKS(60));
    render();                      // updates buffer; refresh_display skipped while scrolling
    display_.stopHardwareScroll(); // stops scroll and flushes the new buffer
}

void Menu::tick_h_scroll() {
    if (blanked_ || !current_menu_) return;
    const auto& children = current_menu_->getChildren();
    if (children.empty()) return;
    const std::string& name = children[current_selection_]->getName();
    int name_len = (int)name.length();
    if (name_len <= 16) return;

    int max_off = name_len - 16;
    h_scroll_offset_ += h_scroll_dir_;
    if (h_scroll_offset_ >= max_off) {
        h_scroll_offset_ = max_off;
        h_scroll_dir_    = -1;
    } else if (h_scroll_offset_ <= 0) {
        h_scroll_offset_ = 0;
        h_scroll_dir_    = 1;
    }
    render();
}

// ── Display blanking ──────────────────────────────────────────────────────────

void Menu::wake() {
    idle_seconds_ = 0;
    if (blanked_) {
        blanked_ = false;
        display_.clear();
        render();
        ESP_LOGI(TAG, "Display woken");
    }
}

void Menu::tick_blank_timer() {
    if (blanked_) return;
    if (++idle_seconds_ >= BLANK_TIMEOUT_S) {
        blanked_ = true;
        display_.clear();
        ESP_LOGI(TAG, "Display blanked after %lu s idle",
                 (unsigned long)BLANK_TIMEOUT_S);
    }
}

// ── wait_for_dismiss ──────────────────────────────────────────────────────────
// Spins (yielding to the scheduler) until dismiss_fn_ returns true (button
// press edge) or until 30s timeout.  The last display line should say
// "Press to return" so the user knows what to do.

static constexpr uint32_t DISMISS_TIMEOUT_MS = 30000;
static constexpr uint32_t DISMISS_HOLD_MS    = 800;

void Menu::wait_for_dismiss() {
    // Debounce: wait for button released from the select/navigate press
    vTaskDelay(pdMS_TO_TICKS(300));

    uint32_t hold_ms = 0;
    uint32_t elapsed = 0;

    while (elapsed < DISMISS_TIMEOUT_MS) {
        bool pressed = dismiss_fn_ ? dismiss_fn_() : false;
        if (pressed) {
            hold_ms += 50;
            if (hold_ms >= DISMISS_HOLD_MS) return;  // long press detected
        } else {
            hold_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }
    ESP_LOGW("Menu", "wait_for_dismiss: timed out");
}

// ── Info sub-screens ──────────────────────────────────────────────────────────
// These are blocking screens; they spin until the button is pressed,
// then return to the caller which re-renders the menu.

void Menu::show_clock_status(ClockManager& cm) {
    char buf[20];
    display_.clear();
    display_.print(0, center_str("Clock Status").c_str());
    display_.print(1, "------------");
    snprintf(buf, sizeof(buf), "Min: %d", cm.displayed_minute());
    display_.print(2, buf);
    snprintf(buf, sizeof(buf), "Offset: %ds", cm.sensor_offset_sec());
    display_.print(3, buf);
    display_.print(4, cm.is_time_valid() ? "SNTP: synced" : "SNTP: no sync");
    display_.print(5, cm.time_hm().c_str());
    wait_for_dismiss();
    render();
}

void Menu::show_net_status(Networking& net) {
    const NetStatus& s = net.get_status();
    char buf[20];
    display_.clear();
    display_.print(0, center_str("Net Status").c_str());
    if (s.wifi_connected) {
        snprintf(buf, sizeof(buf), "%.14s", s.ssid);
        display_.print(1, buf);
        snprintf(buf, sizeof(buf), "RSSI:%ddBm", (int)s.rssi);
        display_.print(2, buf);
        snprintf(buf, sizeof(buf), "%.15s", s.local_ip);
        display_.print(3, buf);
        snprintf(buf, sizeof(buf), "%.15s", s.external_ip);
        display_.print(4, buf);
        snprintf(buf, sizeof(buf), "%.16s", s.city);
        display_.print(5, buf);
        snprintf(buf, sizeof(buf), "%.16s", s.iana_tz);
        display_.print(6, buf);
    } else {
        display_.print(1, "No connection");
    }
    wait_for_dismiss();
    render();
}

void Menu::show_time_screen(ClockManager& cm) {
    display_.clear();
    display_.print(0, center_str("Time & Sync").c_str());
    display_.print(1, cm.time_hms().c_str());
    display_.print(2, cm.date_short().c_str());
    display_.print(3, cm.is_time_valid() ? "SNTP: synced" : "SNTP: no sync");
    wait_for_dismiss();
    render();
}

void Menu::show_info_screen(ClockManager& cm, Networking& net) {
    char buf[20];
    display_.clear();
    display_.print(0, center_str("About").c_str());
    display_.print(1, "Clock Driver v1");
    display_.print(2, "ESP32-S3");
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(buf, sizeof(buf), "Up: %luh %02lum",
             (unsigned long)(uptime_s / 3600),
             (unsigned long)((uptime_s % 3600) / 60));
    display_.print(3, buf);
    display_.print(4, net.is_connected() ? "Net: OK" : "Net: offline");
    wait_for_dismiss();
    render();
}

// ── show_text_input ───────────────────────────────────────────────────────────

// Flat character palette — encoder scrolls through the whole list.
// Ordered: space · a-z · A-Z · 0-9 · symbols
static const char  CHARSET[]     = " abcdefghijklmnopqrstuvwxyz"
                                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "0123456789"
                                    "!@#$%^&*()-_=+[]{}|;:'\",.<>/?~`";
static constexpr int CHARSET_LEN = (int)(sizeof(CHARSET) - 1);

// First index of each group — used by Button A to jump between groups.
static constexpr int CHARSET_GROUPS[]    = { 0, 1, 27, 53, 63 };
static constexpr int CHARSET_GROUP_COUNT = 5;

std::string Menu::show_text_input(const std::string& title, bool mask, size_t max_len)
{
    if (!input_poll_fn_) return {};

    std::string result;
    int  char_idx     = 1;    // start at 'a'
    bool enc_btn_last = false;
    bool btnA_last    = false;
    bool btnB_last    = false;
    bool both_last    = false;
    uint32_t enc_hold_ms  = 0;
    bool     enc_longfire = false;

    auto render_input = [&]() {
        char buf[20];
        display_.clear();

        // Row 0: title ("SSID:" / "Password:")
        display_.print(0, title.c_str());

        // Row 1: entered text + cursor (show last 15 chars, mask passwords)
        std::string visible = mask ? std::string(result.size(), '*') : result;
        visible += '_';
        if (visible.size() > 15) visible = visible.substr(visible.size() - 15);
        display_.print(1, visible.c_str());

        // Row 3: character neighbourhood  prev [cur] next
        char prev_c = CHARSET[(char_idx - 1 + CHARSET_LEN) % CHARSET_LEN];
        char cur_c  = CHARSET[char_idx];
        char next_c = CHARSET[(char_idx + 1) % CHARSET_LEN];
        snprintf(buf, sizeof(buf), "  %c [%c] %c",
                 (prev_c == ' ') ? '<' : prev_c,
                 (cur_c  == ' ') ? '_' : cur_c,
                 (next_c == ' ') ? '>' : next_c);
        display_.print(3, buf);

        // Row 4: current group indicator
        const char* grp;
        if      (char_idx == 0)                      grp = "  [space]";
        else if (char_idx >= 1  && char_idx <= 26)   grp = "  [a-z]";
        else if (char_idx >= 27 && char_idx <= 52)   grp = "  [A-Z]";
        else if (char_idx >= 53 && char_idx <= 62)   grp = "  [0-9]";
        else                                          grp = "  [!@#]";
        display_.print(4, grp);

        // Row 6-7: control reminder
        display_.print(6, "Pr:add  LP:del");
        display_.print(7, "A:grp  A+B:ok");
    };

    // Brief instructions before entering the loop
    display_.clear();
    display_.print(0, title.c_str());
    display_.print(2, "Rot:char  Pr:add");
    display_.print(3, "LP:del    B:spc");
    display_.print(4, "A:grp  A+B:done");
    vTaskDelay(pdMS_TO_TICKS(2000));
    render_input();

    while (true) {
        InputEvent ev = input_poll_fn_();
        bool enc_btn = ev.enc_btn;
        bool btnA    = ev.btnA;
        bool btnB    = ev.btnB;
        bool both    = btnA && btnB;

        bool enc_rise  = !enc_btn && enc_btn_last;
        bool btnA_rise = !btnA && btnA_last && !both_last;
        bool btnB_rise = !btnB && btnB_last && !both_last;
        bool both_fall = both && !both_last;

        // Both GPIO buttons → confirm and exit
        if (both_fall) break;

        // Encoder rotation → scroll through character palette
        if (ev.delta != 0) {
            char_idx = ((char_idx + (int)ev.delta) % CHARSET_LEN + CHARSET_LEN) % CHARSET_LEN;
            render_input();
        }

        // Encoder button: held → backspace (long press), released → add char (short press)
        if (enc_btn) {
            if (!enc_longfire) {
                enc_hold_ms += 50;
                if (enc_hold_ms >= 800) {
                    enc_longfire = true;
                    if (!result.empty()) { result.pop_back(); render_input(); }
                }
            }
        } else {
            if (enc_rise && !enc_longfire) {
                if (result.size() < max_len) { result += CHARSET[char_idx]; render_input(); }
            }
            enc_hold_ms  = 0;
            enc_longfire = false;
        }

        // Button A release → jump to next character group
        if (btnA_rise) {
            int cur_grp = 0;
            for (int g = CHARSET_GROUP_COUNT - 1; g >= 0; g--) {
                if (char_idx >= CHARSET_GROUPS[g]) { cur_grp = g; break; }
            }
            char_idx = CHARSET_GROUPS[(cur_grp + 1) % CHARSET_GROUP_COUNT];
            render_input();
        }

        // Button B release → add space shortcut
        if (btnB_rise) {
            if (result.size() < max_len) { result += ' '; render_input(); }
        }

        enc_btn_last = enc_btn;
        btnA_last    = btnA;
        btnB_last    = btnB;
        both_last    = both;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return result;
}

// ── set_matter_pairing_info ───────────────────────────────────────────────────

void Menu::set_matter_pairing_info(uint32_t pin, uint16_t disc,
                                   const std::string& code)
{
    matter_pin_  = pin;
    matter_disc_ = disc;
    matter_code_ = code;
}

// ── show_matter_pairing_screen ────────────────────────────────────────────────
// Shared between the menu item and first_time_setup; blocks until A+B held.

static void show_matter_pairing_screen(Display& display,
                                       uint32_t pin, uint16_t disc,
                                       const std::string& code,
                                       Menu::InputPollFn poll)
{
    char buf[20];
    display.clear();
    display.print(0, "  Matter Pairing");
    display.print(1, "Open Home/Alexa");
    display.print(2, "app, enter code:");
    snprintf(buf, sizeof(buf), " %s", code.empty() ? "see UART" : code.c_str());
    display.print(3, buf);
    display.print(4, "──────────────");
    snprintf(buf, sizeof(buf), " PIN: %lu", (unsigned long)pin);
    display.print(5, buf);
    snprintf(buf, sizeof(buf), " Disc: %u", (unsigned)disc);
    display.print(6, buf);
    display.print(7, "A+B when done");

    if (!poll) return;

    // Wait for A+B hold (~500ms) to dismiss — no timeout for commissioning
    vTaskDelay(pdMS_TO_TICKS(500));   // debounce after selection
    while (true) {
        auto ev = poll();
        if (ev.btnA && ev.btnB) {
            vTaskDelay(pdMS_TO_TICKS(500));
            ev = poll();
            if (ev.btnA && ev.btnB) break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── first_time_setup ──────────────────────────────────────────────────────────

bool Menu::first_time_setup()
{
    if (!input_poll_fn_) return false;

    // ── 2-item choice screen ──────────────────────────────────────────────────
    int  choice        = 0;   // 0 = Matter, 1 = Setup WiFi
    bool enc_btn_last  = false;
    bool btnA_last     = false;
    bool btnB_last     = false;
    bool both_last     = false;
    uint32_t enc_hold  = 0;
    bool enc_longfire  = false;
    uint32_t btnA_hold = 0;
    bool btnA_longfire = false;
    uint32_t btnB_hold = 0;
    bool btnB_longfire = false;

    auto render_choice = [&]() {
        display_.clear();
        display_.print(0, "  First-Time Setup");
        display_.print(2, choice == 0 ? "> Matter"     : "  Matter");
        display_.print(3, choice == 1 ? "> Setup WiFi" : "  Setup WiFi");
        display_.print(5, "Rot/A/B: pick");
        display_.print(6, "Enc/LongAB: ok");
    };
    render_choice();

    bool selected = false;
    while (!selected) {
        InputEvent ev  = input_poll_fn_();
        bool enc_btn   = ev.enc_btn;
        bool btnA      = ev.btnA;
        bool btnB      = ev.btnB;
        bool both      = btnA && btnB;

        bool enc_rise  = !enc_btn && enc_btn_last;
        bool btnA_rise = !btnA   && btnA_last && !both_last;
        bool btnB_rise = !btnB   && btnB_last && !both_last;

        // Rotation or short A/B → toggle selection
        if (ev.delta != 0) { choice = (choice + 1) % 2; render_choice(); }
        if (btnA_rise && !btnA_longfire) { choice = (choice + 1) % 2; render_choice(); }
        if (btnB_rise && !btnB_longfire) { choice = (choice + 1) % 2; render_choice(); }

        // Encoder short press → confirm
        if (enc_rise && !enc_longfire) selected = true;

        // Encoder long press → confirm
        if (enc_btn) {
            if (!enc_longfire && (enc_hold += 50) >= 800) {
                enc_longfire = true;  selected = true;
            }
        } else { enc_hold = 0;  enc_longfire = false; }

        // Long press A or B → confirm
        if (btnA && !both) {
            if (!btnA_longfire && (btnA_hold += 50) >= 800) {
                btnA_longfire = true;  selected = true;
            }
        } else { btnA_hold = 0;  btnA_longfire = false; }

        if (btnB && !both) {
            if (!btnB_longfire && (btnB_hold += 50) >= 800) {
                btnB_longfire = true;  selected = true;
            }
        } else { btnB_hold = 0;  btnB_longfire = false; }

        enc_btn_last = enc_btn;
        btnA_last    = btnA;
        btnB_last    = btnB;
        both_last    = both;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (choice == 0) {
        // ── Matter path: show commissioning info, wait for A+B ────────────────
        show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                   matter_code_, input_poll_fn_);
        return false;

    } else {
        // ── WiFi path: SSID + password text entry ─────────────────────────────
        std::string ssid = show_text_input("SSID:", false, 63);
        if (ssid.empty()) return false;

        std::string pass = show_text_input("Password:", true, 63);

        NetCfg cfg = {};
        ConfigStore::load(cfg);
        strncpy(cfg.ssid,     ssid.c_str(), sizeof(cfg.ssid)     - 1);
        strncpy(cfg.password, pass.c_str(), sizeof(cfg.password) - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1]         = '\0';
        cfg.password[sizeof(cfg.password) - 1] = '\0';
        ConfigStore::save(cfg);

        display_.clear();
        display_.print(0, "  WiFi Saved!");
        display_.print(2, ssid.c_str());
        display_.print(4, "Connecting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return true;
    }
}

// ── Async action dispatch ─────────────────────────────────────────────────────
// Slow motor callbacks (Set Time, Advance) are posted here so they run on
// action_task and encoder_task stays responsive during motor movement.
// The semaphore ensures at most one action is queued or running at a time;
// extra presses are silently discarded.

void Menu::post_action(MenuItem::Callback cb)
{
    if (xSemaphoreTake(action_sem_, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Action already running — request discarded");
        return;
    }
    auto* cb_ptr = new MenuItem::Callback(std::move(cb));
    if (xQueueSend(action_queue_, &cb_ptr, 0) != pdTRUE) {
        delete cb_ptr;
        xSemaphoreGive(action_sem_);
    }
}

void Menu::action_task_fn(void* arg)
{
    Menu* self = static_cast<Menu*>(arg);
    MenuItem::Callback* cb_ptr = nullptr;
    while (true) {
        if (xQueueReceive(self->action_queue_, &cb_ptr, portMAX_DELAY) == pdTRUE) {
            if (cb_ptr) {
                (*cb_ptr)();
                delete cb_ptr;
                cb_ptr = nullptr;
            }
            xSemaphoreGive(self->action_sem_);
        }
    }
}

// ── build() — full menu tree with wired callbacks ────────────────────────────

void Menu::build(ClockManager& cm, Networking& net, LedManager& leds) {
    leds_ = &leds;

    auto root = std::make_unique<MenuItem>("Main");

    // ── Clock ─────────────────────────────────────────────────────────────────
    auto clk = std::make_unique<MenuItem>("Clock");

    clk->addChild(std::make_unique<MenuItem>("Set Time", [this, &cm]() {
        post_action([&cm]() { cm.cmd_set_time(-1, -1); });
    }));

    clk->addChild(std::make_unique<MenuItem>("Advance 1Min", [this, &cm]() {
        post_action([&cm]() { cm.cmd_test_advance(); });
    }));

    clk->addChild(std::make_unique<MenuItem>("Step Fwd", [&cm]() {
        cm.cmd_microstep(8, true);
    }));

    clk->addChild(std::make_unique<MenuItem>("Step Bwd", [&cm]() {
        cm.cmd_microstep(8, false);
    }));

    clk->addChild(std::make_unique<MenuItem>("Cal Sensor", [&cm]() {
        cm.cmd_calibrate_sensor();
    }));

    clk->addChild(std::make_unique<MenuItem>("Sensor Meas", [&cm]() {
        cm.cmd_measure_sensor_average();
    }));

    // ── Status ────────────────────────────────────────────────────────────────
    auto stat = std::make_unique<MenuItem>("Status");

    stat->addChild(std::make_unique<MenuItem>("Clock", [this, &cm]() {
        show_clock_status(cm);
    }));

    stat->addChild(std::make_unique<MenuItem>("Network", [this, &net]() {
        show_net_status(net);
    }));

    stat->addChild(std::make_unique<MenuItem>("Time/Sync", [this, &cm]() {
        show_time_screen(cm);
    }));

    stat->addChild(std::make_unique<MenuItem>("About", [this, &cm, &net]() {
        show_info_screen(cm, net);
    }));

    // ── Network ───────────────────────────────────────────────────────────────
    auto netm = std::make_unique<MenuItem>("Network");

    netm->addChild(std::make_unique<MenuItem>("Net Info", [this, &net]() {
        show_net_status(net);
    }));

    netm->addChild(std::make_unique<MenuItem>("Sync Status", [this, &cm]() {
        show_time_screen(cm);
    }));

    netm->addChild(std::make_unique<MenuItem>("Set WiFi", [this]() {
        // Enter SSID
        std::string ssid = show_text_input("SSID:", false, 63);
        if (ssid.empty()) { render(); return; }

        // Enter password
        std::string pass = show_text_input("Password:", true, 63);

        // Load existing config to preserve tz_override, then overwrite credentials
        NetCfg cfg;
        ConfigStore::load(cfg);
        strncpy(cfg.ssid,     ssid.c_str(), sizeof(cfg.ssid)     - 1);
        strncpy(cfg.password, pass.c_str(), sizeof(cfg.password)  - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1]         = '\0';
        cfg.password[sizeof(cfg.password) - 1] = '\0';
        ConfigStore::save(cfg);

        // Confirmation screen
        display_.clear();
        display_.print(0, "  WiFi Saved!");
        display_.print(2, ssid.c_str());
        display_.print(4, "Restart device");
        display_.print(5, "to connect.");
        vTaskDelay(pdMS_TO_TICKS(3000));
        render();
    }));

    netm->addChild(std::make_unique<MenuItem>("Matter Pair", [this]() {
        show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                   matter_code_, input_poll_fn_);
        render();
    }));

    // ── Lights ────────────────────────────────────────────────────────────────
    // s_led_tgt persists across menu interactions (static local).
    static LedManager::Target s_led_tgt = LedManager::Target::BOTH;

    auto lights = std::make_unique<MenuItem>("Lights");

    lights->addChild(std::make_unique<MenuItem>("Next Effect", [&leds]() {
        leds.next_effect(s_led_tgt);
    }));

    lights->addChild(std::make_unique<MenuItem>("Bright +", [&leds]() {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b > 230 ? 255 : b + 25);
        }
    }));

    lights->addChild(std::make_unique<MenuItem>("Bright -", [&leds]() {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b < 25 ? 0 : b - 25);
        }
    }));

    lights->addChild(std::make_unique<MenuItem>("White", [&leds]() {
        leds.set_color(s_led_tgt, 255, 255, 255);
    }));

    lights->addChild(std::make_unique<MenuItem>("Warm White", [&leds]() {
        leds.set_color(s_led_tgt, 255, 180, 80);
    }));

    lights->addChild(std::make_unique<MenuItem>("Red", [&leds]() {
        leds.set_color(s_led_tgt, 255, 0, 0);
    }));

    lights->addChild(std::make_unique<MenuItem>("Blue", [&leds]() {
        leds.set_color(s_led_tgt, 0, 80, 255);
    }));

    lights->addChild(std::make_unique<MenuItem>("Green", [&leds]() {
        leds.set_color(s_led_tgt, 0, 220, 50);
    }));

    lights->addChild(std::make_unique<MenuItem>("Purple", [&leds]() {
        leds.set_color(s_led_tgt, 180, 0, 255);
    }));

    lights->addChild(std::make_unique<MenuItem>("-> Both", []() {
        s_led_tgt = LedManager::Target::BOTH;
        ESP_LOGI(TAG, "LED target: Both");
    }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 1", []() {
        s_led_tgt = LedManager::Target::STRIP_1;
        ESP_LOGI(TAG, "LED target: Strip 1");
    }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 2", []() {
        s_led_tgt = LedManager::Target::STRIP_2;
        ESP_LOGI(TAG, "LED target: Strip 2");
    }));

    // ── System ────────────────────────────────────────────────────────────────
    auto sys = std::make_unique<MenuItem>("System");

    sys->addChild(std::make_unique<MenuItem>("Uptime", [this, &cm, &net]() {
        show_info_screen(cm, net);
    }));

    // ── Assemble ──────────────────────────────────────────────────────────────
    root->addChild(std::move(clk));
    root->addChild(std::move(stat));
    root->addChild(std::move(netm));
    root->addChild(std::move(lights));
    root->addChild(std::move(sys));

    root_menu_         = std::move(root);
    current_menu_      = root_menu_.get();
    current_selection_ = 0;
    display_start_     = 0;

    ESP_LOGI(TAG, "Menu built with %zu top-level items",
             current_menu_->getChildren().size());
}
