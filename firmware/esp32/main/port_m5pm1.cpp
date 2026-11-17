// M5PM1 power-management driver for M5Stack StopWatch.
// Register facts are from M5PM1 v1.0.6 and M5Stack's StopWatch BSP.
#include "port_m5pm1.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "hg/battery.hpp"

namespace hgp {
namespace {

constexpr char kTag[] = "hg.m5pm1";
constexpr gpio_num_t kIrqGpio = GPIO_NUM_12;

constexpr uint8_t kRegPowerCfg = 0x06;
constexpr uint8_t kRegI2cCfg = 0x09;
constexpr uint8_t kRegWatchdog = 0x0A;
constexpr uint8_t kRegSystemCommand = 0x0C;
constexpr uint8_t kRegGpioOpenDrain = 0x13;
constexpr uint8_t kRegGpioFunction0 = 0x16;
constexpr uint8_t kRegGpioInput = 0x12;
constexpr uint8_t kRegGpioMode = 0x10;
constexpr uint8_t kRegGpioPull0 = 0x14;
// CHG_STAT is wired to PMIC GPIO2: 0 while the charger is delivering current.
constexpr uint8_t kChargeStatPin = 2;
constexpr uint8_t kRegBatteryMv = 0x22;
constexpr uint8_t kRegVinMv = 0x24;
constexpr uint8_t kRegIrqStatus1 = 0x40;
constexpr uint8_t kRegIrqStatus2 = 0x41;
constexpr uint8_t kRegIrqStatus3 = 0x42;
constexpr uint8_t kRegIrqMask1 = 0x43;
constexpr uint8_t kRegIrqMask2 = 0x44;
constexpr uint8_t kRegIrqMask3 = 0x45;
constexpr uint8_t kRegButtonStatus = 0x48;
constexpr uint8_t kRegButtonCfg1 = 0x49;

constexpr uint8_t kPowerLed = 1u << 4;
constexpr uint8_t kButtonPressed = 1u << 0;
constexpr uint8_t kSingleResetDisable = 1u << 0;
constexpr uint8_t kClickDelayMask = 0x06;
constexpr uint8_t kLongDelayMask = 0x18;
constexpr uint8_t kLongDelay4s = 0x18;
constexpr uint8_t kDoubleDelayMask = 0x60;
constexpr uint8_t kDoubleDelay250ms = 0x20;
constexpr uint8_t kIrqPin = 1u << 1;
constexpr uint8_t kGpio1FunctionMask = 0x0C;
constexpr uint8_t kGpio1IrqFunction = 0x04;
constexpr uint8_t kButtonIrqAll = 0x07;
constexpr uint8_t kButtonWakeIrq = 1u << 1;
constexpr uint8_t kShutdown = 0xA1;
constexpr uint32_t kPollMs = 20;

}  // namespace

bool M5PM1Power::read8(uint8_t reg, uint8_t& value) {
  return dev_ && i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 50) == ESP_OK;
}

bool M5PM1Power::read16(uint8_t reg, uint16_t& value) {
  uint8_t data[2]{};
  if (!dev_ || i2c_master_transmit_receive(dev_, &reg, 1, data, sizeof(data), 50) != ESP_OK) return false;
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool M5PM1Power::write8(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return dev_ && i2c_master_transmit(dev_, data, sizeof(data), 50) == ESP_OK;
}

bool M5PM1Power::update8(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!read8(reg, current)) return false;
  const uint8_t next = static_cast<uint8_t>((current & ~mask) | (value & mask));
  return next == current || write8(reg, next);
}

void M5PM1Power::clear_irqs() {
  for (uint8_t reg = kRegIrqStatus1; reg <= kRegIrqStatus3; ++reg) write8(reg, 0);
}

