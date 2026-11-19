// The shared I2C bus, and audio through codec chips: an ES8311 or ES8388 DAC,
// an AW88298 speaker amplifier, and an ES7210 ADC for the microphones, on one
// duplex I2S bus (esp_codec_dev does the codec register work).
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "esp_codec_dev_defaults.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mic_power.hpp"
#include "speaker_queue.hpp"
#include "ws185.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.codec";
constexpr size_t kMicChunk = 320;             // about 13 ms at 24 kHz
constexpr size_t kSpeakerBuffer = 192 * 1024;  // absorbs WiFi-burst backlog without dropping; PSRAM, 8 MB free
constexpr size_t kSpeakerChunk = 512;         // samples per codec write

}  // namespace

// --------------------------------------------------------------------------
// I2C

namespace i2c {
namespace {
i2c_master_bus_handle_t open(const I2cBusConfig& cfg, i2c_port_num_t port, i2c_master_bus_handle_t& handle) {
  if (handle || cfg.sda < 0 || cfg.scl < 0) return handle;
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = port;
  bus_cfg.sda_io_num = static_cast<gpio_num_t>(cfg.sda);
  bus_cfg.scl_io_num = static_cast<gpio_num_t>(cfg.scl);
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus on SDA %d / SCL %d failed", cfg.sda, cfg.scl);
    handle = nullptr;
  }
  return handle;
}
}  // namespace

i2c_master_bus_handle_t bus(const I2cBusConfig& cfg) {
  static i2c_master_bus_handle_t handle = nullptr;
  return open(cfg, I2C_NUM_0, handle);
}

i2c_master_bus_handle_t touch_bus(const I2cBusConfig& cfg) {
  static i2c_master_bus_handle_t handle = nullptr;
  return open(cfg, I2C_NUM_1, handle);
}
}  // namespace i2c

// --------------------------------------------------------------------------
// Codecs

