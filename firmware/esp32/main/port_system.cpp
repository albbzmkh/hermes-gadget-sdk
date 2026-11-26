#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace hgp {

uint32_t EspSystem::now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint64_t EspSystem::now_us() { return static_cast<uint64_t>(esp_timer_get_time()); }

bool local_wall_time(hg::WallTime& out) {
  const time_t t = time(nullptr);
  struct tm tm{};
  // A fresh boot without RTC and network still reads 1970; a real date is
  // 2025 or later.
  if (!localtime_r(&t, &tm) || tm.tm_year + 1900 < 2025) return false;
  out.year = static_cast<uint16_t>(tm.tm_year + 1900);
  out.month = static_cast<uint8_t>(tm.tm_mon + 1);
  out.day = static_cast<uint8_t>(tm.tm_mday);
  out.weekday = static_cast<uint8_t>(tm.tm_wday);
  out.hour = static_cast<uint8_t>(tm.tm_hour);
  out.minute = static_cast<uint8_t>(tm.tm_min);
  out.second = static_cast<uint8_t>(tm.tm_sec);
  return true;
}

void EspSystem::random_bytes(uint8_t* out, size_t len) {
  // Hardware RNG; entropy is good once the radio is up, and the device key is
  // generated after Wi-Fi starts (see main.cpp).
  esp_fill_random(out, len);
}

