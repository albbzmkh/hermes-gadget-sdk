// Active-low push buttons, polled from the app task with a short debounce.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

namespace hgp {
namespace {
const char* TAG = "hg.buttons";
// HG 本地改动：防抖回到 3 次（上游原值）。
// 曾经为了配合降到 30 ms 的主循环把它改成 2，后来发现那是在治症不是治因：
// 息屏后单击没反应，根因是主循环轮询太慢，而主循环慢本来是为了让 CPU 睡得久。
// 现在按键改为 light sleep 的 GPIO 唤醒源（见 begin()），按下瞬间芯片就醒，
// 醒来后主循环按 10 ms 的正常节奏采样，3 次防抖仍然只花 30 ms。
constexpr uint8_t kDebounceTicks = 3;
}

void Buttons::begin(const ButtonConfig& cfg) {
  const int gpios[4] = {cfg.talk, cfg.cancel, cfg.up, cfg.down};
  const hg::Button ids[4] = {hg::Button::Talk, hg::Button::Cancel, hg::Button::Up, hg::Button::Down};
  for (int i = 0; i < 4; ++i) {
    buttons_[i].gpio = gpios[i];
    buttons_[i].id = ids[i];
    if (gpios[i] < 0) continue;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gpios[i];
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    // HG 本地改动：让这颗键成为 light sleep 的唤醒源。
    // 为什么：按钮是主动低电平 + 上拉，按下时电平从 1 掉到 0，正好可以当
    // GPIO 低电平唤醒用。ESP-IDF 的自动 light sleep 允许把 GPIO 配成唤醒源，
    // 醒来后 CPU 从 esp_light_sleep_start() 返回，主循环继续跑、立刻采到按下。
    // 这样按键的响应不再取决于主循环多久转一圈，doze 时就能安心让它睡满
    // kDozeWaitMs=250 ms，而不是为了不漏按键被迫把主循环缩短到 30 ms。
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(static_cast<gpio_num_t>(gpios[i]), GPIO_INTR_LOW_LEVEL));
  }
  // 开总闸：arm 之后 light sleep 才会被 GPIO 电平变化打断。
  // 不 arm 时 gpio_wakeup_enable 只是个登记，没有实际作用。
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());
  ESP_LOGI(TAG, "talk/cancel keys armed as light-sleep wake sources");
}

void Buttons::poll(hg::App& app) {
  for (auto& b : buttons_) {
    if (b.gpio < 0) continue;
    bool down = gpio_get_level(static_cast<gpio_num_t>(b.gpio)) == 0;
    if (down == b.pressed) {
      b.stable = 0;
      continue;
    }
    if (++b.stable >= kDebounceTicks) {
      b.pressed = down;
      b.stable = 0;
      app.on_button(b.id, down);
    }
  }
}

}  // namespace hgp
