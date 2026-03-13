#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

class Display;

class MenuItem {
public:
    using Callback = std::function<void()>;

    explicit MenuItem(const std::string& name);
    MenuItem(const std::string& name, Callback callback);

    void addChild(std::unique_ptr<MenuItem> child);

    const std::string& getName()     const { return name_; }
    MenuItem*          getParent()   const { return parent_; }
    const std::vector<std::unique_ptr<MenuItem>>& getChildren() const { return children_; }
    bool               hasChildren() const { return !children_.empty(); }

    void execute() const;
    void setCallback(Callback callback) { callback_ = callback; }

private:
    std::string name_;
    MenuItem*   parent_;
    std::vector<std::unique_ptr<MenuItem>> children_;
    Callback    callback_;

    friend class Menu;
};

// ── Forward declarations for all system components ────────────────────────────
class ClockManager;
class Networking;
class LedManager;

class Menu {
public:
    static constexpr uint8_t MAX_VISIBLE_ITEMS = 6;

    /**
     * Callback that returns true when the user has signalled "dismiss"
     * (i.e. button pressed and released).  Injected via set_dismiss_fn()
     * so that Menu doesn't depend on the encoder directly.
     */
    using DismissFn = std::function<bool()>;

    explicit Menu(Display& display);
    ~Menu() = default;

    Menu(const Menu&)            = delete;
    Menu& operator=(const Menu&) = delete;

    /**
     * @brief Inject the button-poll function used by info screens to wait
     *        for a dismiss press.  Call once from main before build().
     *        The function should return true exactly once per button press
     *        (edge-detected, not level).
     */
    void set_dismiss_fn(DismissFn fn) { dismiss_fn_ = fn; }

    // ── Navigation ────────────────────────────────────────────────────────────
    void next();
    void previous();
    void select();
    void back();
    void render();

    // ── Wiring ────────────────────────────────────────────────────────────────
    /**
     * @brief Build the full menu tree and wire all callbacks.
     *        Call once after all system components are initialised.
     */
    void build(ClockManager& clock_mgr, Networking& net, LedManager& leds);

    // ── Display blanking ──────────────────────────────────────────────────────
    /**
     * @brief Must be called from the encoder poll loop on any encoder event.
     *        Resets the blank timer and wakes the display if it was blanked.
     */
    void wake();

    /**
     * @brief Call periodically (e.g. every second) to enforce the blank timeout.
     */
    void tick_blank_timer();

    bool is_blanked() const { return blanked_; }

    MenuItem* getCurrentMenu()      const { return current_menu_; }
    size_t    getCurrentSelection()  const { return current_selection_; }

private:
    Display&                    display_;
    LedManager*                 leds_             = nullptr;
    std::unique_ptr<MenuItem>   root_menu_;
    MenuItem*                   current_menu_     = nullptr;
    size_t                      current_selection_ = 0;
    size_t                      display_start_     = 0;
    DismissFn                   dismiss_fn_        = nullptr;

    // Blank timer
    static constexpr uint32_t BLANK_TIMEOUT_S = 300;  // 5 minutes
    uint32_t                  idle_seconds_    = 0;
    bool                      blanked_         = false;

    void                        updateDisplayStart();
    std::vector<std::string>    getVisibleItems() const;

    // Sub-screens (blocking, return to menu on button)
    void show_info_screen(ClockManager& cm, Networking& net);
    void show_clock_status(ClockManager& cm);
    void show_net_status(Networking& net);
    void show_time_screen(ClockManager& cm);

    // Blocks until dismiss_fn_ fires or 30s timeout
    void wait_for_dismiss();

    // Async action dispatch — posts cb to action_task so encoder_task stays free.
    // If an action is already queued or running the new request is silently dropped.
    void post_action(MenuItem::Callback cb);
    static void action_task_fn(void* arg);

    SemaphoreHandle_t action_sem_         = nullptr;  // 1 = slot free
    QueueHandle_t     action_queue_       = nullptr;  // depth 1
    TaskHandle_t      action_task_handle_ = nullptr;
};