bool M5PM1Power::begin(i2c_master_bus_handle_t bus) {
  if (!bus || i2c_master_probe(bus, kAddr, 50) != ESP_OK) return false;

  i2c_device_config_t cfg{};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = kAddr;
  cfg.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;

  gpio_config_t irq{};
  irq.pin_bit_mask = 1ULL << kIrqGpio;
  irq.mode = GPIO_MODE_INPUT;
  irq.pull_up_en = GPIO_PULLUP_ENABLE;
  if (gpio_config(&irq) != ESP_OK) return false;

  // The PMIC keeps configuration over an ESP32 reset. Restore known-safe key
  // behavior on every boot: no single-click reset, software hold at 1.5 s,
  // and the PMIC's emergency long action delayed to 4 s.
  bool ok = write8(kRegI2cCfg, 0) && write8(kRegWatchdog, 0);
  ok = ok && update8(kRegButtonCfg1,
                     kSingleResetDisable | kClickDelayMask | kLongDelayMask | kDoubleDelayMask,
                     kSingleResetDisable | kLongDelay4s | kDoubleDelay250ms);
  ok = ok && update8(kRegIrqMask1, 0x1F, 0x1F);
  ok = ok && update8(kRegIrqMask2, 0x3F, 0x3F);
  ok = ok && update8(kRegIrqMask3, kButtonIrqAll, kButtonWakeIrq);
  ok = ok && update8(kRegGpioOpenDrain, kIrqPin, kIrqPin);
  ok = ok && update8(kRegGpioFunction0, kGpio1FunctionMask, kGpio1IrqFunction);
  clear_irqs();

  // The AMOLED is this board's visible power indication and the PMIC LED just
  // burns the 250 mAh cell, so drive it dark from the start. M5Stack's own
  // display HAL does the same (hal_display.cpp display_init()).
  if (update8(kRegPowerCfg, kPowerLed, 0)) led_on_ = false;

  bool down = false;
  if (!ok || !key_pressed(down)) {
    ESP_LOGE(kTag, "M5PM1 initialization failed");
    return false;
  }
  key_down_ = down;
  key_armed_ = !down;
  ESP_LOGI(kTag, "M5PM1 ready; red key=%s, IRQ GPIO12=%d", down ? "down" : "up", gpio_get_level(kIrqGpio));
  return true;
}

bool M5PM1Power::key_pressed(bool& pressed) {
  uint8_t status = 0;
  if (!read8(kRegButtonStatus, status)) return false;
  pressed = (status & kButtonPressed) != 0;
  return true;
}

void M5PM1Power::poll(hg::App& app, uint32_t now_ms) {
  if (!dev_ || now_ms - polled_at_ < kPollMs) return;
  polled_at_ = now_ms;

  bool down = false;
  if (!key_pressed(down)) return;
  if (!key_armed_) {
    key_down_ = down;
    if (!down) key_armed_ = true;
    return;
  }

  // A click is held back for kDoubleClickWindowMs before it becomes the display
  // toggle. The PMIC still owns double-click shutdown (DBL_DLY = 250 ms in
  // begin()), so a first click that toggled the display at once would blank the
  // screen under the user's finger and the second click would land on a dark
  // panel. Waiting past the PMIC's window means a double click never touches the
  // display at all, and a single click lands 500 ms late — what a phone's power
  // key does. Nothing here disables the hardware path: the PMIC powers the board
  // off by itself once it sees the double click.
  if (down && !key_down_) {
    key_down_ = true;
    key_held_ = false;
    key_down_at_ = now_ms;
    if (click_pending_) {
      // Second press inside the window: this is a double click, so the PMIC will
      // power off. Drop the click so the screen is not toggled on the way out.
      click_pending_ = false;
      ESP_LOGI(kTag, "red key double click: leaving shutdown to the PMIC");
    }
  } else if (down && !key_held_ && now_ms - key_down_at_ >= kSettingsHoldMs) {
    key_held_ = true;
    click_pending_ = false;  // a hold is not a click
    if (app.settings_open()) {
      app.close_settings();
      ESP_LOGI(kTag, "red key hold: Settings closed");
    } else if (app.open_settings()) {
      ESP_LOGI(kTag, "red key hold: Settings opened");
    }
  } else if (!down && key_down_) {
    key_down_ = false;
    if (!key_held_) {
      click_pending_ = true;
      click_release_at_ = now_ms;
    }
  } else if (click_pending_ && !down && now_ms - click_release_at_ >= kDoubleClickWindowMs) {
    click_pending_ = false;
    app.on_power_key();
    ESP_LOGI(kTag, "red key short press: display toggled");
  }

  // Reading the live key can leave a completed click latched on IRQ. Clear it
  // after release; the driver derives gestures from the live state itself.
  if (!down && gpio_get_level(kIrqGpio) == 0) clear_irqs();
}

