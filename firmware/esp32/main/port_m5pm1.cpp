// M5PM1 power-management driver for M5Stack StopWatch.
// Register facts are from M5PM1 v1.0.6 and M5Stack's StopWatch BSP.

#include "port_m5pm1.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "hg/battery.hpp"
// HG 本地改动：poll() 要靠 EspSystem::dozing_now() 决定息屏时放慢轮询。
#include "port.hpp"

namespace hgp {
namespace {

constexpr char kTag[] = "hg.m5pm1";
constexpr gpio_num_t kIrqGpio = GPIO_NUM_12;

constexpr uint8_t kRegPowerCfg = 0x06;
// HG 本地改动：0x06 的电源位与 0x07 的保持位（位定义见 M5PM1.h 的注释）。
// 0x06: [3] BOOST_EN / [2] LDO_EN / [1] DCDC_EN / [0] CHG_EN
// 0x07: [5] LDO hold / [4..0] GPIO4..0 hold
constexpr uint8_t kRegHoldCfg = 0x07;
constexpr uint8_t kBoostEnable = 1u << 3;
constexpr uint8_t kLdoEnable = 1u << 2;
constexpr uint8_t kDcdcEnable = 1u << 1;
constexpr uint8_t kChargeEnable = 1u << 0;
constexpr uint8_t kHoldLdo = 1u << 5;
constexpr uint8_t kHoldGpio1 = 1u << 1;
// HG 本地改动：明确写出 0x06 的值。BOOST 关（不用 Grove 口），其余三项开。
constexpr uint8_t kPowerCfgBase = kLdoEnable | kDcdcEnable | kChargeEnable;
// HG 本地改动：IRQ_MASK1 的 5 个 GPIO 位，写 0 表示全部使能中断。
constexpr uint8_t kIrqMask1All = 0x1F;
// HG 本地改动：PMIC 的 I2C 空闲睡眠超时（秒）。0 = 禁用，1..15 有效。
constexpr uint8_t kI2cSleepSeconds = 2;
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
// HG 本地改动：doze 时的轮询间隔。见 poll() 里的说明——20 Hz 的 I2C 读会把 light
// sleep 一次次打断，息屏时没必要这么勤。
constexpr uint32_t kDozePollMs = 200;

}  // namespace

// HG 本地改动：失败时重试一次。
// 为什么需要：上面把 0x09 的 I2C 空闲睡眠打开了（kI2cSleepSeconds）。厂商文档写明
// PMIC 睡着后**第一次 I2C 事务只用来唤醒、该次会失败**，第二次才有效。这里补一次
// 重试，把这一次预期内的失败吃掉，调用方就不用到处判断"这次失败是不是正常的"。
// 额外的好处：M5PM1 的 IRQ 是开漏，总线偶尔被拉低时也能自己恢复。
bool M5PM1Power::read8(uint8_t reg, uint8_t& value) {
  if (!dev_) return false;
  if (i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 50) == ESP_OK) return true;
  return i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 50) == ESP_OK;
}

