/**
 * @file led_manager.cpp
 * @brief WS2812B dual-strip LED manager with extensible effects
 *
 * Effects
 * ───────
 *  OFF      — strips dark
 *  STATIC   — solid colour at brightness
 *  BREATHE  — sinusoidal pulse (~3 s period)
 *  RAINBOW  — hue rotates across all pixels
 *  CHASE    — lit pixel with short tail sweeping the strip
 *  SPARKLE  — random pixels twinkle
 *  WIPE     — fills then clears the strip from one end
 *  COMET    — bright head + 8-pixel fading tail
 *
 * Each strip is independent.  Target::BOTH applies changes to both.
 * The effect task runs at ~30 fps (33 ms tick) on a dedicated FreeRTOS task.
 */

#include "led_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdlib>
#include <cmath>

static const char* TAG = "LedMgr";

// ── Static helpers ────────────────────────────────────────────────────────────

const char* LedManager::effect_name(Effect e)
{
    switch (e) {
        case Effect::OFF:     return "Off";
        case Effect::STATIC:  return "Static";
        case Effect::BREATHE: return "Breathe";
        case Effect::RAINBOW: return "Rainbow";
        case Effect::CHASE:   return "Chase";
        case Effect::SPARKLE: return "Sparkle";
        case Effect::WIPE:    return "Wipe";
        case Effect::COMET:   return "Comet";
        default:              return "Unknown";
    }
}

LedManager::Effect LedManager::effect_next(Effect e)
{
    uint8_t n = (static_cast<uint8_t>(e) + 1) % static_cast<uint8_t>(Effect::COUNT);
    return static_cast<Effect>(n);
}

uint8_t LedManager::effect_count()
{
    return static_cast<uint8_t>(Effect::COUNT);
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

LedManager::LedManager(gpio_num_t gpio1, gpio_num_t gpio2, uint16_t max_len)
{
    strips_[0].gpio       = gpio1;
    strips_[0].max_len    = max_len;
    strips_[0].active_len = max_len;

    strips_[1].gpio       = gpio2;
    strips_[1].max_len    = max_len;
    strips_[1].active_len = max_len;

    mutex_ = xSemaphoreCreateMutex();
}

LedManager::~LedManager()
{
    if (task_handle_) { vTaskDelete(task_handle_); task_handle_ = nullptr; }
    for (auto& s : strips_) {
        if (s.handle) { led_strip_del(s.handle); s.handle = nullptr; }
    }
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
}

// ── init() ────────────────────────────────────────────────────────────────────

esp_err_t LedManager::init()
{
    for (auto& s : strips_) {
        led_strip_config_t cfg = {};
        cfg.strip_gpio_num          = static_cast<int>(s.gpio);
        cfg.max_leds                = s.max_len;
        cfg.led_model               = LED_MODEL_WS2812;
        cfg.color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

        led_strip_rmt_config_t rmt_cfg = {};
        rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz

        esp_err_t ret = led_strip_new_rmt_device(&cfg, &rmt_cfg, &s.handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "led_strip_new_rmt_device GPIO%d: %s",
                     (int)s.gpio, esp_err_to_name(ret));
            return ret;
        }
        led_strip_clear(s.handle);
        ESP_LOGI(TAG, "Strip GPIO%d ready, %u px", (int)s.gpio, s.max_len);
    }
    return ESP_OK;
}

// ── start() ───────────────────────────────────────────────────────────────────

void LedManager::start()
{
    xTaskCreate(effect_task, "led_fx", 3072, this, 2, &task_handle_);
}

// ── Public setters ────────────────────────────────────────────────────────────

void LedManager::set_color(Target t, uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            strips_[i].r = r;
            strips_[i].g = g;
            strips_[i].b = b;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_brightness(Target t, uint8_t brightness)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i)
            strips_[i].brightness = brightness;
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_effect(Target t, Effect e)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            strips_[i].effect    = e;
            strips_[i].phase     = 0;
            strips_[i].chase_pos = 0;
            strips_[i].wipe_pos  = 0;
            strips_[i].wipe_fill = true;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_active_len(Target t, uint16_t len)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            if (len > strips_[i].max_len) len = strips_[i].max_len;
            strips_[i].active_len = len;
        }
    }
    xSemaphoreGive(mutex_);
}

void LedManager::next_effect(Target t)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i) {
            Effect e = effect_next(strips_[i].effect);
            strips_[i].effect    = e;
            strips_[i].phase     = 0;
            strips_[i].chase_pos = 0;
            strips_[i].wipe_pos  = 0;
            strips_[i].wipe_fill = true;
            ESP_LOGI(TAG, "Strip %d → %s", i, effect_name(e));
        }
    }
    xSemaphoreGive(mutex_);
}

// ── Public getters ────────────────────────────────────────────────────────────

