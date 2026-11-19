#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_private/pm_impl.h"
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
// HG 本地改动：正在进行的那次 doze 的起点（us）。`diag sleep` 要在 doze 结束前
// 也能报出实时进度，所以粒度到文件级，不放在成员里。
std::atomic<int64_t> g_doze_started_us{0};
// HG 本地改动：hg-awake 锁建成功没有。静态成员函数读不到成员变量，存一份文件级的。
std::atomic<bool> g_awake_created{false};
// HG 临时诊断：esp_pm_register_skip_light_sleep_callback 能让我们看到
// periph_should_skip_light_sleep() 到底被调用了几次、有没有返回 true。
// 计数为 0 就说明 vApplicationSleep 里第一个条件就没过（s_mode != PM_MODE_LIGHT_SLEEP
// 或 s_is_switching），根本没走到门限比较。收尾时要删掉这段。
std::atomic<uint32_t> g_skip_probe_calls{0};
// HG 临时诊断：enter 回调只会在 vApplicationSleep() 内部、
// esp_light_sleep_start() 之前被 esp_pm_execute_enter_sleep_callbacks() 调到。
// 它计数为 0 就是铁证：vApplicationSleep() 压根没被调用过
// （tasks.c:4386 那道 xExpectedIdleTime 门没过），而不是进去了没睡成。
std::atomic<uint32_t> g_probe_enter_calls{0};
std::atomic<int64_t> g_probe_enter_time{0};
static IRAM_ATTR int probe_enter_cb(int64_t sleep_time_us, void* arg) {
  (void)arg;
  g_probe_enter_calls.fetch_add(1, std::memory_order_relaxed);
  g_probe_enter_time.store(sleep_time_us, std::memory_order_relaxed);
  return 0;
}
// HG 临时诊断：把"下一个 esp_timer 闹钟还有多久"也记下来。
// vApplicationSleep 里的 sleep_time_us = MIN(空闲tick换算的us, time_until_next_alarm)，
// 只要这个值小于门限就一次都不睡。我们在 skip 回调里采样这个值，
// 就能看出是"定时器太密"还是"参数算错"。
std::atomic<int64_t> g_probe_alarm_min{INT64_MAX};
std::atomic<int64_t> g_probe_alarm_last{0};
std::atomic<uint32_t> g_probe_alarm_negative{0};
static bool IRAM_ATTR skip_probe_cb(void) {
  g_skip_probe_calls.fetch_add(1, std::memory_order_relaxed);
  const int64_t next = esp_timer_get_next_alarm_for_wake_up();
  const int64_t now = esp_timer_get_time();
  const int64_t d = next - now;
  g_probe_alarm_last.store(d, std::memory_order_relaxed);
  if (d <= 0) g_probe_alarm_negative.fetch_add(1, std::memory_order_relaxed);
  int64_t cur = g_probe_alarm_min.load(std::memory_order_relaxed);
  while (d < cur && !g_probe_alarm_min.compare_exchange_weak(cur, d, std::memory_order_relaxed)) {
  }
  // 返回 false：不改真实判定，只计数。
  return false;
}
// HG 本地改动：本次上电以来的累计（见 port.hpp 的 SleepTally 说明）。
// 为什么不用 NVS：上电清零正好对应"这一块电池能撑多久"，而 NVS 写入要擦 flash，
// 为了记一行统计去磨损 flash、还多一次唤醒，不值。
int64_t g_total_doze_us = 0;
int64_t g_total_sleep_us = 0;
uint32_t g_total_wakeups = 0;
uint16_t g_total_dozes = 0;
uint8_t g_last_percent = 0;
// HG 本地改动：一次 light sleep 最短能到几百微秒，用 us 记最短的那次最有说服力——
// 如果最短只有几百 us，说明有高频中断在反复把芯片从睡眠里戳醒。
int64_t g_shortest_sleep_us = 0;
int64_t g_longest_sleep_us = 0;
// HG 本地改动：被拒绝的入睡尝试。IDF 只在 slept_us > 0 时算真睡，但回调无论
// 成功失败都会被调，所以 slept_us == 0 的次数就是"想睡没睡成"的次数。
// 拿它和真睡次数一比，就知道有多少机会被别的东西抢走了。
uint32_t g_refused_sleeps = 0;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t count_sleep(int64_t slept_us, void*) {
  // HG 本地改动：只有真的睡着了才算一次唤醒。
  // 为什么：IDF 的 pm_impl.c:922 把 exit 回调放在
  // `if (sleep_time_us >= EXPECTED_IDLE_TIME_BEFORE_SLEEP)` 之外，空闲循环每转
  // 一圈——哪怕空闲时间太短没进 sleep、或者 esp_light_sleep_start() 被拒绝——
  // 都会带着 slept_us = 0 调一次。原来的写法把这种"尝试"也计成唤醒，
  // 实测虚报成 42 万次/96 分钟（约 74 Hz），把真正的唤醒频率完全淹掉了。
  if (slept_us <= 0) {
    ++g_refused_sleeps;
    return ESP_OK;
  }
  g_slept_us.fetch_add(slept_us, std::memory_order_relaxed);
  g_sleeps.fetch_add(1, std::memory_order_relaxed);
  if (g_shortest_sleep_us == 0 || slept_us < g_shortest_sleep_us) g_shortest_sleep_us = slept_us;
  if (slept_us > g_longest_sleep_us) g_longest_sleep_us = slept_us;
  return ESP_OK;
}
#endif
}  // namespace