bool CodecAudio::begin(const CodecAudioConfig& cfg, i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  if (cfg.speaker_pa && cfg.pa >= 0) {
    gpio_reset_pin(static_cast<gpio_num_t>(cfg.pa));
    gpio_set_direction(static_cast<gpio_num_t>(cfg.pa), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(cfg.pa), 0);
  }
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // silence on underrun instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx_, &rx_) != ESP_OK) return false;
  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);  // MCLK = 256 x fs
  // The classic-ESP32 macro compares its arguments without parenthesizing them,
  // so a ternary passed straight in would bind wrong; name them first.
  const auto bits = cfg.stereo32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;
  const auto slots = cfg.stereo32 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, slots);
  std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(cfg.mclk);
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(cfg.bclk);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(cfg.ws);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(cfg.dout);
  std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(cfg.din);
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK || i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "I2S setup failed");
    return false;
  }
  // HG 本地改动：这里原来无条件 i2s_channel_enable(tx_); i2s_channel_enable(rx_);
  // 两条通道一开就一直开着。ESP-IDF 官方 Power Management 文档写明 I2S 驱动在
  // enable() 到 disable() 之间持续持有 ESP_PM_NO_LIGHT_SLEEP 锁，所以那样等于
  // 永久禁止 light sleep。现在由 apply_audio_channels() 按需开关（见本函数后半段），
  // 真正打开由录音任务、放音任务和睡眠门三方共同触发。
  // HG 本地改动：两条通道先各打开一次，再交给 esp_codec_dev 自己接管。
  // 为什么必须先开：esp_codec_dev_open() 会调 data_if->set_fmt()，其内部
  // set_fs()（audio_codec_data_i2s.c:422-427）在 rx 是 slave 时会
  // "_i2s_drv_enable(paired=tx, true, false)" 先把 tx 关掉、改完时钟再开回来。
  // 这条 tx 如果从没被 enable 过，就会打出
  //   E i2s_common: i2s_channel_disable(1516): the channel has not been enabled yet
  // 上游固件之所以没这个报错，正是因为它在这里无条件 enable 了 tx 和 rx。
  // 我们保留这个"先开一次"只为满足 open() 的前提；真正的省电在于紧接着
  // 由放音/录音任务把通道关回去（见 apply_audio_channels 的第一次同步）。
  if (tx_) { if (i2s_channel_enable(tx_) == ESP_OK) tx_enabled_ = true; }
  if (rx_) { if (i2s_channel_enable(rx_) == ESP_OK) rx_enabled_ = true; }
  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = I2S_NUM_1;
  i2s_cfg.rx_handle = rx_;
  i2s_cfg.tx_handle = tx_;
  const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
  const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
  // HG 本地改动：这里不再抢先 enable 两条通道。
  // esp_codec_dev_open() 会先调 data_if->set_fmt()（音频数据层内部先把两条通道
  // disable 掉再接新格式），再调 data_if->enable() 把需要的方向打开。如果我们
  // 在 open 之前就把通道开起来，那两次 disable 会因为驱动自己的
  // out_enable/in_enable 状态表还不知道通道已开而打出
  // "i2s_channel_disable: the channel has not been enabled yet" 的报错。
  // 所以这里什么都不做：通道由 esp_codec_dev 在 open() 里打开，我们只在 close/
  // 静音之后按需关掉（见 apply_audio_channels 与 hold_audio_off）。

  audio_codec_i2c_cfg_t dac_i2c = {};
  dac_i2c.port = I2C_NUM_0;
  // Address macros are the 8-bit form. The I2C driver shifts them to 7-bit.
  dac_i2c.addr = cfg.speaker == SpeakerCodec::Aw88298 ? AW88298_CODEC_DEFAULT_ADDR
               : cfg.speaker == SpeakerCodec::Es8388 ? ES8388_CODEC_DEFAULT_ADDR
                                                     : ES8311_CODEC_DEFAULT_ADDR;
  dac_i2c.bus_handle = bus;
  es8311_codec_cfg_t dac = {};
  dac.ctrl_if = audio_codec_new_i2c_ctrl(&dac_i2c);
  dac.gpio_if = gpio_if;
  // The ES8311 has an ADC too. A board that wires an analog microphone to it
  // uses one codec handle for both directions instead of an ES7210.
  const bool shared = cfg.mic == MicCodec::Es8311 && cfg.speaker == SpeakerCodec::Es8311;
  dac.codec_mode = shared ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC;
  // With pa_pin, esp_codec_dev turns the amplifier on in esp_codec_dev_open()
  // and leaves it on; muting only writes a DAC register. speaker_pa boards
  // keep it off while idle and drop it as soon as playback is aborted.
  dac.pa_pin = cfg.speaker_pa ? -1 : static_cast<int16_t>(cfg.pa);
  dac.pa_reverted = false;
  dac.master_mode = false;
  dac.use_mclk = cfg.dac_mclk;
  dac.hw_gain.pa_voltage = cfg.amp_supply_v;
  dac.hw_gain.codec_dac_voltage = 3.3;
  esp_codec_dev_cfg_t out_cfg = {};
  out_cfg.dev_type = shared ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT;
  if (cfg.speaker == SpeakerCodec::Aw88298) {
    aw88298_codec_cfg_t amp = {};
    amp.ctrl_if = dac.ctrl_if;
    amp.gpio_if = gpio_if;
    // No reserve: the driver subtracts the hardware gain from every volume, and 15 dB here (about
    // 11.4 dB net, with the 3.3 V/5 V default) held 100% near -11 dB. M5Unified runs this
    // amplifier at 0 dB (HAGCCFG4 0x0064) on the CoreS3. The driver leaves the AW88298's AGC off
    // (HAGCE=0), so nothing limits the level beyond the volume itself. Checked by ear on a
    // CoreS3, with no audible distortion at 100 (issue #289).
    amp.hw_gain.pa_gain = 0;
    out_cfg.codec_if = aw88298_codec_new(&amp);
  } else if (cfg.speaker == SpeakerCodec::Es8388) {
    // DAC only. The ES8388 ADC is not the microphone path.
    es8388_codec_cfg_t es8388 = {};
    es8388.ctrl_if = dac.ctrl_if;
    es8388.gpio_if = gpio_if;
    es8388.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8388.pa_pin = cfg.speaker_pa ? -1 : static_cast<int16_t>(cfg.pa);
    es8388.pa_reverted = false;
    es8388.master_mode = false;
    es8388.hw_gain.pa_voltage = cfg.amp_supply_v;
    es8388.hw_gain.codec_dac_voltage = 3.3;
    out_cfg.codec_if = es8388_codec_new(&es8388);
  } else {
    out_cfg.codec_if = es8311_codec_new(&dac);
  }
  out_cfg.data_if = data_if;
  out_ = out_cfg.codec_if ? esp_codec_dev_new(&out_cfg) : nullptr;

  if (!shared) {
    audio_codec_i2c_cfg_t adc_i2c = {};
    adc_i2c.port = I2C_NUM_0;
    adc_i2c.addr = ES7210_CODEC_DEFAULT_ADDR;
    adc_i2c.bus_handle = bus;
    es7210_codec_cfg_t adc = {};
    adc.ctrl_if = audio_codec_new_i2c_ctrl(&adc_i2c);
    adc.mic_selected = cfg.es7210_mics;
    esp_codec_dev_cfg_t in_cfg = {};
    in_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    in_cfg.codec_if = es7210_codec_new(&adc);
    in_cfg.data_if = data_if;
    in_ = in_cfg.codec_if ? esp_codec_dev_new(&in_cfg) : nullptr;
  }

  // Both stay open at one rate: they share the I2S clocks.
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = kRate;
  fs.channel = cfg.stereo32 ? 2 : 1;
  fs.bits_per_sample = cfg.stereo32 ? 32 : 16;
  if (out_ && esp_codec_dev_open(out_, &fs) != ESP_CODEC_DEV_OK) out_ = nullptr;
  if (shared) in_ = out_;  // one handle, opened once
  else if (in_ && esp_codec_dev_open(in_, &fs) != ESP_CODEC_DEV_OK) in_ = nullptr;
  if (out_) {
    esp_codec_dev_set_out_vol(out_, 70);
    esp_codec_dev_set_out_mute(out_, true);  // unmuted while something plays
    // A board that asks for it: the ES8311's DAC ramps volume on mute/unmute (reg 0x37, ramp
    // rate in bits 4-7). Another speaker codec has some other register there.
    if (cfg.dac_fade_ramp && cfg.speaker == SpeakerCodec::Es8311) esp_codec_dev_write_reg(out_, 0x37, 0x38);
  }
  if (in_) esp_codec_dev_set_in_gain(in_, cfg.mic_gain_db);
  mic_link_ = {in_ && in_ != out_, rx_, fs, cfg.mic_gain_db};
  mic_link_.owner = this;  // HG: mic 任务通过它开关 rx 通道
  // HG 本地改动：把"每条方向有没有任务会重开它"登记进去。以后 CodecMic /
  // CodecSpeaker 建任务时还会再各自登记一次，这里先把能确定的说清楚：
  //   * 有 in_ 就说明有麦克风任务（CodecMic 一定会被 begin），rx 有人管；
  //   * 有 out_ 就说明有放音设备，CodecSpeaker 会在 begin() 里建任务，tx 有人管。
  // 两边都没有的板子根本不会走到这里（begin 失败返回）。
  bind_tasks(out_ != nullptr, in_ != nullptr);
  ESP_LOGI(TAG, "codecs: speaker %s, microphones %s (%s)", out_ ? "ready" : "missing", in_ ? "ready" : "missing",
           shared ? "es8311 analog" : "es7210");
  return out_ || in_;
}