LedManager::Effect LedManager::get_effect(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return Effect::OFF;
    return strips_[idx].effect;
}

uint8_t LedManager::get_brightness(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 0;
    return strips_[idx].brightness;
}

uint16_t LedManager::get_active_len(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 0;
    return strips_[idx].active_len;
}

void LedManager::get_color(int idx, uint8_t& r, uint8_t& g, uint8_t& b) const
{
    if (idx < 0 || idx >= STRIP_COUNT) { r = g = b = 0; return; }
    r = strips_[idx].r;
    g = strips_[idx].g;
    b = strips_[idx].b;
}

uint8_t LedManager::get_speed(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 5;
    return strips_[idx].speed;
}

uint16_t LedManager::get_group_size(int idx) const
{
    if (idx < 0 || idx >= STRIP_COUNT) return 0;
    return strips_[idx].group_size;
}

void LedManager::set_speed(Target t, uint8_t speed)
{
    if (speed < 1)  speed = 1;
    if (speed > 10) speed = 10;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i)
            strips_[i].speed = speed;
    }
    xSemaphoreGive(mutex_);
}

void LedManager::set_group_size(Target t, uint16_t size)
{
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (t == Target::BOTH || static_cast<int>(t) == i)
            strips_[i].group_size = size;
    }
    xSemaphoreGive(mutex_);
}

// ── Effect task ───────────────────────────────────────────────────────────────

void LedManager::effect_task(void* arg)
{
    auto* self = static_cast<LedManager*>(arg);
    while (true) {
        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        for (auto& s : self->strips_) {
            if (s.handle) self->tick_strip(s);
        }
        xSemaphoreGive(self->mutex_);
        vTaskDelay(pdMS_TO_TICKS(33));   // ~30 fps
    }
}

// ── tick_strip ────────────────────────────────────────────────────────────────

void LedManager::tick_strip(StripState& s)
{
    switch (s.effect) {
        case Effect::OFF:     fx_off(s);     break;
        case Effect::STATIC:  fx_static(s);  break;
        case Effect::BREATHE: fx_breathe(s); break;
        case Effect::RAINBOW: fx_rainbow(s); break;
        case Effect::CHASE:   fx_chase(s);   break;
        case Effect::SPARKLE: fx_sparkle(s); break;
        case Effect::WIPE:    fx_wipe(s);    break;
        case Effect::COMET:   fx_comet(s);   break;
        default: break;
    }
    s.phase++;
    led_strip_refresh(s.handle);
}

// ── eff_divisor / eff_group ───────────────────────────────────────────────────

uint16_t LedManager::eff_divisor(const StripState& s)
{
    // Normalised to strip length: one full cycle ≈ 2 s at speed=5 for any strip.
    // divisor = 300 / (speed × active_len), min 1
    // Examples at speed=5: 6 LEDs→10, 30 LEDs→2, 60 LEDs→1
    if (s.active_len == 0) return 1;
    uint32_t d = 300u / (static_cast<uint32_t>(s.speed) * s.active_len);
    return static_cast<uint16_t>(d < 1u ? 1u : d);
}

uint16_t LedManager::eff_group(const StripState& s)
{
    uint16_t g = s.group_size > 0 ? s.group_size
                                   : static_cast<uint16_t>(std::max(1, (int)s.active_len / 5));
    return std::min(g, s.active_len);
}

// ── apply_pixel ───────────────────────────────────────────────────────────────

void LedManager::apply_pixel(StripState& s, uint16_t idx,
                              uint8_t r, uint8_t g, uint8_t b)
{
    if (!s.handle || idx >= s.active_len) return;
    uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(r) * s.brightness / 255);
    uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(g) * s.brightness / 255);
    uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(b) * s.brightness / 255);
    led_strip_set_pixel(s.handle, idx, rs, gs, bs);
}

// ── hue_to_rgb ────────────────────────────────────────────────────────────────

void LedManager::hue_to_rgb(uint8_t hue, uint8_t& r, uint8_t& g, uint8_t& b)
{
    uint8_t region = hue / 43;
    uint8_t rem    = static_cast<uint8_t>((hue - region * 43) * 6);
    uint8_t q      = 255 - rem;
    uint8_t t      = rem;
    switch (region) {
        case 0:  r = 255; g = t;   b = 0;   break;
        case 1:  r = q;   g = 255; b = 0;   break;
        case 2:  r = 0;   g = 255; b = t;   break;
        case 3:  r = 0;   g = q;   b = 255; break;
        case 4:  r = t;   g = 0;   b = 255; break;
        default: r = 255; g = 0;   b = q;   break;
    }
}

// ── Effects ───────────────────────────────────────────────────────────────────