bool EspSystem::enable_doze() {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  if (awake_) return true;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = &count_sleep;
  cbs.enter_cb = &probe_enter_cb;
  esp_pm_light_sleep_register_cbs(&cbs);
#endif
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-awake", &awake_) != ESP_OK) {
    awake_ = nullptr;
    return false;
  }
  g_awake_created.store(true, std::memory_order_relaxed);
  esp_pm_lock_acquire(awake_);
  // HG 临时诊断：挂一个只计数的 skip 回调，看 periph_should_skip_light_sleep() 被问到几次。
  esp_pm_register_skip_light_sleep_callback(skip_probe_cb);
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
  g_last_percent = dozed_ms > 0 ? static_cast<uint8_t>(slept_ms * 100 / dozed_ms) : 0;
  // HG 本地改动：累加进总账，`diag sleep` 事后取。
  g_total_doze_us += (esp_timer_get_time() - doze_started_us_);
  g_total_sleep_us += g_slept_us.load();
  g_total_wakeups += g_sleeps.load();
  if (g_total_dozes < 0xFFFF) ++g_total_dozes;
  ESP_LOGI("hg.power", "awake after %lld s dozing: light sleep %lld s (%d%%), %u wake-ups", dozed_ms / 1000,
           slept_ms / 1000, static_cast<int>(g_last_percent), static_cast<unsigned>(g_sleeps.load()));
}

// HG 本地改动：把累计的睡眠账目交出去，给 `diag sleep` 用。
EspSystem::SleepTally EspSystem::sleep_tally() {
  SleepTally t;
  // HG 本地改动：把"正在进行、还没结束的那一次 doze"也算进来。
  // 原来的 bug：g_total_* 只在 set_dozing(false) 时累加，于是拔线跑一小时、
  // 直接插回 USB 那一刻正是 doze 被结束的时候——先读 diag 再插线，这次电池
  // 的全部睡眠数据都还挂在"未结算"里，读出来全是 0，看起来像根本没用。
  // 这里把总量和进行中的量分开给：`diag sleep` 报总量（含进行中的），
  // `diag pmlocks` 报 live_* 那一组。
  const bool dozing = g_dozing.load(std::memory_order_relaxed);
  const int64_t started = g_doze_started_us.load(std::memory_order_relaxed);
  const int64_t live_us = dozing && started ? esp_timer_get_time() - started : 0;
  const int64_t total_doze_us = g_total_doze_us + live_us;
  const int64_t total_sleep_us = g_total_sleep_us + g_slept_us.load();
  const uint32_t total_wakeups = g_total_wakeups + g_sleeps.load();
  const uint32_t dozes = static_cast<uint32_t>(g_total_dozes) + (dozing ? 1u : 0u);
  t.doze_s = static_cast<uint32_t>(total_doze_us / 1000000);
  t.sleep_s = static_cast<uint32_t>(total_sleep_us / 1000000);
  t.wakeups = total_wakeups;
  t.dozes = dozes > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(dozes);
  t.last_percent = g_last_percent;
  t.shortest_sleep_us = static_cast<uint32_t>(g_shortest_sleep_us);
  t.longest_sleep_us = static_cast<uint32_t>(g_longest_sleep_us);
  t.avg_sleep_us = total_wakeups ? static_cast<uint32_t>(static_cast<uint64_t>(total_sleep_us) / total_wakeups) : 0;
  t.refused = g_refused_sleeps;
  return t;
}