// --------------------------------------------------------------------------
// I2S 通道门控（HG 本地改动）

void CodecAudio::apply_audio_channels() {
  // 睡眠门压过一切：audio_hold_ 大于 0 时两个方向都必须关。
  // 有任务负责的方向按"该方向是否被需要"决定；没有任务负责的方向（没接麦克风、
  // 或者根本没有放音任务）一律保持关闭，因为关掉就再也没人会重开。
  const bool want_tx = audio_hold_ == 0 && spk_wants_ && tx_bound_;
  const bool want_rx = audio_hold_ == 0 && mic_wants_ && rx_bound_;
  // 先开 rx 再开 tx：ESP-IDF 的双向 I2S 里 rx 会自己切到 slave，两条同时开时
  // 顺序决定了谁做主，先 rx 与上游"两条一起 enable"的最终状态一致。
  if (rx_ && want_rx != rx_enabled_) {
    const esp_err_t err = want_rx ? i2s_channel_enable(rx_) : i2s_channel_disable(rx_);
    if (err == ESP_OK) rx_enabled_ = want_rx;
  }
  if (tx_ && want_tx != tx_enabled_) {
    const esp_err_t err = want_tx ? i2s_channel_enable(tx_) : i2s_channel_disable(tx_);
    if (err == ESP_OK) tx_enabled_ = want_tx;
  }
  // HG 本地改动：把这个状态同步给 mic 任务。共用编解码器的 MicPower 是"一直读"，
  // 通道关着的时候再读只会刷错误日志，所以让它知道什么时候该读。
  if (mic_link_.rx_ok != rx_enabled_) {
    mic_link_.rx_ok = rx_enabled_;
    ESP_LOGI(TAG, "i2s gate: rx %s (hold %d, mic wants %d)", rx_enabled_ ? "on" : "off", audio_hold_, (int)mic_wants_);
  }
}

