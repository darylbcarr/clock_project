#include "display.h"
#include "esp_log.h"
#include "font_latin_8x8.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DISPLAY";

Display::Display() : m_dev(nullptr), m_bus_handle(nullptr),
                     m_bus_mutex(nullptr),
                     m_initialized(false), m_selected_line(-1),
                     m_display_offset(0), m_batch_mode(false),
                     m_dirty(false), m_hardware_scrolling(false) {
    memset(m_lines, 0, sizeof(m_lines));
    m_bus_mutex = xSemaphoreCreateMutex();
}

Display::~Display() {
    if (m_initialized && m_dev != nullptr) {
        if (m_hardware_scrolling) stopHardwareScroll();
        ssd1306_delete(m_dev);
    }
    if (m_bus_mutex) {
        vSemaphoreDelete(m_bus_mutex);
        m_bus_mutex = nullptr;
    }
}

esp_err_t Display::init_i2c_bus(int sda_pin, int scl_pin) {
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = (gpio_num_t)sda_pin,
        .scl_io_num        = (gpio_num_t)scl_pin,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd               = false,
        }
    };
    return i2c_new_master_bus(&i2c_bus_config, &m_bus_handle);
}

bool Display::init(i2c_port_t port, uint8_t addr, int sda_pin, int scl_pin) {
    ESP_LOGI(TAG, "Initializing SSD1306 display");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = init_i2c_bus(sda_pin, scl_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ssd1306_config_t ssd1306_config = {
        .i2c_address   = addr,
        .i2c_clock_speed = 400000,
        .panel_size    = SSD1306_PANEL_128x64,
        .offset_x      = 0,
        .flip_enabled  = false,
        .display_enabled = true
    };

    // Retry SSD1306 init: the display may need a moment after power-on to
    // respond.  Three attempts with 300 ms gaps covers most cold-start races.
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "SSD1306 init retry %d/3...", attempt);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        ret = ssd1306_init(m_bus_handle, &ssd1306_config, &m_dev);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SSD1306 init attempt %d failed: %s", attempt, esp_err_to_name(ret));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed after 3 attempts: %s", esp_err_to_name(ret));
        return false;
    }

    m_initialized = true;
    ESP_LOGI(TAG, "Clearing display");
    ssd1306_clear_display(m_dev, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ssd1306_display_pages(m_dev);
    ssd1306_enable_display(m_dev);
    ESP_LOGI(TAG, "Display initialized successfully");
    return true;
}

void Display::clear() {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    ssd1306_clear_display(m_dev, false);
    ssd1306_display_pages(m_dev);
    xSemaphoreGive(m_bus_mutex);
    memset(m_lines, 0, sizeof(m_lines));
}

void Display::update() {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) {
        ESP_LOGW(TAG, "Cannot update while hardware scrolling is active");
        return;
    }
    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    ssd1306_display_pages(m_dev);
    xSemaphoreGive(m_bus_mutex);
}

void Display::beginBatch() {
    m_batch_mode = true;
    m_dirty = false;
}

void Display::endBatch() {
    m_batch_mode = false;
    if (m_dirty && !m_hardware_scrolling) {
        refresh_display();
        m_dirty = false;
    }
}

void Display::refresh_display() {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) return;

    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);

    ssd1306_clear_display(m_dev, false);

    for (int visible_page = 0; visible_page < VISIBLE_LINES; visible_page++) {
        int line_num = m_display_offset + visible_page;
        if (line_num < MAX_LINES && m_lines[line_num][0] != '\0') {
            bool invert = (line_num == m_selected_line);
            esp_err_t ret = ssd1306_display_text(m_dev, visible_page,
                                                 m_lines[line_num], invert);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ssd1306_display_text page=%d: %s",
                         visible_page, esp_err_to_name(ret));
            }
        }
    }

    ssd1306_display_pages(m_dev);

    xSemaphoreGive(m_bus_mutex);
}