bool M5PM1Power::read16(uint8_t reg, uint16_t& value) {
  if (!dev_) return false;
  uint8_t data[2]{};
  if (i2c_master_transmit_receive(dev_, &reg, 1, data, sizeof(data), 50) != ESP_OK) {
    // 同上：PMIC 刚被上一条事务唤醒时这一读会失败，再试一次。
    if (i2c_master_transmit_receive(dev_, &reg, 1, data, sizeof(data), 50) != ESP_OK) return false;
  }
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool M5PM1Power::write8(uint8_t reg, uint8_t value) {
  if (!dev_) return false;
  const uint8_t data[] = {reg, value};
  if (i2c_master_transmit(dev_, data, sizeof(data), 50) == ESP_OK) return true;
  // 同上：写入同样会撞上"唤醒事务"，重发一次即可（M5PM1 的寄存器写是幂等的）。
  return i2c_master_transmit(dev_, data, sizeof(data), 50) == ESP_OK;
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
  //
  // HG 本地改动：0x09 原来写 0，把 PMIC 自己的 I2C 空闲睡眠禁用了。
  // 头文件对 0x09 [3:0] SLP_TO 的定义是"I2C空闲超时(秒)，0=禁用，超时后PM1进入睡眠模式"。
  // 这块板待机时 PMIC 是唯一常驻带电的芯片（L0 级，电池直连），它自身不睡就是
  // 白白的静息电流。改成 2 秒：正常轮询（20 ms 一次按键、5 s 一次电池）远密于 2 s，
  // 它根本来不及睡；只有真进 doze、轮询降到 1 s 一次时才会睡下去。
  // 代价按厂商文档：PMIC 睡后第一次 I2C 只用来唤醒、该次事务会失败。所以把
  // read8/write8 改成失败重试一次（见那两个函数），把这一次失败吃掉。
  bool ok = write8(kRegI2cCfg, kI2cSleepSeconds) && write8(kRegWatchdog, 0);
  ok = ok && update8(kRegButtonCfg1,
                     kSingleResetDisable | kClickDelayMask | kLongDelayMask | kDoubleDelayMask,
                     kSingleResetDisable | kLongDelay4s | kDoubleDelay250ms);
  // HG 本地改动：0x43 (IRQ_MASK1) 的位语义和 0x45 相反——头文件写的是
  // "[4:0] 每bit: 0=使能中断 1=屏蔽中断"。原来写 0x1F 等于把 GPIO0-4 五路中断
  // 全部屏蔽（而 0x16 又把 GPIO1 设成 IRQ 功能，等于配了却发不出来）。
  // 这里改成写 0：全部使能。GPIO1 是 PMIC 侧那根 IRQ 输出，对应 ESP32 的 GPIO12。
  ok = ok && update8(kRegIrqMask1, kIrqMask1All, 0);
  ok = ok && update8(kRegIrqMask2, 0x3F, 0x3F);
  // HG 本地改动：0x45（按钮中断屏蔽）的语义和其它屏蔽寄存器相反——库的
  // irqSetBtnMask() 里 ENABLE 是"置位"、DISABLE 是"清位"，所以要把 0x07 三位
  // （CLICK/WAKE/DOUBLE）都置上才算全部使能。原来写 kButtonWakeIrq(0x02) 等于
  // 只放开了 WAKE，单击和双击事件都被屏蔽。
  ok = ok && update8(kRegIrqMask3, kButtonIrqAll, kButtonIrqAll);
  ok = ok && update8(kRegGpioOpenDrain, kIrqPin, kIrqPin);
  ok = ok && update8(kRegGpioFunction0, kGpio1FunctionMask, kGpio1IrqFunction);
  clear_irqs();

  // The AMOLED is this board's visible power indication and the PMIC LED just
  // burns the 250 mAh cell, so drive it dark from the start. M5Stack's own
  // display HAL does the same (hal_display.cpp display_init()).
  if (update8(kRegPowerCfg, kPowerLed, 0)) led_on_ = false;

  // HG 本地改动：显式写出 0x06 的电源位，不再依赖上电残留。
  // 头文件 0x06 的位： [3] BOOST_EN 5VINOUT升压 / [2] LDO_EN 3.3V / [1] DCDC_EN 5V DCDC / [0] CHG_EN 充电。
  // 这一位为什么不读-改-写就别动：0x06 的 [0] CHG_EN 注释写"下载模式或复位事件时自动清零"，
  // 说明这个寄存器在复位后是已知的干净状态，而不是需要保留的现场值。
  // BOOST 关掉的理由：用户确认不用 Grove 口。电池供电时它会一直把电池升到 5 V
  // 送到 5VINOUT 上，那里什么都不接，纯属空转耗电。
  // LDO/DCDC/CHG 保持开启：3.3 V 轨是 ESP32-S3 自己（以及 I2C 上的 M5IOE1/M5PM1 外设）
  // 的电源，5 V DCDC 是 AMOLED 与音频功放的输入；充电使能插着 USB 时要给电池充。
  ok = ok && write8(kRegPowerCfg, kPowerCfgBase);
  led_on_ = false;

  // HG 本地改动：建立 3.3 V LDO 的电源保持，然后才允许 PMIC 进自己的睡眠。
  // 0x07 HOLD_CFG 的注释写得很明白："复位/下载模式/关机时自动清零为0x00"。
  // 我们从没写过它，所以复位之后 [5] LDO(3.3V)电源保持 = 0；而上面刚把 0x09 的
  // I2C 睡眠打开——如果不先建立 hold，PMIC 一睡，3.3 V 轨可能跟着掉，设备就醒不过来。
  // 顺序是硬要求：先 hold，再睡。这里只有一个 GPIO 保持位需要顾及：[1] GPIO1，
  // 那是 IRQ 输出，必须保住电平，否则 ESP32 侧的 GPIO12 会悬空。
  ok = ok && update8(kRegHoldCfg, kHoldLdo | kHoldGpio1, kHoldLdo | kHoldGpio1);

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

  // HG 本地改动：doze 时把轮询从 20 ms 放到 200 ms。这块板的红键在 M5PM1 内部，
  // GPIO12 只是它的 IRQ，而实测 PMIC 的按键状态位和 IRQ 都不动，所以只能在主循环里
  // 反复读 0x48。20 Hz 的 I2C 读本身就会一次次把 CPU 从 light sleep 里叫醒，
  // 把 tickless idle 的占空比压下去；息屏时没人会精确到 20 ms 地按红键，200 ms 足够。
  const bool dozing = hgp::EspSystem::dozing_now();
  if (dozing && now_ms - polled_at_ < kDozePollMs) return;

  bool down = false;
  const bool read_ok = key_pressed(down);
  if (!read_ok) return;
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
  // HG 本地改动：这里原来是 update8（读-改-写），会把当前 0x06 里的一切原样保留。
  // 现在 0x06 由 kPowerCfgBase 显式定义（BOOST 关、LDO/DCDC/CHG 开），关机前
  // 也照这个值写一次，避免残留上一次的 BOOST 状态。LED 位清 0。
  write8(kRegPowerCfg, kPowerCfgBase);
  return write8(kRegSystemCommand, kShutdown);
}

void M5PM1Power::set_power_led(bool on) {
  // PWR_CFG bit 4 is the LED_EN default level: 0 drives the pin low. The
  // StopWatch's AMOLED is the visible power indication, so the LED is only
  // worth lighting when the panel is dark — M5Stack's own hal_display.cpp
  // keeps it off while brightness is above zero for exactly that reason.
  if (!dev_ || led_on_ == on) return;
  // HG 本地改动：同上，以 kPowerCfgBase 为底，只叠加 LED 位，不再读当前值。
  if (!write8(kRegPowerCfg, static_cast<uint8_t>(kPowerCfgBase | (on ? kPowerLed : 0)))) return;
  led_on_ = on;
  ESP_LOGI(kTag, "power LED %s", on ? "on" : "off");
}

}  // namespace hgp