std::optional<hg::PowerStatus> M5PM1Power::read() {
  uint16_t battery_mv = 0, vin_mv = 0;
  uint8_t source = 0;
  // Both voltage registers are 16-bit little-endian (M5PM1_REG_VBAT_L 0x22 /
  // M5PM1_REG_VIN_L 0x24 in M5PM1 v1.0.6). A read of either failing is
  // reported rather than hidden: a silent nullopt here once looked like a
  // board with no battery at all.
  if (!read16(kRegBatteryMv, battery_mv)) {
    ESP_LOGW(kTag, "battery voltage register 0x%02X unreadable", kRegBatteryMv);
    return std::nullopt;
  }
  if (!read16(kRegVinMv, vin_mv)) {
    ESP_LOGW(kTag, "VIN register 0x%02X unreadable", kRegVinMv);
    return std::nullopt;
  }
  const bool charge_known = charge_status(source);

  hg::PowerStatus status;
  if (battery_mv > 2000 && battery_mv < 5000) {
    status.battery_present = true;
    status.battery_mv = battery_mv;
    status.battery_percent = hg::estimate_liion_curve_percent(battery_mv);
  } else {
    status.battery_present = false;
  }
  // The PMIC has no charge-status register: M5Stack wires CHG_STAT to PMIC
  // GPIO2, which reads 0 while the charger is actually pushing current. VIN
  // alone only says a supply is connected, not that charging is under way
  // (M5Stack's own hal_pmic.cpp:161-176 does exactly this).
  status.external_power = vin_mv > 4000;
  if (status.battery_present && *status.battery_present) status.charging = charge_known && source == 0;
  ESP_LOGI(kTag, "battery %u mV (%u%%), VIN %u mV, CHG_STAT=%d", battery_mv,
           status.battery_percent ? *status.battery_percent : 0u, vin_mv, charge_known ? source : -1);
  return status;
}

bool M5PM1Power::charge_status(uint8_t& level) {
  uint8_t input = 0;
  if (!read8(kRegGpioInput, input)) return false;
  level = static_cast<uint8_t>((input >> kChargeStatPin) & 0x01);
  return true;
}

std::optional<bool> M5PM1Power::external_power() {
  uint16_t vin_mv = 0;
  if (!read16(kRegVinMv, vin_mv)) return std::nullopt;
  return vin_mv > 4000;
}

bool M5PM1Power::power_off() {
  ESP_LOGI(kTag, "requesting M5PM1 shutdown");
  // Keep the LED off if USB continues to power the board.
  update8(kRegPowerCfg, kPowerLed, 0);
  return write8(kRegSystemCommand, kShutdown);
}

void M5PM1Power::set_power_led(bool on) {
  // PWR_CFG bit 4 is the LED_EN default level: 0 drives the pin low. The
  // StopWatch's AMOLED is the visible power indication, so the LED is only
  // worth lighting when the panel is dark — M5Stack's own hal_display.cpp
  // keeps it off while brightness is above zero for exactly that reason.
  if (!dev_ || led_on_ == on) return;
  if (!update8(kRegPowerCfg, kPowerLed, on ? kPowerLed : 0)) return;
  led_on_ = on;
  ESP_LOGI(kTag, "power LED %s", on ? "on" : "off");
}

}  // namespace hgp