void EspSystem::log(hg::LogLevel level, std::string_view message) {
  static const char* TAG = "hg";
  std::string msg(message);
  switch (level) {
    case hg::LogLevel::Debug: ESP_LOGD(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Info: ESP_LOGI(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Warn: ESP_LOGW(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Error: ESP_LOGE(TAG, "%s", msg.c_str()); break;
  }
}

namespace {
// Light sleep taken since the doze began, for the log line that ends it.
std::atomic<int64_t> g_slept_us{0};
std::atomic<uint32_t> g_sleeps{0};
std::atomic<bool> g_dozing{false};

// HG 本地改动：跨 doze 的累计账本（详见 port.hpp 的 SleepTally 注释）。
std::atomic<int64_t> g_total_doze_us{0};
std::atomic<int64_t> g_total_sleep_us{0};
std::atomic<uint32_t> g_total_wakeups{0};
std::atomic<uint16_t> g_total_dozes{0};
std::atomic<uint8_t> g_last_percent{0};
std::atomic<int64_t> g_shortest_sleep_us{0};
std::atomic<int64_t> g_longest_sleep_us{0};
std::atomic<uint32_t> g_refused_sleeps{0};
// 本文件里没有以成员变量存活的状态（count_sleep 是静态回调，读不到 this），
// 所以 doze 起点也从文件级变量取。
std::atomic<int64_t> g_doze_started_us{0};
std::atomic<bool> g_awake_created{false};

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t count_sleep(int64_t slept_us, void*) {
  // HG 本地改动：IDF 在空闲循环的每一圈也会带着 slept_us = 0 调一次这个回调
  // （esp_pm_execute_exit_sleep_callbacks 在 pm_impl.c 里不在那个 slept_us 的
  // 判断块内），所以不判这一下就会把"尝试入睡"也数成唤醒——曾经报出 74 Hz 的
  // 假唤醒，把结论带偏了一整天。
  if (slept_us <= 0) {
    g_refused_sleeps.fetch_add(1, std::memory_order_relaxed);
    return ESP_OK;
  }
  g_slept_us.fetch_add(slept_us, std::memory_order_relaxed);
  g_sleeps.fetch_add(1, std::memory_order_relaxed);
  int64_t shortest = g_shortest_sleep_us.load(std::memory_order_relaxed);
  while (slept_us < shortest &&
         !g_shortest_sleep_us.compare_exchange_weak(shortest, slept_us, std::memory_order_relaxed)) {
  }
  int64_t longest = g_longest_sleep_us.load(std::memory_order_relaxed);
  while (slept_us > longest &&
         !g_longest_sleep_us.compare_exchange_weak(longest, slept_us, std::memory_order_relaxed)) {
  }
  return ESP_OK;
}
#endif
}  // namespace

EspSystem::SleepTally EspSystem::sleep_tally() {
  SleepTally out;
  // 正在进行的这次 doze 也要算进去，否则拔线测到一半插回来读，最后一个 doze 是空的。
  const bool live = g_dozing.load(std::memory_order_relaxed) && g_doze_started_us.load(std::memory_order_relaxed) > 0;
  const int64_t live_us =
      live ? esp_timer_get_time() - g_doze_started_us.load(std::memory_order_relaxed) : 0;
  const int64_t doze_us = g_total_doze_us.load() + live_us;
  const int64_t sleep_us = g_total_sleep_us.load() + g_slept_us.load();
  const uint32_t wakeups = g_total_wakeups.load() + g_sleeps.load();
  out.doze_s = static_cast<uint32_t>(doze_us / 1000000);
  out.sleep_s = static_cast<uint32_t>(sleep_us / 1000000);
  out.wakeups = wakeups;
  out.dozes = static_cast<uint16_t>(g_total_dozes.load() + (live ? 1 : 0));
  out.last_percent =
      doze_us > 0 ? static_cast<uint8_t>(sleep_us * 100 / doze_us) : g_last_percent.load();
  out.shortest_sleep_us = static_cast<uint32_t>(g_shortest_sleep_us.load());
  out.longest_sleep_us = static_cast<uint32_t>(g_longest_sleep_us.load());
  out.avg_sleep_us = wakeups > 0 ? static_cast<uint32_t>(sleep_us / wakeups) : 0;
  out.refused = g_refused_sleeps.load();
  return out;
}

void EspSystem::pm_lock_report(hg::json::Value& out) {
  out.set("dozing", g_dozing.load(std::memory_order_relaxed));
  out.set("awake_lock_created", g_awake_created.load());
  out.set("idle_before_sleep_ticks", static_cast<int>(configEXPECTED_IDLE_TIME_BEFORE_SLEEP));
  out.set("freertos_hz", static_cast<int>(configTICK_RATE_HZ));
  out.set("cpu_max_mhz", static_cast<int>(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
  out.set("tickless", true);
#else
  out.set("tickless", false);
#endif
  const SleepTally t = sleep_tally();
  out.set("live_doze_s", static_cast<int>(t.doze_s));
  out.set("live_sleep_s", static_cast<int>(t.sleep_s));
  out.set("live_wakeups", static_cast<int>(t.wakeups));
  out.set("live_refused", static_cast<int>(t.refused));
  out.set("sleep_ms_min", static_cast<int>(t.shortest_sleep_us / 1000));
  out.set("sleep_ms_max", static_cast<int>(t.longest_sleep_us / 1000));

#if CONFIG_PM_ENABLE
  // esp_pm_dump_locks 只认 FILE*：先写进内存流再逐行解析。
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem = open_memstream(&buf, &len);
  if (!mem) {
    out.set("error", "no stream");
    return;
  }
  esp_pm_dump_locks(mem);
  fflush(mem);
  fclose(mem);

  hg::json::Value locks = hg::json::Value::array();
  hg::json::Value modes = hg::json::Value::array();
  int light_sleep_ok = -1;
  int light_sleep_rejected = -1;
  bool in_modes = false;
  std::string_view text(buf ? buf : "", len);
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const std::string line(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
    pos = nl == std::string_view::npos ? text.size() : nl + 1;
    if (line.find("Mode stats:") != std::string::npos) {
      in_modes = true;
      continue;
    }
    if (line.find("Sleep stats:") != std::string::npos) {
      in_modes = false;
      continue;
    }
    if (line.find("light_sleep_counts:") != std::string::npos) {
      light_sleep_ok = std::atoi(line.c_str() + line.find(':') + 1);
      continue;
    }
    if (line.find("reject_counts:") != std::string::npos) {
      light_sleep_rejected = std::atoi(line.c_str() + line.find(':') + 1);
      continue;
    }
    if (in_modes) {
      // "            SLEEP     0     0 us     0%" 这种行
      char name[32] = {0};
      unsigned count = 0;
      long long us = 0;
      int pct = 0;
      if (std::sscanf(line.c_str(), " %31s %u %lld us %d%%", name, &count, &us, &pct) == 4) {
        hg::json::Value m = hg::json::Value::object();
        m.set("name", name);
        m.set("count", static_cast<int>(count));
        m.set("time_ms", static_cast<int>(us / 1000));
        m.set("pct", pct);
        modes.push(m);
      }
      continue;
    }
    // "i2s_driver     APB_FREQ_MAX    arg=0 active=0 count=838 time=0 us (0%)"
    if (line.find("arg=") == std::string::npos || line.find("active=") == std::string::npos) continue;
    char name[64] = {0}, type[32] = {0};
    int arg = 0, active = 0;
    unsigned count = 0;
    long long us = 0;
    int pct = 0;
    if (std::sscanf(line.c_str(), " %63s %31s arg=%d active=%d count=%u time=%lld us (%d%%)", name, type,
                    &arg, &active, &count, &us, &pct) >= 7) {
      hg::json::Value l = hg::json::Value::object();
      l.set("name", name);
      l.set("type", type);
      l.set("active", active != 0);
      l.set("count", static_cast<int>(count));
      l.set("time_ms", static_cast<int>(us / 1000));
      l.set("pct", pct);
      locks.push(l);
    }
  }
  free(buf);
  out.set("locks", locks);
  out.set("modes", modes);
  out.set("idf_light_sleep_ok", light_sleep_ok);
  out.set("idf_light_sleep_rejected", light_sleep_rejected);
#endif
}

bool EspSystem::enable_doze() {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  if (awake_) return true;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = &count_sleep;
  esp_pm_light_sleep_register_cbs(&cbs);
#endif
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-awake", &awake_) != ESP_OK) {
    awake_ = nullptr;
    return false;
  }
  g_awake_created.store(true, std::memory_order_relaxed);
  esp_pm_lock_acquire(awake_);
  return true;
#else
  return false;
#endif
}

bool EspSystem::dozing_now() { return g_dozing.load(std::memory_order_relaxed); }

namespace {
std::atomic<bool> g_sleep_held{false};
}

void EspSystem::set_sleep_held(bool held) {
  if (g_sleep_held.exchange(held) == held) return;
  if (on_sleep_held_change) on_sleep_held_change();
}

bool EspSystem::sleep_held_now() { return g_sleep_held.load(std::memory_order_relaxed); }

void EspSystem::set_dozing(bool dozing) {
  if (!awake_ || dozing == dozing_) return;
  dozing_ = dozing;
  if (dozing) {
    g_slept_us = 0;
    g_sleeps = 0;
    doze_started_us_ = esp_timer_get_time();
    g_doze_started_us.store(doze_started_us_, std::memory_order_relaxed);
    if (on_doze_start) on_doze_start();
    g_dozing = true;
    ESP_LOGI("hg.power", "dozing: light sleep allowed");
    esp_pm_lock_release(awake_);
    return;
  }
  esp_pm_lock_acquire(awake_);
  g_dozing = false;
  if (on_doze_end) on_doze_end();
  const int64_t dozed_ms = (esp_timer_get_time() - doze_started_us_) / 1000;
  const int64_t slept_ms = g_slept_us.load() / 1000;
  // 结账：把这一次 doze 累加进跨 doze 的账本（diag 的 sleep 段）。
  g_total_doze_us.fetch_add(dozed_ms * 1000, std::memory_order_relaxed);
  g_total_sleep_us.fetch_add(slept_ms * 1000, std::memory_order_relaxed);
  g_total_wakeups.fetch_add(g_sleeps.load(), std::memory_order_relaxed);
  g_total_dozes.fetch_add(1, std::memory_order_relaxed);
  g_last_percent.store(
      static_cast<uint8_t>(dozed_ms > 0 ? slept_ms * 100 / dozed_ms : 0), std::memory_order_relaxed);
  g_doze_started_us.store(0, std::memory_order_relaxed);
  ESP_LOGI("hg.power", "awake after %lld s dozing: light sleep %lld s (%d%%), %u wake-ups", dozed_ms / 1000,
           slept_ms / 1000, dozed_ms > 0 ? static_cast<int>(slept_ms * 100 / dozed_ms) : 0,
           static_cast<unsigned>(g_sleeps.load()));
}

bool EspSystem::wall_time(hg::WallTime& out) { return local_wall_time(out); }

}  // namespace hgp
