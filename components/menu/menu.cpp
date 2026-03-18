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

static const char *TAG = "Menu";

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string center_str(const std::string &s, int width = 16)
{
    int len = static_cast<int>(s.size());
    if (len >= width)
        return s.substr(0, width);
    int left = (width - len) / 2;
    return std::string(left, ' ') + s;
}

// ── MenuItem ─────────────────────────────────────────────────────────────────

MenuItem::MenuItem(const std::string &name)
    : name_(name), parent_(nullptr), callback_(nullptr) {}

MenuItem::MenuItem(const std::string &name, Callback callback)
    : name_(name), parent_(nullptr), callback_(callback) {}

void MenuItem::addChild(std::unique_ptr<MenuItem> child)
{
    child->parent_ = this;
    children_.push_back(std::move(child));
}

void MenuItem::execute() const
{
    if (callback_)
    {
        callback_();
    }
    else
    {
        ESP_LOGI(TAG, "No callback for: %s", name_.c_str());
    }
}

// ── Menu core ─────────────────────────────────────────────────────────────────

Menu::Menu(Display &display)
    : display_(display)
{
    action_sem_ = xSemaphoreCreateBinary();
    xSemaphoreGive(action_sem_); // start available
    action_queue_ = xQueueCreate(1, sizeof(MenuItem::Callback *));
    xTaskCreate(action_task_fn, "menu_act", 4096, this, 3, &action_task_handle_);
}