void CodecAudio::set_want_tx(bool wanted) {
  spk_wants_ = wanted;
  apply_audio_channels();
}

void CodecAudio::set_want_rx(bool wanted) {
  mic_wants_ = wanted;
  apply_audio_channels();
}

void CodecAudio::hold_audio_off(bool off) {
  if (off) {
    ++audio_hold_;
  } else if (audio_hold_ > 0) {
    --audio_hold_;
  }
  apply_audio_channels();
}

// --------------------------------------------------------------------------
// Microphone

bool CodecMic::begin(esp_codec_dev_handle_t dev, bool rmnm, const CodecAudio::MicLink& link) {
  if (!dev) return false;
  dev_ = dev;
  link_ = link;
  // HG 本地改动：MicLink 里带上 CodecAudio 的指针，mic 任务才能按需开关 rx 通道。
  audio_ = link.owner;
  rmnm_ = rmnm;
  if (rmnm) {
    raw_ = static_cast<int16_t*>(heap_caps_malloc(kMicChunk * 4 * sizeof(int16_t), MALLOC_CAP_8BIT));
    if (!raw_) return false;
  }
  xTaskCreate(&CodecMic::task, "hg-mic", 4096, this, 6, &task_);
  return true;
}

bool CodecMic::start(uint32_t sample_rate) {
  if (!dev_) return false;
  if (sample_rate != CodecAudio::kRate) {
    ESP_LOGW(TAG, "microphone runs at %u Hz only", static_cast<unsigned>(CodecAudio::kRate));
    return false;
  }
  capturing_ = true;
  if (task_) xTaskNotifyGive(task_);  // wakes a powered-down mic task
  return true;
}