void LedManager::fx_off(StripState& s)
{
    for (uint16_t i = 0; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_static(StripState& s)
{
    for (uint16_t i = 0; i < s.active_len; i++)
        apply_pixel(s, i, s.r, s.g, s.b);
    // blank any pixels beyond active_len
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_breathe(StripState& s)
{
    // Period: speed=1→~15 s, speed=5→~3 s, speed=10→~1.5 s
    uint32_t period = std::max(10u, 450u / s.speed);
    float t     = static_cast<float>(s.phase % period) / static_cast<float>(period);
    float level = (1.0f - cosf(2.0f * static_cast<float>(M_PI) * t)) * 0.5f;
    auto  bsc   = static_cast<uint8_t>(static_cast<float>(s.brightness) * level);
    for (uint16_t i = 0; i < s.active_len; i++) {
        uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(s.r) * bsc / 255);
        uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(s.g) * bsc / 255);
        uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(s.b) * bsc / 255);
        led_strip_set_pixel(s.handle, i, rs, gs, bs);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_rainbow(StripState& s)
{
    // hue rotation: speed=1→1/tick, speed=5→3/tick (current), speed=10→6/tick
    uint8_t hue_step = static_cast<uint8_t>(std::max(1, (int)s.speed * 3 / 5));
    for (uint16_t i = 0; i < s.active_len; i++) {
        uint8_t hue = static_cast<uint8_t>(
            (i * 256 / s.active_len + s.phase * hue_step) & 0xFF);
        uint8_t r, g, b;
        hue_to_rgb(hue, r, g, b);
        apply_pixel(s, i, r, g, b);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_chase(StripState& s)
{
    if (s.active_len == 0) return;

    if (s.phase % eff_divisor(s) == 0)
        s.chase_pos = static_cast<uint16_t>((s.chase_pos + 1) % s.active_len);

    for (uint16_t i = 0; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);

    uint16_t tail = eff_group(s);
    for (uint16_t t = 0; t < tail; t++) {
        int pos = (static_cast<int>(s.chase_pos) - t + s.active_len) % s.active_len;
        float fade = 1.0f - static_cast<float>(t) / tail;
        uint8_t sc = static_cast<uint8_t>(s.brightness * fade);
        uint8_t rs = static_cast<uint8_t>(static_cast<uint16_t>(s.r) * sc / 255);
        uint8_t gs = static_cast<uint8_t>(static_cast<uint16_t>(s.g) * sc / 255);
        uint8_t bs = static_cast<uint8_t>(static_cast<uint16_t>(s.b) * sc / 255);
        led_strip_set_pixel(s.handle, static_cast<uint16_t>(pos), rs, gs, bs);
    }
}

void LedManager::fx_sparkle(StripState& s)
{
    if (s.phase == 0) {
        for (uint16_t i = 0; i < s.max_len; i++)
            led_strip_set_pixel(s.handle, i, 0, 0, 0);
    }
    // Light and extinguish eff_group() pixels per tick
    uint16_t n = eff_group(s);
    for (uint16_t k = 0; k < n; k++) {
        apply_pixel(s, static_cast<uint16_t>(rand() % s.active_len), s.r, s.g, s.b);
        led_strip_set_pixel(s.handle, static_cast<uint16_t>(rand() % s.active_len), 0, 0, 0);
    }
}

void LedManager::fx_wipe(StripState& s)
{
    if (s.phase % eff_divisor(s) == 0) {
        s.wipe_pos++;
        if (s.wipe_pos > s.active_len) {
            s.wipe_pos  = 0;
            s.wipe_fill = !s.wipe_fill;
        }
    }
    // Rebuild full strip state
    for (uint16_t i = 0; i < s.active_len; i++) {
        bool lit = s.wipe_fill ? (i < s.wipe_pos)
                               : (i >= (s.active_len - s.wipe_pos));
        if (lit) apply_pixel(s, i, s.r, s.g, s.b);
        else     led_strip_set_pixel(s.handle, i, 0, 0, 0);
    }
    for (uint16_t i = s.active_len; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);
}

void LedManager::fx_comet(StripState& s)
{
    if (s.active_len == 0) return;

    for (uint16_t i = 0; i < s.max_len; i++)
        led_strip_set_pixel(s.handle, i, 0, 0, 0);

    int head = static_cast<int>((s.phase / eff_divisor(s)) % s.active_len);
    uint16_t tail_len = eff_group(s);
    for (uint16_t t = 0; t < tail_len; t++) {
        int pos = head - static_cast<int>(t);
        if (pos < 0) pos += s.active_len;
        float fade = 1.0f - static_cast<float>(t) / tail_len;
        uint8_t rs = static_cast<uint8_t>(s.r * fade * s.brightness / 255.0f);
        uint8_t gs = static_cast<uint8_t>(s.g * fade * s.brightness / 255.0f);
        uint8_t bs = static_cast<uint8_t>(s.b * fade * s.brightness / 255.0f);
        led_strip_set_pixel(s.handle, static_cast<uint16_t>(pos), rs, gs, bs);
    }
}