void Menu::next()
{
    if (!current_menu_)
        return;
    if (current_selection_ < current_menu_->getChildren().size() - 1)
    {
        current_selection_++;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
}

void Menu::previous()
{
    if (!current_menu_)
        return;
    if (current_selection_ > 0)
    {
        current_selection_--;
        updateDisplayStart();
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
}

void Menu::select()
{
    if (!current_menu_)
        return;
    const auto &children = current_menu_->getChildren();
    if (children.empty())
        return;
    auto *selected = children[current_selection_].get();
    if (selected->hasChildren())
    {
        current_menu_ = selected;
        current_selection_ = 0;
        display_start_ = 0;
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
        ESP_LOGI(TAG, "Entering: %s", selected->getName().c_str());
    }
    else
    {
        ESP_LOGI(TAG, "Execute: %s", selected->getName().c_str());
        selected->execute();
    }
}

void Menu::back()
{
    if (!current_menu_ || !current_menu_->getParent())
        return;
    current_menu_ = current_menu_->getParent();
    current_selection_ = 0;
    display_start_ = 0;
    h_scroll_offset_ = 0;
    h_scroll_dir_ = 1;
}

void Menu::updateDisplayStart()
{
    if (current_selection_ < display_start_)
    {
        display_start_ = current_selection_;
    }
    else if (current_selection_ >= display_start_ + MAX_VISIBLE_ITEMS)
    {
        display_start_ = current_selection_ - MAX_VISIBLE_ITEMS + 1;
    }
}

std::vector<std::string> Menu::getVisibleItems() const
{
    std::vector<std::string> items;
    if (!current_menu_)
        return items;
    const auto &children = current_menu_->getChildren();
    size_t start = display_start_;
    size_t end = std::min(start + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = start; i < end; i++)
        items.push_back(children[i]->getName());
    while (items.size() < MAX_VISIBLE_ITEMS)
        items.emplace_back("");
    return items;
}

void Menu::render()
{
    if (blanked_ || !current_menu_)
        return;

    const auto &children = current_menu_->getChildren();
    std::vector<std::string> lines;
    lines.reserve(1 + MAX_VISIBLE_ITEMS);

    // Line 0: centred title
    lines.push_back(center_str(current_menu_->getName()));

    // Lines 1..MAX_VISIBLE_ITEMS: items (with h-scroll on selected item)
    size_t end = std::min(display_start_ + MAX_VISIBLE_ITEMS, children.size());
    for (size_t i = display_start_; i < end; i++)
    {
        const std::string &name = children[i]->getName();
        if (i == current_selection_ && (int)name.length() > 16)
        {
            int max_off = (int)name.length() - 16;
            int off = std::min(h_scroll_offset_, max_off);
            lines.push_back(name.substr(off, 16));
        }
        else
        {
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

void Menu::render_scrolled(bool going_next)
{
    if (blanked_)
        return;
    display_.startHardwareScroll(
        going_next ? ScrollMode::HARDWARE_UP : ScrollMode::HARDWARE_DOWN, 7);
    vTaskDelay(pdMS_TO_TICKS(60));
    render();                      // updates buffer; refresh_display skipped while scrolling
    display_.stopHardwareScroll(); // stops scroll and flushes the new buffer
}

void Menu::tick_h_scroll()
{
    if (blanked_ || !current_menu_)
        return;
    const auto &children = current_menu_->getChildren();
    if (children.empty())
        return;
    const std::string &name = children[current_selection_]->getName();
    int name_len = (int)name.length();
    if (name_len <= 16)
        return;

    int max_off = name_len - 16;
    h_scroll_offset_ += h_scroll_dir_;
    if (h_scroll_offset_ >= max_off)
    {
        h_scroll_offset_ = max_off;
        h_scroll_dir_ = -1;
    }
    else if (h_scroll_offset_ <= 0)
    {
        h_scroll_offset_ = 0;
        h_scroll_dir_ = 1;
    }
    render();
}

// ── Display blanking ──────────────────────────────────────────────────────────

void Menu::wake()
{
    idle_seconds_ = 0;
    if (blanked_)
    {
        blanked_ = false;
        display_.clear();
        render();
        ESP_LOGI(TAG, "Display woken");
    }
}

void Menu::tick_blank_timer()
{
    if (blanked_)
        return;
    if (++idle_seconds_ >= BLANK_TIMEOUT_S)
    {
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
static constexpr uint32_t DISMISS_HOLD_MS = 800;

void Menu::wait_for_dismiss()
{
    // Debounce: wait for button released from the select/navigate press
    vTaskDelay(pdMS_TO_TICKS(300));

    uint32_t hold_ms = 0;
    uint32_t elapsed = 0;

    while (elapsed < DISMISS_TIMEOUT_MS)
    {
        bool pressed = dismiss_fn_ ? dismiss_fn_() : false;
        if (pressed)
        {
            hold_ms += 50;
            if (hold_ms >= DISMISS_HOLD_MS)
                return; // long press detected
        }
        else
        {
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

void Menu::show_clock_status(ClockManager &cm)
{
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

void Menu::show_net_status(Networking &net)
{
    const NetStatus &s = net.get_status();
    char buf[20];
    display_.clear();
    display_.print(0, center_str("Net Status").c_str());
    if (s.wifi_connected)
    {
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
    }
    else
    {
        display_.print(1, "No connection");
    }
    wait_for_dismiss();
    render();
}

void Menu::show_time_screen(ClockManager &cm)
{
    display_.clear();
    display_.print(0, center_str("Time & Sync").c_str());
    display_.print(1, cm.time_hms().c_str());
    display_.print(2, cm.date_short().c_str());
    display_.print(3, cm.is_time_valid() ? "SNTP: synced" : "SNTP: no sync");
    wait_for_dismiss();
    render();
}

void Menu::show_info_screen(ClockManager &cm, Networking &net)
{
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
//
// Character grid — all characters visible at once as a 6×16 grid.
//
// Layout (display rows 2-7, with row 0=title, row 1=entered text):
//   Row 0:  a b c d e f g h i j k l m n o p
//   Row 1:  q r s t u v w x y z   A B C D E   (space at col 10)
//   Row 2:  F G H I J K L M N O P Q R S T U
//   Row 3:  V W X Y Z 0 1 2 3 4 5 6 7 8 9
//   Row 4:  ! @ # $ % ^ & * ( ) - _ = + [ ]
//   Row 5:  { } | ; : ' " , . < > / ? ~ `  >  (last slot = OK/confirm)
//
// The cursor is a SINGLE INVERTED CHARACTER — no brackets.
//
// Controls (button-only, encoder-only, or both):
//   Tap A (release < 800ms)  → navigate backward one step
//   Hold A to 800ms          → append current character (no movement during hold)
//   Tap B (release < 800ms)  → navigate forward one step
//   Hold B to 800ms          → backspace (no movement during hold)
//   A+B brief (release both) → confirm and return
//   A+B hold 800ms           → cancel (return empty string)
//   Encoder rotate           → navigate (fast; same flat cursor)
//   Encoder short press      → append character (or confirm if cursor on '>')
//   Encoder long press       → backspace

static const char GRID_CHARS[] =
    "abcdefghijklmnop"    // row 0
    "qrstuvwxyz ABCDE"    // row 1  (space at col 10)
    "FGHIJKLMNOPQRSTU"    // row 2
    "VWXYZ0123456789 "    // row 3  (trailing space = padding)
    "!@#$%^&*()-_=+[]"    // row 4
    "{}|;:'\",.<>/?~` >"; // row 5  (last slot '>' = confirm sentinel)

static constexpr int GRID_ROWS    = 6;
static constexpr int GRID_COLS    = 16;
static constexpr int GRID_SIZE    = GRID_ROWS * GRID_COLS;  // 96
static constexpr int GRID_OK_POS  = GRID_SIZE - 1;          // '>' = confirm

std::string Menu::show_text_input(const std::string &title, bool mask, size_t max_len)
{
    if (!input_poll_fn_)
        return {};

    std::string result;
    int cursor_pos = 0;   // flat index 0..GRID_SIZE-1; starts on 'a'

    bool enc_btn_last = false;
    bool btnA_last    = false;
    bool btnB_last    = false;
    bool both_last    = false;

    uint32_t enc_hold_ms    = 0;
    bool     enc_longfire   = false;
    uint32_t btnA_hold_ms   = 0;
    bool     btnA_longfire  = false;
    int      btnA_streak    = 0;   // consecutive rapid taps
    uint32_t btnA_inter_ms  = 999; // ms since last A tap fired (start high = no streak)
    uint32_t btnB_hold_ms   = 0;
    bool     btnB_longfire  = false;
    int      btnB_streak    = 0;
    uint32_t btnB_inter_ms  = 999;

    static constexpr uint32_t LONG_PRESS_MS  = 800;
    static constexpr uint32_t TAP_WINDOW_MS  = 400; // max gap between taps to extend streak
    static constexpr int      MAX_STREAK     = 8;   // caps steps per tap

    auto get_row = [&]() { return cursor_pos / GRID_COLS; };
    auto get_col = [&]() { return cursor_pos % GRID_COLS; };

    auto cur_char = [&]() -> char {
        char c = GRID_CHARS[cursor_pos];
        return c ? c : ' ';
    };

    auto render_grid = [&]()
    {
        int crow = get_row();
        int ccol = get_col();

        std::vector<std::string> lines;

        // Row 0: title
        std::string t = title;
        if ((int)t.size() > 16) t.resize(16);
        lines.push_back(t);

        // Row 1: entered text + trailing cursor marker
        std::string vis = mask ? std::string(result.size(), '*') : result;
        vis += '_';
        if ((int)vis.size() > 16) vis = vis.substr(vis.size() - 16);
        while ((int)vis.size() < 16) vis += ' ';
        lines.push_back(vis);

        // Rows 2-7: character grid — all chars shown normally (no brackets)
        for (int r = 0; r < GRID_ROWS; r++) {
            int base = r * GRID_COLS;
            std::string s;
            for (int c = 0; c < GRID_COLS; c++) {
                char ch = GRID_CHARS[base + c];
                s += ch ? ch : ' ';
            }
            lines.push_back(s);
        }

        // No whole-row highlight; cursor is a single inverted character
        display_.writeLines(lines, -1);

        // Overlay the cursor character inverted at its exact pixel position
        // (page = crow+2 because rows 0 and 1 are title and text field)
        char dc = cur_char();
        display_.render_char_inverted(crow + 2, ccol, dc);
    };

    // Brief control hint
    {
        std::vector<std::string> hint;
        std::string t = title;
        if ((int)t.size() > 16) t.resize(16);
        hint.push_back(t);
        hint.push_back("");
        hint.push_back("A/B: navigate");
        hint.push_back("Hold A: add char");
        hint.push_back("Hold B: delete");
        hint.push_back("Nav to > confirm");
        hint.push_back("Hold both: cancel");
        hint.push_back("Enc press: add");
        display_.writeLines(hint, -1);
        vTaskDelay(pdMS_TO_TICKS(1800));
    }

    // Snapshot current button state so stale holds don't trigger false edges
    {
        InputEvent init = input_poll_fn_();
        enc_btn_last = init.enc_btn;
        btnA_last    = init.btnA;
        btnB_last    = init.btnB;
        both_last    = init.btnA && init.btnB;
    }

    render_grid();

    uint32_t both_hold_ms  = 0;
    bool     both_longfire = false;

    while (true)
    {
        InputEvent ev = input_poll_fn_();
        bool enc_btn = ev.enc_btn;
        bool btnA    = ev.btnA;
        bool btnB    = ev.btnB;
        bool both    = btnA && btnB;

        bool enc_rise = !enc_btn && enc_btn_last;
        bool redraw   = false;

        // ── A+B chord: hold LONG_PRESS_MS = cancel ────────────────────────────
        // Brief A+B release no longer confirms (too easy to trigger accidentally).
        // Confirm by navigating to '>' and tapping A or B, same as encoder.
        if (both) {
            both_hold_ms += 50;
            if (both_hold_ms >= LONG_PRESS_MS && !both_longfire) {
                both_longfire = true;
                result.clear();   // cancel — return empty string
                break;
            }
            // While both held, reset individual button timers
            btnA_hold_ms  = 0;
            btnA_longfire = false;
            btnA_streak   = 0;
            btnB_hold_ms  = 0;
            btnB_longfire = false;
            btnB_streak   = 0;
        } else {
            both_hold_ms  = 0;
            both_longfire = false;
        }

        // ── Encoder rotation → navigate all chars linearly ────────────────────
        if (!both && ev.delta != 0) {
            cursor_pos = ((cursor_pos + ev.delta) % GRID_SIZE + GRID_SIZE) % GRID_SIZE;
            redraw = true;
        }

        // ── Button A: tap = step back (accelerating); hold = append ─────────────
        // Rapid taps build a streak: each tap moves (streak) steps, up to MAX_STREAK.
        // A pause longer than TAP_WINDOW_MS resets the streak to 1.
        if (!btnA) btnA_inter_ms += 50;
        if (btnA_inter_ms >= TAP_WINDOW_MS) btnA_streak = 0;

        if (btnA && !both) {
            if (!btnA_last) {
                btnA_hold_ms  = 0;
                btnA_longfire = false;
            } else if (!btnA_longfire) {
                btnA_hold_ms += 50;
                if (btnA_hold_ms >= LONG_PRESS_MS) {
                    btnA_longfire = true;
                    btnA_streak   = 0;  // hold action breaks streak
                    if (cursor_pos != GRID_OK_POS && result.size() < max_len) {
                        result += cur_char();
                        redraw = true;
                    }
                }
            }
        } else {
            if (btnA_last && !btnA_longfire && !both_last) {
                // Tap: confirm if on '>', else navigate backward with acceleration
                if (cursor_pos == GRID_OK_POS) {
                    break;
                }
                if (btnA_inter_ms < TAP_WINDOW_MS) {
                    btnA_streak = (btnA_streak < MAX_STREAK) ? btnA_streak + 1 : MAX_STREAK;
                } else {
                    btnA_streak = 1;
                }
                btnA_inter_ms = 0;
                cursor_pos = ((cursor_pos - btnA_streak) % GRID_SIZE + GRID_SIZE) % GRID_SIZE;
                redraw = true;
            }
            btnA_hold_ms  = 0;
            btnA_longfire = false;
        }

        // ── Button B: tap = step fwd (accelerating); hold = backspace ───────────
        if (!btnB) btnB_inter_ms += 50;
        if (btnB_inter_ms >= TAP_WINDOW_MS) btnB_streak = 0;

        if (btnB && !both) {
            if (!btnB_last) {
                btnB_hold_ms  = 0;
                btnB_longfire = false;
            } else if (!btnB_longfire) {
                btnB_hold_ms += 50;
                if (btnB_hold_ms >= LONG_PRESS_MS) {
                    btnB_longfire = true;
                    btnB_streak   = 0;  // hold action breaks streak
                    if (!result.empty()) {
                        result.pop_back();
                        redraw = true;
                    }
                }
            }
        } else {
            if (btnB_last && !btnB_longfire && !both_last) {
                // Tap: confirm if on '>', else navigate forward with acceleration
                if (cursor_pos == GRID_OK_POS) {
                    break;
                }
                if (btnB_inter_ms < TAP_WINDOW_MS) {
                    btnB_streak = (btnB_streak < MAX_STREAK) ? btnB_streak + 1 : MAX_STREAK;
                } else {
                    btnB_streak = 1;
                }
                btnB_inter_ms = 0;
                cursor_pos = (cursor_pos + btnB_streak) % GRID_SIZE;
                redraw = true;
            }
            btnB_hold_ms  = 0;
            btnB_longfire = false;
        }

        // ── Encoder: long press = backspace; short press = append / confirm ───
        if (enc_btn) {
            if (!enc_longfire) {
                enc_hold_ms += 50;
                if (enc_hold_ms >= LONG_PRESS_MS) {
                    enc_longfire = true;
                    if (!result.empty()) {
                        result.pop_back();
                        redraw = true;
                    }
                }
            }
        } else {
            if (enc_rise && !enc_longfire) {
                if (cursor_pos == GRID_OK_POS) {
                    break;  // encoder press on '>' = confirm (encoder-only)
                } else if (result.size() < max_len) {
                    result += cur_char();
                    redraw = true;
                }
            }
            enc_hold_ms  = 0;
            enc_longfire = false;
        }

        if (redraw) render_grid();

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
                                   const std::string &code)
{
    matter_pin_ = pin;
    matter_disc_ = disc;
    matter_code_ = code;
}

// ── show_matter_pairing_screen ────────────────────────────────────────────────
// Layout (8 lines on 128×64 SSD1306):
//   0  " Matter Pairing " (centered)
//   1  "Go to Smart Home"
//   2  (blank)
//   3  (blank)
//   4  "Disc: XXXX"
//   5  "Code:XXXXXXXXXXX"
//   6  (blank)
//   7  encoder hint -or- button hint
//
// Returns true  = commissioned (auto-exit; proceed to main menu).
//         false = back pressed  (encoder short press or single A/B tap).
static bool show_matter_pairing_screen(Display &display,
                                       uint32_t /*pin*/, uint16_t disc,
                                       const std::string &code,
                                       Menu::InputPollFn poll,
                                       bool encoder_ok,
                                       std::function<bool()> commissioned_fn)
{
    char buf[20];
    display.clear();
    display.print(0, " Matter Pairing ");
    display.print(1, "Go to Smart Home");
    // lines 2 & 3 intentionally blank
    snprintf(buf, sizeof(buf), "Disc: %u", (unsigned)disc);
    display.print(4, buf);
    snprintf(buf, sizeof(buf), "Code:%.11s", code.empty() ? "see UART   " : code.c_str());
    display.print(5, buf);
    // line 6 intentionally blank
    display.print(7, encoder_ok ? "ShrtPress: back" : "A or B: back");

    if (!poll)
        return true;

    vTaskDelay(pdMS_TO_TICKS(500)); // debounce after selection

    // Snapshot: only auto-exit if commissioning happens DURING this screen.
    // If the device already has a fabric when the screen appears (stale NVS
    // data from a prior flash), the transition never fires and the user must
    // press back manually — preventing an immediate skip to main menu.
    bool was_commissioned = commissioned_fn && commissioned_fn();

    bool     enc_last = false;
    uint32_t enc_hold = 0;

    while (true)
    {
        // Auto-exit only on false → true transition
        if (commissioned_fn && !was_commissioned && commissioned_fn())
            return true;

        auto ev   = poll();
        bool both = ev.btnA && ev.btnB;

        // Encoder short press = back
        if (ev.enc_btn) {
            enc_hold += 100;
        } else {
            if (enc_last && enc_hold < 800) {
                vTaskDelay(pdMS_TO_TICKS(200));
                return false;
            }
            enc_hold = 0;
        }
        enc_last = ev.enc_btn;

        // Single A or B (not both) = back
        if (!both && (ev.btnA || ev.btnB)) {
            vTaskDelay(pdMS_TO_TICKS(200)); // wait for release
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── first_time_setup ──────────────────────────────────────────────────────────

Menu::SetupResult Menu::first_time_setup()
{
    if (!input_poll_fn_)
        return SetupResult::Pending;

    // ── 2-item choice screen ──────────────────────────────────────────────────
    int choice = 0; // 0 = Matter, 1 = Setup WiFi
    bool enc_btn_last = false;
    bool btnA_last = false;
    bool btnB_last = false;
    bool both_last = false;
    uint32_t enc_hold = 0;
    bool enc_longfire = false;
    uint32_t btnA_hold = 0;
    bool btnA_longfire = false;

    auto render_choice = [&]()
    {
        display_.clear();
        display_.print(0, "  First-Time Setup");
        display_.print(2, choice == 0 ? "> Matter" : "  Matter");
        display_.print(3, choice == 1 ? "> Setup WiFi" : "  Setup WiFi");
        display_.print(5, "A/B/Rot: pick");
        display_.print(6, "LongA/Enc: ok");
        display_.print(7, "Enc 5s:WiFiRst");
    };
    render_choice();

    bool selected = false;
    while (!selected)
    {
        InputEvent ev = input_poll_fn_();
        bool enc_btn = ev.enc_btn;
        bool btnA = ev.btnA;
        bool btnB = ev.btnB;
        bool both = btnA && btnB;

        bool enc_rise = !enc_btn && enc_btn_last;
        bool btnA_rise = !btnA && btnA_last && !both_last;
        bool btnB_rise = !btnB && btnB_last && !both_last;

        // Rotation or short A/B → toggle selection
        if (ev.delta != 0)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }
        if (btnA_rise && !btnA_longfire)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }
        if (btnB_rise)
        {
            choice = (choice + 1) % 2;
            render_choice();
        }

        // Encoder short press → confirm
        if (enc_rise && !enc_longfire)
            selected = true;

        // Encoder long press → confirm
        if (enc_btn)
        {
            if (!enc_longfire && (enc_hold += 50) >= 800)
            {
                enc_longfire = true;
                selected = true;
            }
        }
        else
        {
            enc_hold = 0;
            enc_longfire = false;
        }

        // Long press A → confirm (B long press is not a select action)
        if (btnA && !both)
        {
            if (!btnA_longfire && (btnA_hold += 50) >= 800)
            {
                btnA_longfire = true;
                selected = true;
            }
        }
        else
        {
            btnA_hold = 0;
            btnA_longfire = false;
        }


        enc_btn_last = enc_btn;
        btnA_last = btnA;
        btnB_last = btnB;
        both_last = both;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (choice == 0)
    {
        // Return to app_main so it can start the Matter stack at shallow stack
        // depth before showing the pairing screen.
        return SetupResult::MatterChosen;
    }

    // ── WiFi path: SSID + password text entry ─────────────────────────────────
    // Loops back to the choice screen if the user cancels (hold-both).
    while (true)
    {
        std::string ssid = show_text_input("SSID:", false, 63);
        if (ssid.empty())
        {
            // Cancelled — restart the whole first-time setup choice screen
            return first_time_setup();
        }

        std::string pass = show_text_input("Password:", false, 63);
        // Empty pass is valid (open network); proceed regardless.

        NetCfg cfg = {};
        ConfigStore::load(cfg);
        strncpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid) - 1);
        strncpy(cfg.password, pass.c_str(), sizeof(cfg.password) - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
        cfg.password[sizeof(cfg.password) - 1] = '\0';
        ConfigStore::save(cfg);

        display_.clear();
        display_.print(0, "  WiFi Saved!");
        display_.print(2, ssid.c_str());
        display_.print(4, "Connecting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return SetupResult::WiFiSaved;
    }
}

// ── show_matter_pairing_standalone ────────────────────────────────────────────
// Called from app_main AFTER s_matter.start() so BLE is advertising when the
// user sees the pairing screen.

bool Menu::show_matter_pairing_standalone(std::function<bool()> commissioned_fn)
{
    return show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                      matter_code_, input_poll_fn_,
                                      encoder_ok_, commissioned_fn);
}

// ── Async action dispatch ─────────────────────────────────────────────────────
// Slow motor callbacks (Set Time, Advance) are posted here so they run on
// action_task and encoder_task stays responsive during motor movement.
// The semaphore ensures at most one action is queued or running at a time;
// extra presses are silently discarded.

void Menu::post_action(MenuItem::Callback cb)
{
    if (xSemaphoreTake(action_sem_, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Action already running — request discarded");
        return;
    }
    auto *cb_ptr = new MenuItem::Callback(std::move(cb));
    if (xQueueSend(action_queue_, &cb_ptr, 0) != pdTRUE)
    {
        delete cb_ptr;
        xSemaphoreGive(action_sem_);
    }
}

void Menu::action_task_fn(void *arg)
{
    Menu *self = static_cast<Menu *>(arg);
    MenuItem::Callback *cb_ptr = nullptr;
    while (true)
    {
        if (xQueueReceive(self->action_queue_, &cb_ptr, portMAX_DELAY) == pdTRUE)
        {
            if (cb_ptr)
            {
                (*cb_ptr)();
                delete cb_ptr;
                cb_ptr = nullptr;
            }
            xSemaphoreGive(self->action_sem_);
        }
    }
}

// ── build() — full menu tree with wired callbacks ────────────────────────────

void Menu::build(ClockManager &cm, Networking &net, LedManager &leds)
{
    leds_ = &leds;

    auto root = std::make_unique<MenuItem>("Main");

    // ── Clock ─────────────────────────────────────────────────────────────────
    auto clk = std::make_unique<MenuItem>("Clock");

    clk->addChild(std::make_unique<MenuItem>("Set Time", [this, &cm]()
                                             { post_action([&cm]()
                                                           { cm.cmd_set_time(-1, -1); }); }));

    clk->addChild(std::make_unique<MenuItem>("Advance 1Min", [this, &cm]()
                                             { post_action([&cm]()
                                                           { cm.cmd_test_advance(); }); }));

    clk->addChild(std::make_unique<MenuItem>("Step Fwd", [&cm]()
                                             { cm.cmd_microstep(8, true); }));

    clk->addChild(std::make_unique<MenuItem>("Step Bwd", [&cm]()
                                             { cm.cmd_microstep(8, false); }));

    clk->addChild(std::make_unique<MenuItem>("Cal Sensor", [&cm]()
                                             { cm.cmd_calibrate_sensor(); }));

    clk->addChild(std::make_unique<MenuItem>("Sensor Meas", [&cm]()
                                             { cm.cmd_measure_sensor_average(); }));

    // ── Status ────────────────────────────────────────────────────────────────
    auto stat = std::make_unique<MenuItem>("Status");

    stat->addChild(std::make_unique<MenuItem>("Clock", [this, &cm]()
                                              { show_clock_status(cm); }));

    stat->addChild(std::make_unique<MenuItem>("Network", [this, &net]()
                                              { show_net_status(net); }));

    stat->addChild(std::make_unique<MenuItem>("Time/Sync", [this, &cm]()
                                              { show_time_screen(cm); }));

    stat->addChild(std::make_unique<MenuItem>("About", [this, &cm, &net]()
                                              { show_info_screen(cm, net); }));

    // ── Network ───────────────────────────────────────────────────────────────
    auto netm = std::make_unique<MenuItem>("Network");

    netm->addChild(std::make_unique<MenuItem>("Net Info", [this, &net]()
                                              { show_net_status(net); }));

    netm->addChild(std::make_unique<MenuItem>("Sync Status", [this, &cm]()
                                              { show_time_screen(cm); }));

    netm->addChild(std::make_unique<MenuItem>("Set WiFi", [this]()
                                              {
        // Enter SSID
        std::string ssid = show_text_input("SSID:", false, 63);
        if (ssid.empty()) { render(); return; }

        // Enter password
        std::string pass = show_text_input("Password:", false, 63);

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
        render(); }));

    netm->addChild(std::make_unique<MenuItem>("Matter Pair", [this]()
                                              {
        show_matter_pairing_screen(display_, matter_pin_, matter_disc_,
                                   matter_code_, input_poll_fn_,
                                   encoder_ok_, nullptr);
        render(); }));

    // ── Lights ────────────────────────────────────────────────────────────────
    // s_led_tgt persists across menu interactions (static local).
    static LedManager::Target s_led_tgt = LedManager::Target::BOTH;

    auto lights = std::make_unique<MenuItem>("Lights");

    lights->addChild(std::make_unique<MenuItem>("Next Effect", [&leds]()
                                                { leds.next_effect(s_led_tgt); }));

    lights->addChild(std::make_unique<MenuItem>("Bright +", [&leds]()
                                                {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b > 230 ? 255 : b + 25);
        } }));

    lights->addChild(std::make_unique<MenuItem>("Bright -", [&leds]()
                                                {
        for (int i = 0; i < LedManager::STRIP_COUNT; i++) {
            uint8_t b = leds.get_brightness(i);
            leds.set_brightness(s_led_tgt, b < 25 ? 0 : b - 25);
        } }));

    lights->addChild(std::make_unique<MenuItem>("White", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 255, 255); }));

    lights->addChild(std::make_unique<MenuItem>("Warm White", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 180, 80); }));

    lights->addChild(std::make_unique<MenuItem>("Red", [&leds]()
                                                { leds.set_color(s_led_tgt, 255, 0, 0); }));

    lights->addChild(std::make_unique<MenuItem>("Blue", [&leds]()
                                                { leds.set_color(s_led_tgt, 0, 80, 255); }));

    lights->addChild(std::make_unique<MenuItem>("Green", [&leds]()
                                                { leds.set_color(s_led_tgt, 0, 220, 50); }));

    lights->addChild(std::make_unique<MenuItem>("Purple", [&leds]()
                                                { leds.set_color(s_led_tgt, 180, 0, 255); }));

    lights->addChild(std::make_unique<MenuItem>("-> Both", []()
                                                {
        s_led_tgt = LedManager::Target::BOTH;
        ESP_LOGI(TAG, "LED target: Both"); }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 1", []()
                                                {
        s_led_tgt = LedManager::Target::STRIP_1;
        ESP_LOGI(TAG, "LED target: Strip 1"); }));

    lights->addChild(std::make_unique<MenuItem>("-> Strip 2", []()
                                                {
        s_led_tgt = LedManager::Target::STRIP_2;
        ESP_LOGI(TAG, "LED target: Strip 2"); }));

    // ── System ────────────────────────────────────────────────────────────────
    auto sys = std::make_unique<MenuItem>("System");

    sys->addChild(std::make_unique<MenuItem>("Uptime", [this, &cm, &net]()
                                             { show_info_screen(cm, net); }));

    // ── Assemble ──────────────────────────────────────────────────────────────
    root->addChild(std::move(clk));
    root->addChild(std::move(stat));
    root->addChild(std::move(netm));
    root->addChild(std::move(lights));
    root->addChild(std::move(sys));

    root_menu_ = std::move(root);
    current_menu_ = root_menu_.get();
    current_selection_ = 0;
    display_start_ = 0;

    ESP_LOGI(TAG, "Menu built with %zu top-level items",
             current_menu_->getChildren().size());
}