void CodecMic::task(void* arg) {
  auto* self = static_cast<CodecMic*>(arg);
  int16_t pcm[kMicChunk];
  // The microphone ADC runs only while something records (hg::MicPower).
  struct Ops {
    CodecMic* mic;
    bool power_up() {
      esp_codec_dev_sample_info_t fs = mic->link_.format;
      if (esp_codec_dev_open(mic->dev_, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone did not start");
        return false;
      }
      esp_codec_dev_set_in_gain(mic->dev_, mic->link_.gain_db);
      return true;
    }
    void power_down() { esp_codec_dev_close(mic->dev_); }
    void drain() {
      // The receive channel kept running for the speaker; skip its stale buffers.
      if (!mic->link_.rx) return;
      uint8_t scratch[512];
      size_t got = 0;
      for (int i = 0; i < 64; ++i)
        if (i2s_channel_read(mic->link_.rx, scratch, sizeof(scratch), &got, 0) != ESP_OK || !got) break;
    }
    bool read(int16_t* chunk) {
      void* data = mic->rmnm_ ? static_cast<void*>(mic->raw_) : static_cast<void*>(chunk);
      const size_t bytes = mic->rmnm_ ? kMicChunk * 4 * sizeof(int16_t) : kMicChunk * sizeof(int16_t);
      if (esp_codec_dev_read(mic->dev_, data, bytes) != ESP_CODEC_DEV_OK) return false;
      if (mic->rmnm_) hg::ws185_mono(mic->raw_, chunk, kMicChunk);
      return true;
    }
    // HG 本地改动：rx 通道现在由本文件的门控按需开关，闲时是关着的。
    // MicPower 对"共享编解码器"（switchable_ == false）的设计是让它一直读——
    // 但那种读法在通道关闭后会每 20 ms 打一条
    //   E i2s_common: i2s_channel_read: The channel is not enabled
    // 所以给它一个"这条通道现在能不能读"的旁路：不能读就当这一轮什么都没发生，
    // 既不报错也不阻塞（MicPower 随后会调 wait() 等下一次唤醒）。
    // 注意问的是 owner 的实时状态，不是 mic->link_ 里那份快照：那份快照是
    // begin() 那一刻拍的，门控之后把它改成 off 的话，拷贝里还是 on。
    bool read_ready() const { return !mic->link_.rx || !mic->audio_ || mic->audio_->rx_live(); }
    void deliver(const int16_t* chunk) { events::post(EventType::Mic, chunk, kMicChunk * sizeof(int16_t)); }
    void wait() { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); }
    void pause() { vTaskDelay(pdMS_TO_TICKS(10)); }
  } ops{self};
  hg::MicPower power(self->link_.switchable);
  // HG 本地改动：录音期间打开 rx 通道，空闲时关掉，让 I2S 交还 ESP_PM_NO_LIGHT_SLEEP。
  // 放在这里而不是 MicPower 里，是因为 MicPower 是 host 侧可测的纯逻辑，
  // 不应该知道 I2S 的存在；而且 link_.rx 只在 esp32 侧有效。
  bool rx_on = false;
  // HG 本地改动：第一次循环无条件同步一次（rx_on 初值设为"与实际相反"），
  // 这样开机时 rx 会被显式关掉一次，把 esp_codec_dev_open() 里打开的通道交还给
  // 电源管理——light sleep 才不会被 I2S 的 ESP_PM_NO_LIGHT_SLEEP 锁永久挡住。
  for (;;) {
    const bool capturing = self->capturing_.load();
    if (self->link_.rx && capturing != rx_on) {
      self->audio_->set_want_rx(capturing);
      rx_on = capturing;
    }
    // HG 本地改动：rx 关着的时候不驱动 MicPower 的去读分支。
    // 共用编解码器（ES8311）的 MicPower 认为 ADC 一直开着、可以一直读；
    // 现在通道由门控管，关着时读只会报错。这里直接等下一次唤醒。
    if (!ops.read_ready()) {
      ops.wait();
      continue;
    }
    power.turn(capturing, ops, pcm);
  }
}

// --------------------------------------------------------------------------
// Speaker