// HG 本地改动：把 IDF 记的电源锁账本交出去（`diag pmlocks`）。
// 为什么要官方工具：我们自己能控制的只有 hg-awake 这一把锁和 I2S 通道，
// 但只要还有任何第三方组件（Wi-Fi、esp_timer、spi_flash、触摸……）持着
// ESP_PM_NO_LIGHT_SLEEP 或 ESP_PM_APB_FREQ_MAX 不放，芯片就睡不下去。
// esp_pm_lock_get_stats() 直接给出每个锁的持有时长和次数，比继续做时序推理强。
//
// 顺带修一个记账 bug：g_total_sleep_us / g_total_wakeups 原来只在 set_dozing(false)
// 时累加，于是"正在进行的这一次 doze"完全不在账里——拔线跑完直接插 USB，
// 插上那一刻 doze 被结束，账面数字恰好就是这次电池的；但如果期间复位过，
// 或者插上后又被 pid 拉起来，账面对不上。现在累加同时包含进行中的那次，
// 已经结束的量另存一份，`diag sleep` 两者都给。
void EspSystem::pm_lock_report(hg::json::Value& out) {
  // doze 状态是文件内匿名 namespace 的原子变量，不走成员，这里直接取。
  const bool dozing = g_dozing.load(std::memory_order_relaxed);
  const int64_t doze_started = g_doze_started_us.load(std::memory_order_relaxed);
  const int64_t slept = g_slept_us.load(std::memory_order_relaxed);
  const uint32_t wakes = g_sleeps.load(std::memory_order_relaxed);
  const uint32_t refused = g_refused_sleeps;
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  out.set("lock_held", !dozing);
  out.set("dozing", dozing);
#else
  out.set("lock_held", false);
  out.set("dozing", dozing);
  out.set("enabled", false);
#endif
  out.set("awake_lock_created", g_awake_created.load(std::memory_order_relaxed));
  out.set("skip_probe_calls", static_cast<int>(g_skip_probe_calls.load(std::memory_order_relaxed)));
  out.set("probe_enter_calls", static_cast<int>(g_probe_enter_calls.load(std::memory_order_relaxed)));
  out.set("probe_enter_time_us", static_cast<long long>(g_probe_enter_time.load(std::memory_order_relaxed)));
  out.set("probe_alarm_min_us", static_cast<long long>(g_probe_alarm_min.load(std::memory_order_relaxed)));
  out.set("probe_alarm_last_us", static_cast<long long>(g_probe_alarm_last.load(std::memory_order_relaxed)));
  out.set("probe_alarm_negative", static_cast<int>(g_probe_alarm_negative.load(std::memory_order_relaxed)));
  out.set("idle_before_sleep_ticks", CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP);
  out.set("freertos_hz", CONFIG_FREERTOS_HZ);
  out.set("cpu_max_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
  out.set("tickless", true);
#else
  out.set("tickless", false);
#endif
  // 正在进行的这一次 doze 的实时账：结束前也看得到，不必等唤醒。
  out.set("live_doze_s", static_cast<int>(dozing && doze_started ? (esp_timer_get_time() - doze_started) / 1000000 : 0));
  out.set("live_sleep_s", static_cast<int>(slept / 1000000));
  out.set("live_wakeups", static_cast<int>(wakes));
  out.set("live_refused", static_cast<int>(refused));

  // HG 本地改动：把 IDF 自己记的锁账原样搬进 JSON。
  // 为什么不用 esp_pm_get_lock_stats_all()：那个接口按"锁类型"汇总，只给
  // created / acquired 两个数。实测下来靠它分辨不出"某个锁被拿了多久"，
  // 而 time_held 才是判断"谁在拦着 light sleep"的关键。esp_pm_dump_locks()
  // 输出的是**每个锁实例**一行，带着 Name / Active / Total_count / Time(us) / Time(%)，
  // 这正是需要的。它要一个 FILE*，用 open_memstream 截到内存里再逐行解析。
  // 解析时只认已知类型名，格式若有变化（IDF 升级）就当没匹配上，不会错报。
#if CONFIG_PM_ENABLE
  char* buf = nullptr;
  size_t len = 0;
  FILE* mem = open_memstream(&buf, &len);
  if (mem) {
    hg::json::Value rows = hg::json::Value::array();
    if (esp_pm_dump_locks(mem) == ESP_OK) {
      fclose(mem);
      mem = nullptr;
      // 每行形如
      //   hg-awake        NO_LIGHT_SLEEP  0       1        42            12345          97%
      // 列之间是空格；名字最多 15 字符，可能含连字符。
      const char* p = buf;
      while (p && *p) {
        const char* eol = strchr(p, '\n');
        const size_t n = eol ? static_cast<size_t>(eol - p) : strlen(p);
        if (n) {
          std::string line(p, n);
          // 只挑含类型名的行，表头和数据行都以类型名区分。
          const char* types[] = {"CPU_FREQ_MAX", "APB_FREQ_MAX", "NO_LIGHT_SLEEP"};
          for (const char* t : types) {
            const size_t at = line.find(t);
            if (at == std::string::npos) continue;
            // 名字 = 类型名之前的文本（去掉末尾空格）。
            std::string name = line.substr(0, at);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            // 类型之后的数字：%d arg, %d active, %d total_count, %lld time_us, %lld %
            long arg = 0, active = 0, count = 0;
            long long time_us = 0, pct = 0;
            const int got = sscanf(line.c_str() + at + strlen(t), "%ld %ld %ld %lld %lld", &arg, &active, &count,
                                   &time_us, &pct);
            if (got >= 4) {
              hg::json::Value one = hg::json::Value::object();
              one.set("name", name).set("type", std::string(t)).set("arg", static_cast<int>(arg))
                  .set("active", static_cast<int>(active)).set("count", static_cast<int>(count))
                  .set("time_ms", static_cast<int>(time_us / 1000)).set("pct", static_cast<int>(pct));
              rows.push(one);
            }
            break;
          }
        }
        if (!eol) break;
        p = eol + 1;
      }
    } else {
      fclose(mem);
      mem = nullptr;
    }
    // Mode stats / Sleep stats 两段单独取：它们才是"到底睡没睡、被拒几次"的权威数。
    // 格式：
    //   Mode stats:
    //   Mode      CPU_freq    Time(us)    Time(%)
    //   CPU160M   160        12345       45%
    //   ...
    //   Sleep stats:
    //   light_sleep_counts:12  light_sleep_reject_counts:0
    hg::json::Value modes = hg::json::Value::array();
    if (buf) {
      const char* p2 = buf;
      bool in_modes = false;
      while (p2 && *p2) {
        const char* eol2 = strchr(p2, '\n');
        const size_t n2 = eol2 ? static_cast<size_t>(eol2 - p2) : strlen(p2);
        const std::string line2(p2, n2);
        if (line2.find("Mode stats:") != std::string::npos) {
          in_modes = true;
        } else if (line2.find("Sleep stats:") != std::string::npos) {
          in_modes = false;
          // 下一行是 counts
          if (eol2) {
            const char* p3 = eol2 + 1;
            const char* eol3 = strchr(p3, '\n');
            const size_t n3 = eol3 ? static_cast<size_t>(eol3 - p3) : strlen(p3);
            const std::string counts(p3, n3);
            const size_t at = counts.find("light_sleep_counts:");
            const size_t rj = counts.find("reject_counts:");
            if (at != std::string::npos) out.set("idf_light_sleep_ok", atoi(counts.c_str() + at + 19));
            if (rj != std::string::npos) out.set("idf_light_sleep_rejected", atoi(counts.c_str() + rj + 14));
          }
        } else if (in_modes) {
          // Mode 行：名字 + 频率 + us + %
          char name[16] = {0};
          long long us = 0, pct = 0;
          unsigned freq = 0;
          if (sscanf(line2.c_str(), "%15s %u %lld %lld%%", name, &freq, &us, &pct) == 4) {
            hg::json::Value m = hg::json::Value::object();
            m.set("mode", name).set("freq_mhz", static_cast<int>(freq))
             .set("us", us).set("pct", static_cast<int>(pct));
            modes.push(m);
          }
        }
        if (!eol2) break;
        p2 = eol2 + 1;
      }
    }
    out.set("modes", modes);
    out.set("locks", rows);
    // 把 dump 原文尾段带出去，临时用于确认 Mode stats 到底有没有被打印（解析没命中时看这个）。
    out.set("raw_bytes", static_cast<int>(len));
    if (buf && len > 300) {
      out.set("raw_tail", std::string(buf + len - 300, 300));
    }
  }
  free(buf);
#else
  out.set("modes", hg::json::Value::array());
  out.set("locks", hg::json::Value::array());
#endif
}

bool EspSystem::wall_time(hg::WallTime& out) { return local_wall_time(out); }

}  // namespace hgp
