#include "audio.h"
#include "display.h"   // expander (amp enable on XCA9554 pin 3)
#include "es8311.h"
// NOTE: deliberately do NOT include stats.h — it is file-static, single-TU
// state. Read volume/waveform via audioVolume()/audioSineWave() (audio.h),
// which main.cpp backs with the one true Settings instance.
#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

// I2S / codec pins (Waveshare 4B, from demo pin_config.h)
#define PIN_BCLK 16
#define PIN_LRCK 7
#define PIN_DOUT 6
#define PIN_DIN  15
#define PIN_MCLK 5
#define AMP_EN_PIN 3            // XCA9554 expander pin driving the speaker amp
#define SAMPLE_RATE 16000

struct ToneJob { uint16_t freq; uint16_t dur; };

static i2s_chan_handle_t txChan     = nullptr;
static QueueHandle_t     toneQ      = nullptr;
static bool              audioReady = false;

// Per-volume-level peak amplitude (fraction of int16 full-scale).
static const int16_t AMP[5] = { 0, 4915, 11468, 19660, 32767 };  // 0,.15,.35,.6,1.0

static esp_err_t es8311_codec_init() {
  es8311_handle_t h = es8311_create(0, ES8311_ADDRRES_0);
  if (!h) return ESP_FAIL;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256,
    .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return ESP_FAIL;
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);
  es8311_voice_volume_set(h, 65, NULL);   // fixed codec gain; per-level loudness is set in synth (AMP[])
  // Explicitly UNMUTE the DAC. The ES8311 is a separate chip whose registers
  // survive an ESP32 reset/reflash, and es8311_init does not clear the mute
  // bit — so a prior firmware that muted the DAC would otherwise leave it muted
  // (silent) across every subsequent flash until a full power cycle. Always
  // forcing unmute here keeps codec state deterministic regardless of history.
  es8311_voice_mute(h, false);
  return ESP_OK;
}

// Bring up the I2S TX channel in STD (Philips) mode on the codec pins. Replaces
// the ESP_I2S Arduino wrapper, which SELECTIVE_COMPILATION drops. MCLK runs at
// 256x sample rate (I2S_STD_CLK_DEFAULT_CONFIG default), matching the ES8311.
static bool i2sSetup() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Auto-clear the DMA buffer when idle. Without this the TX DMA underruns by
  // REPEATING its last buffer, so the tail of the last tone loops forever — a
  // continuous hardware drone the beep()/volume gate can't silence (it isn't a
  // beep). With auto_clear the channel emits zeros (silence) whenever the audio
  // task isn't actively writing.
  chanCfg.auto_clear = true;
  if (i2s_new_channel(&chanCfg, &txChan, NULL) != ESP_OK) return false;

  // clk_cfg is set by member assignment, not the I2S_STD_CLK_DEFAULT_CONFIG
  // macro: on HW_VERSION_2 SoCs (ESP32-S3) that macro lists its designators out
  // of struct order (ext_clk_freq_hz vs mclk_multiple), which C++ rejects.
  // MCLK = 256x sample rate matches the ES8311; ext_clk_freq_hz stays 0.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
  stdCfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
  stdCfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = (gpio_num_t)PIN_MCLK;
  stdCfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
  stdCfg.gpio_cfg.ws   = (gpio_num_t)PIN_LRCK;
  stdCfg.gpio_cfg.dout = (gpio_num_t)PIN_DOUT;
  stdCfg.gpio_cfg.din  = (gpio_num_t)PIN_DIN;
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv   = false;

  if (i2s_channel_init_std_mode(txChan, &stdCfg) != ESP_OK) return false;
  if (i2s_channel_enable(txChan) != ESP_OK) return false;
  return true;
}

// Render `job` into I2S in small stereo chunks. Blocks this task (not the loop).
static void playTone(const ToneJob& job) {
  uint8_t vol = audioVolume();
  if (vol == 0 || vol > 4 || job.freq == 0) return;
  int16_t amp = AMP[vol];
  bool sine = audioSineWave();
  uint32_t total = (uint32_t)SAMPLE_RATE * job.dur / 1000;   // samples
  uint32_t halfP = SAMPLE_RATE / (2u * job.freq);
  if (halfP == 0) halfP = 1;

  static int16_t buf[256 * 2];   // 256 stereo frames
  size_t written;
  uint32_t n = 0;
  while (n < total) {
    int frames = 0;
    while (frames < 256 && n < total) {
      int16_t v;
      if (sine) v = (int16_t)(amp * sinf(2.0f * (float)M_PI * job.freq * n / SAMPLE_RATE));
      else      v = ((n / halfP) & 1) ? amp : (int16_t)-amp;
      buf[frames * 2]     = v;   // L
      buf[frames * 2 + 1] = v;   // R
      frames++; n++;
    }
    i2s_channel_write(txChan, buf, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  }
  // Short silence tail so the amp settles and tones don't run together.
  memset(buf, 0, sizeof(buf));
  i2s_channel_write(txChan, buf, sizeof(buf), &written, portMAX_DELAY);
}

static void audioTask(void*) {
  ToneJob job;
  for (;;) {
    if (xQueueReceive(toneQ, &job, portMAX_DELAY) == pdTRUE) playTone(job);
  }
}

// The speaker amp is left powered on for the device's lifetime (enabled in
// audioInit). Two approaches to silence its faint idle hiss were tried and
// abandoned: (1) gating the amp-enable pin per-tone — this board's amp has a
// slow turn-on/pop-suppression mute that outlasts a short beep, so toggling it
// swallowed the tones; (2) muting the ES8311 DAC when idle — no effect, which
// proves the hiss is the amp's own analog noise floor, not codec noise. The
// residual hiss is output-side only (the mic codec is disabled) and audible
// only with an ear at the speaker, so we accept it and keep the path simple.

void audioInit() {
  // Enable the speaker amplifier via the IO expander and leave it on (see the
  // note above audioMaintain on why we don't gate it per-tone).
  if (expander) {
    expander->pinMode(AMP_EN_PIN, OUTPUT);
    expander->digitalWrite(AMP_EN_PIN, HIGH);
    delay(10);
  }
  if (!i2sSetup()) {
    Serial.println("[audio] I2S init failed");
    return;
  }
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("[audio] ES8311 init failed");
    return;
  }
  toneQ = xQueueCreate(4, sizeof(ToneJob));
  if (!toneQ) { Serial.println("[audio] queue alloc failed"); return; }
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 1);
  audioReady = true;
  Serial.println("[audio] ready");
}

void beep(uint16_t freq, uint16_t dur) {
  if (!audioReady || audioVolume() == 0) return;
  ToneJob job = { freq, dur };
  xQueueSend(toneQ, &job, 0);   // zero timeout: drop if full, never block
}