bool CodecSpeaker::begin(esp_codec_dev_handle_t dev, bool stereo32, int pa, CodecAudio* audio) {
  if (!dev) return false;
  dev_ = dev;
  audio_ = audio;
  uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(kSpeakerBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticStreamBuffer_t control;
  if (storage) buffer_ = xStreamBufferCreateStatic(kSpeakerBuffer, 1, storage, &control);
  else buffer_ = xStreamBufferCreate(16 * 1024, 1);
  if (!buffer_) return false;
  capacity_ = storage ? kSpeakerBuffer : 16 * 1024;
  stereo32_ = stereo32;
  if (pa >= 0) {
    const auto pin = static_cast<gpio_num_t>(pa);
    pa_.emplace([this](void* chunk, size_t bytes) { return xStreamBufferReceive(buffer_, chunk, bytes, 0); },
                [this] { xStreamBufferReset(buffer_); },
                [pin](bool on) { gpio_set_level(pin, on); });
  } else if (board_pa) {
    pa_.emplace([this](void* chunk, size_t bytes) { return xStreamBufferReceive(buffer_, chunk, bytes, 0); },
                [this] { xStreamBufferReset(buffer_); }, board_pa);
  }
  if (stereo32) {
    stereo_ = static_cast<int32_t*>(heap_caps_malloc(kSpeakerChunk * 2 * sizeof(int32_t), MALLOC_CAP_8BIT));
    if (!stereo_) return false;
  }
  xTaskCreate(&CodecSpeaker::task, "hg-spk", 4096, this, 7, &task_);
  if (audio_) audio_->bind_tasks(true, false);  // HG: tx 从此有任务管，可以收门
  return true;
}

void CodecSpeaker::power_down() {
  if (!dev_ || !can_power_down_ || !powered_ || busy()) return;
  esp_codec_dev_close(dev_);
  powered_ = false;
}

void CodecSpeaker::power_up() {
  if (!dev_ || powered_) return;
  esp_codec_dev_sample_info_t fs = format_;
  if (esp_codec_dev_open(dev_, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "speaker did not start");
    return;
  }
  esp_codec_dev_set_out_vol(dev_, volume_);
  esp_codec_dev_set_out_mute(dev_, true);  // unmuted while something plays
  if (es8311_fade_ramp) esp_codec_dev_write_reg(dev_, 0x37, 0x38);  // reopening reset it (CodecAudio::begin)
  powered_ = true;
}

bool CodecSpeaker::begin(uint32_t sample_rate) {
  if (!dev_) return false;
  if (sample_rate != CodecAudio::kRate) {
    // The host resamples to the rate the device declares, so this is not expected.
    ESP_LOGW(TAG, "speaker runs at %u Hz only", static_cast<unsigned>(CodecAudio::kRate));
    return false;
  }
  // A stream still open, draining, queued or being reset is aborted first. Without the PA
  // queue the speaker task empties the queue for that abort() on its next pass, and audio
  // written before then would go with the old: wait for that reset, at most 6 ticks, so
  // write() stays non-blocking. The P4 speaker waits in begin() too, and as there, an idle
  // speaker has nothing to drop and starts at once. The PA queue resets inside abort().
  if (pa_ || busy() || flush_) abort();
  for (int i = 0; !pa_ && flush_ && i < 6; ++i) vTaskDelay(1);
  power_up();  // a reply can start before the doze that powered it down ends
  open_ = true;
  draining_ = false;
  return true;
}

void CodecSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  size_t bytes = count * sizeof(int16_t);
  // The app task is the only writer, so the room can only grow before the send.
  // Whole samples only: a partial one would shift every later sample by a byte.
  size_t fit = hg::whole_sample_bytes(bytes, xStreamBufferSpacesAvailable(buffer_));
  size_t sent = xStreamBufferSend(buffer_, samples, fit, 0);
  if (task_) xTaskNotifyGive(task_);  // an idle speaker task waits for this
  if (sent < bytes) {
    // Counted for the stream-end log; warn at most every 2 s.
    drop_bytes_ += bytes - sent;
    static int64_t last_drop_log = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_drop_log > 2000) {
      last_drop_log = now;
      ESP_LOGW(TAG, "playback buffer full, dropped %u B cumulative", static_cast<unsigned>(drop_bytes_.load()));
    }
  }
}

void CodecSpeaker::end() {
  // Publication order matters: mark draining first. A consumer that observes
  // open_ == false must already see draining_ == true, or it could mute the amp
  // with queued audio still unplayed.
  draining_ = true;
  open_ = false;
}