void Display::renderLine(int line_num, int y_pos, bool is_selected) {
    if (line_num < 0 || line_num >= MAX_LINES ||
        !m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) return;

    int page = y_pos / 8;
    if (page < 0 || page >= 8) return;

    ssd1306_display_filled_rectangle(m_dev, 0, y_pos,
                                     SCREEN_WIDTH - 1, y_pos + CHAR_HEIGHT - 1,
                                     false);
    if (is_selected) {
        ssd1306_display_filled_rectangle(m_dev, 0, y_pos,
                                         SCREEN_WIDTH - 1, y_pos + CHAR_HEIGHT - 1,
                                         true);
        if (m_lines[line_num][0] != '\0')
            ssd1306_display_text(m_dev, page, m_lines[line_num], true);
    } else {
        if (m_lines[line_num][0] != '\0')
            ssd1306_display_text(m_dev, page, m_lines[line_num], false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeLines — interface for Menu::render()
//
// Accepts a vector of strings (one per visible menu row) and a highlight index
// (0-based within that vector, or -1 for none).  Replaces the display buffer
// from line 0 and refreshes in a single batch.
// ─────────────────────────────────────────────────────────────────────────────
void Display::writeLines(const std::vector<std::string>& lines, int highlight_line) {
    if (!m_initialized || m_dev == nullptr) return;

    beginBatch();

    // Reset scroll position — menu always renders from the top of its window
    m_display_offset = 0;

    // Clear all lines that will be visible
    for (int i = 0; i < VISIBLE_LINES && i < MAX_LINES; i++) {
        m_lines[i][0] = '\0';
    }

    // Copy incoming strings into the line buffer
    for (size_t i = 0; i < lines.size() && (int)i < MAX_LINES; i++) {
        strncpy(m_lines[i], lines[i].c_str(), MAX_LINE_LENGTH - 1);
        m_lines[i][MAX_LINE_LENGTH - 1] = '\0';
        if (strlen(m_lines[i]) > 16) m_lines[i][16] = '\0';
    }

    // highlight_line is relative to the visible window (i.e. 0-based line index)
    m_selected_line = (highlight_line >= 0) ? highlight_line : -1;
    m_dirty = true;

    endBatch();
}

void Display::print(int line, const char* text) {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (line < 0 || line >= MAX_LINES) return;

    char truncated[MAX_LINE_LENGTH];
    strncpy(truncated, text, MAX_LINE_LENGTH - 1);
    truncated[MAX_LINE_LENGTH - 1] = '\0';
    if (strlen(truncated) > 16) truncated[16] = '\0';

    strncpy(m_lines[line], truncated, MAX_LINE_LENGTH - 1);
    m_lines[line][MAX_LINE_LENGTH - 1] = '\0';
    m_dirty = true;

    if (!m_batch_mode) refresh_display();
}

void Display::clearLine(int line) {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (line < 0 || line >= MAX_LINES) return;
    m_lines[line][0] = '\0';
    m_dirty = true;
    if (!m_batch_mode) refresh_display();
}

void Display::setSelectedLine(int line) {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (line < -1 || line >= MAX_LINES) return;

    m_selected_line = line;

    if (m_selected_line >= 0) {
        if (m_selected_line < m_display_offset)
            m_display_offset = m_selected_line;
        else if (m_selected_line >= m_display_offset + VISIBLE_LINES)
            m_display_offset = m_selected_line - VISIBLE_LINES + 1;

        if (m_display_offset < 0) m_display_offset = 0;
        if (m_display_offset > MAX_LINES - VISIBLE_LINES)
            m_display_offset = MAX_LINES - VISIBLE_LINES;
    }
    refresh_display();
}

void Display::clearSelectedLine() { setSelectedLine(-1); }

void Display::scrollUp() {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (m_display_offset > 0) { m_display_offset--; refresh_display(); }
}

void Display::scrollDown() {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (m_display_offset < MAX_LINES - VISIBLE_LINES) {
        m_display_offset++;
        refresh_display();
    }
}

void Display::setScrollPosition(int top_line) {
    if (!m_initialized || m_dev == nullptr) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    if (top_line >= 0 && top_line <= MAX_LINES - VISIBLE_LINES) {
        m_display_offset = top_line;
        refresh_display();
    }
}

ssd1306_scroll_types_t Display::convertScrollMode(ScrollMode mode) {
    switch (mode) {
        case ScrollMode::HARDWARE_RIGHT: return SSD1306_SCROLL_RIGHT;
        case ScrollMode::HARDWARE_LEFT:  return SSD1306_SCROLL_LEFT;
        case ScrollMode::HARDWARE_UP:    return SSD1306_SCROLL_UP;
        case ScrollMode::HARDWARE_DOWN:  return SSD1306_SCROLL_DOWN;
        default:                         return SSD1306_SCROLL_STOP;
    }
}

void Display::startHardwareScroll(ScrollMode mode, ssd1306_scroll_frames_t frame_speed) {
    if (!m_initialized || m_dev == nullptr) return;
    if (mode == ScrollMode::NONE || mode == ScrollMode::PAGE_UP ||
        mode == ScrollMode::PAGE_DOWN) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    esp_err_t ret = ssd1306_set_hardware_scroll(m_dev,
                        convertScrollMode(mode), frame_speed);
    xSemaphoreGive(m_bus_mutex);
    if (ret == ESP_OK) m_hardware_scrolling = true;
}

void Display::startHardwareScroll(ScrollMode mode, uint8_t speed) {
    if (!m_initialized || m_dev == nullptr) return;
    if (mode == ScrollMode::NONE || mode == ScrollMode::PAGE_UP ||
        mode == ScrollMode::PAGE_DOWN) return;
    if (m_hardware_scrolling) stopHardwareScroll();
    vTaskDelay(pdMS_TO_TICKS(10));

    const ssd1306_scroll_frames_t lut[] = {
        SSD1306_SCROLL_5_FRAMES,   // 0x00
        SSD1306_SCROLL_64_FRAMES,  // 0x01
        SSD1306_SCROLL_128_FRAMES, // 0x02
        SSD1306_SCROLL_256_FRAMES, // 0x03
        SSD1306_SCROLL_3_FRAMES,   // 0x04
        SSD1306_SCROLL_4_FRAMES,   // 0x05
        SSD1306_SCROLL_25_FRAMES,  // 0x06
        SSD1306_SCROLL_2_FRAMES,   // 0x07
    };
    ssd1306_scroll_frames_t frame_speed =
        (speed < 8) ? lut[speed] : SSD1306_SCROLL_5_FRAMES;

    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    esp_err_t ret = ssd1306_set_hardware_scroll(m_dev,
                        convertScrollMode(mode), frame_speed);
    xSemaphoreGive(m_bus_mutex);
    if (ret == ESP_OK) m_hardware_scrolling = true;
}

void Display::stopHardwareScroll() {
    if (!m_initialized || m_dev == nullptr || !m_hardware_scrolling) return;
    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    ssd1306_set_hardware_scroll(m_dev, SSD1306_SCROLL_STOP,
                                SSD1306_SCROLL_5_FRAMES);
    xSemaphoreGive(m_bus_mutex);
    m_hardware_scrolling = false;
    vTaskDelay(pdMS_TO_TICKS(10));
    refresh_display();
}

void Display::render_char_inverted(int page, int col, char c)
{
    if (!m_initialized || m_dev == nullptr) return;
    if (page < 0 || page >= 8 || col < 0 || col >= 16) return;

    uint8_t inv[8];
    const uint8_t *src = font_latin_8x8_tr[(uint8_t)(unsigned char)c];
    for (int i = 0; i < 8; i++) inv[i] = src[i] ^ 0xFF;

    xSemaphoreTake(m_bus_mutex, portMAX_DELAY);
    ssd1306_display_image(m_dev, (uint8_t)page, (uint8_t)(col * 8), inv, 8);
    xSemaphoreGive(m_bus_mutex);
}

void Display::debug_display_info() {
    if (!m_initialized || m_dev == nullptr) return;
    ESP_LOGI(TAG, "=== Display Debug Info ===");
    ESP_LOGI(TAG, "offset=%d  selected=%d  hw_scroll=%s",
             m_display_offset, m_selected_line,
             m_hardware_scrolling ? "yes" : "no");
    for (int page = 0; page < 8; page++) {
        esp_err_t ret = ssd1306_display_text(m_dev, page, "TEST", false);
        ESP_LOGI(TAG, "Page %d: %s", page,
                 ret == ESP_OK ? "OK" : esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100));
        ssd1306_display_filled_rectangle(m_dev, 0, page*8, 127, page*8+7, true);
    }
    ESP_LOGI(TAG, "=== End Debug Info ===");
}