void CodecSpeaker::abort() {
  open_ = false;
  draining_ = false;
  if (pa_) pa_->abort();
  flush_ = true;
  if (task_) xTaskNotifyGive(task_);
}

bool CodecSpeaker::busy() const {
  return open_ || draining_ || (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
}

void CodecSpeaker::set_volume(uint8_t percent) {
  volume_ = percent > 100 ? 100 : percent;
  if (dev_ && powered_) esp_codec_dev_set_out_vol(dev_, volume_);
}

void CodecSpeaker::task(void* arg) {
  auto* self = static_cast<CodecSpeaker*>(arg);
  int16_t chunk[kSpeakerChunk];
  int32_t* stereo = self->stereo_;
  auto& pa = self->pa_;
  bool playing = false;
  int64_t stream_t0 = 0;
  uint64_t stream_bytes = 0;
  hg::SpeakerUnderruns gaps;
  // HG 本地改动：放音任务一开始就把 tx 门关掉（bind_tasks 已登记 tx 归它管）。
  // esp_codec_dev_open() 在 begin 阶段已经打开过 tx，那条通道会一直握着
  // ESP_PM_NO_LIGHT_SLEEP 让芯片睡不下去；这里显式收回，等到真要放音时
  // 下面的 set_want_tx(true) 再打开。
  if (self->audio_) self->audio_->set_want_tx(false);
  for (;;) {
    if (self->flush_) {
      stream_t0 = 0;
      stream_bytes = 0;
      self->drop_bytes_ = 0;
      gaps.finish();  // an aborted stream isn't reported
      if (pa) {
        // abort() already emptied the queue and turned the amplifier off.
        esp_codec_dev_set_out_mute(self->dev_, true);
        playing = false;
      } else {
        xStreamBufferReset(self->buffer_);
      }
      self->flush_ = false;  // only after the reset: a write() waiting for it is not wiped
    }
    uint32_t generation = 0;
    size_t got = pa ? pa->receive(chunk, sizeof(chunk), generation)
                    : xStreamBufferReceive(self->buffer_, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
    // Received while an abort() waits for its reset: audio from before it. Drop it;
    // the reset comes next. (The PA queue drops such a chunk by its generation.)
    if (!pa && self->flush_) continue;
    if (got == 0) {
      if (playing && self->open_) gaps.ran_dry(speaker_now_ms());
      if (playing && !self->open_) {
        // The ring is empty but the last real audio may still sit in the I2S DMA
        // ring (esp_codec_dev_write/i2s_channel_write return once queued, not once
        // played). Push silence exceeding DMA ring depth before muting, else the
        // final phoneme clips. I2S_CHANNEL_DEFAULT_CONFIG gives 6 DMA descriptors
        // x 240 frames, in the codec's actual frame width: mono 16-bit = 2880 B
        // (60 ms at 24 kHz); stereo 32-bit = 11520 B. Silence is written in that
        // same width, sized to exceed the ring with margin (3 x 1 KB for mono16,
        // 3 x 4 KB through the stereo conversion for stereo32).
        int errs = 0, first_rc = 0;
        if (self->stereo32_) {
          // Convert silence through the same width path as real audio: 1KB of
          // int16 mono becomes 4KB of int32 stereo; 3 writes = 12288 B > 11520 B.
          for (int i = 0; i < 3 && !self->flush_; i++) {  // a new stream waits: stop early
            memset(self->stereo_, 0, kSpeakerChunk * 2 * sizeof(int32_t));
            int rc = esp_codec_dev_write(self->dev_, self->stereo_, kSpeakerChunk * 2 * sizeof(int32_t));
            if (rc != ESP_CODEC_DEV_OK) { errs++; if (!first_rc) first_rc = rc; }
          }
        } else {
          memset(chunk, 0, sizeof(chunk));  // got == 0, so nothing in it is unplayed
          for (int i = 0; i < 3 && !self->flush_; i++) {  // a new stream waits: stop early
            int rc = esp_codec_dev_write(self->dev_, chunk, sizeof(chunk));
            if (rc != ESP_CODEC_DEV_OK) { errs++; if (!first_rc) first_rc = rc; }
          }
        }
        esp_codec_dev_set_out_mute(self->dev_, true);
        if (pa) vTaskDelay(pdMS_TO_TICKS(20));  // DAC decays to silence before PA off
        self->draining_ = false;  // busy() stays true until the tail is played and muted
        if (errs) ESP_LOGW(TAG, "spk tail drain: %d/3 writes failed (rc of first=%d), tail may clip", errs, first_rc);
        if (pa) pa->disable();
        // HG 本地改动：音频尾巴送完、静音之后才关 tx 通道，让 I2S 交还
        // ESP_PM_NO_LIGHT_SLEEP 锁。必须在静音之后，否则最后一个音节会被截断。
        if (self->audio_) self->audio_->set_want_tx(false);
        // One line per finished stream: underruns (speaker_underruns.hpp), then
        // what played and what overflowed. ~1.00x means it played in real time.
        const auto t = gaps.finish();
        const int64_t elapsed = stream_t0 ? esp_timer_get_time() / 1000 - stream_t0 : 0;
        ESP_LOGI(TAG, "playback ended: %u underruns, %u ms in total, longest %u ms; %llu B played, %u B dropped, "
                 "in %lld ms (%.2fx real time)",
                 static_cast<unsigned>(t.count), static_cast<unsigned>(t.total_ms), static_cast<unsigned>(t.longest_ms),
                 static_cast<unsigned long long>(stream_bytes), static_cast<unsigned>(self->drop_bytes_.exchange(0)),
                 static_cast<long long>(elapsed),
                 elapsed > 0 ? static_cast<double>(stream_bytes) / elapsed /
                     (CodecAudio::kRate * sizeof(int16_t) / 1000.0) : 0.0);
        stream_t0 = 0;
        stream_bytes = 0;
        playing = false;
      } else if (!self->open_ && self->draining_) {
        // Closed with nothing played (its audio was dropped, or none came): nothing
        // is left to drain, so busy() must not stay true until the next stream.
        self->draining_ = false;
      }
      // Idle on a light-sleep board: wait for write()/abort() instead of waking
      // every 20 ms, so a doze can light-sleep. Draining, and other boards, keep
      // the short poll.
      if (pa) {
        if (self->can_power_down_ && !playing) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        else vTaskDelay(pdMS_TO_TICKS(20));
      }
      continue;
    }
    gaps.resumed(speaker_now_ms());
    if (!playing) {
      if (pa) {
        if (!pa->enable(generation)) continue;
        vTaskDelay(pdMS_TO_TICKS(20));  // PA settles while the DAC is still muted
      }
      // HG 本地改动：真正有音频要放了才打开 tx 通道。放在 mute 之前，
      // 这样 I2S 一开始出时钟，ES8311 就已经准备好了。
      if (self->audio_) self->audio_->set_want_tx(true);
      esp_codec_dev_set_out_mute(self->dev_, false);
      playing = true;
    } else if (pa && !pa->enable(generation)) {
      continue;  // a chunk taken before an abort(): drop it rather than play it
    }
    if (stream_t0 == 0) stream_t0 = esp_timer_get_time() / 1000;  // the stream-end log's clock starts at the first frame played
    if (self->stereo32_) {
      const size_t frames = got / sizeof(int16_t);
      hg::ws185_stereo32(chunk, stereo, frames);
      esp_codec_dev_write(self->dev_, stereo, static_cast<int>(frames * 2 * sizeof(int32_t)));
      stream_bytes += frames * sizeof(int16_t);  // the stream's own bytes, not the 4x wider I2S frames
    } else {
      size_t wr = got & ~static_cast<size_t>(1);
      esp_codec_dev_write(self->dev_, chunk, static_cast<int>(wr));
      stream_bytes += wr;
    }
  }
}

}  // namespace hgp
