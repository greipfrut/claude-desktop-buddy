#include "audio.h"
#include "display.h"   // expander (amp enable on XCA9554 pin 3)
#include "stats.h"     // settings().volume, settings().sineWave
#include "ESP_I2S.h"
#include "es8311.h"
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// I2S / codec pins (Waveshare 4B, from demo pin_config.h)
#define PIN_BCLK 16
#define PIN_LRCK 7
#define PIN_DOUT 6
#define PIN_DIN  15
#define PIN_MCLK 5
#define AMP_EN_PIN 3            // XCA9554 expander pin driving the speaker amp
#define SAMPLE_RATE 16000

struct ToneJob { uint16_t freq; uint16_t dur; };

static I2SClass     i2s;
static QueueHandle_t toneQ = nullptr;
static bool          audioReady = false;

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
  es8311_voice_volume_set(h, 90, NULL);   // fixed codec gain; loudness is set in synth
  return ESP_OK;
}

// Render `job` into I2S in small stereo chunks. Blocks this task (not the loop).
static void playTone(const ToneJob& job) {
  uint8_t vol = settings().volume;
  if (vol == 0 || vol > 4 || job.freq == 0) return;
  int16_t amp = AMP[vol];
  bool sine = settings().sineWave;
  uint32_t total = (uint32_t)SAMPLE_RATE * job.dur / 1000;   // samples
  uint32_t halfP = SAMPLE_RATE / (2u * job.freq);
  if (halfP == 0) halfP = 1;

  static int16_t buf[256 * 2];   // 256 stereo frames
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
    i2s.write((uint8_t*)buf, frames * 2 * sizeof(int16_t));
  }
  // Short silence tail so the amp settles and tones don't run together.
  memset(buf, 0, sizeof(buf));
  i2s.write((uint8_t*)buf, sizeof(buf));
}

static void audioTask(void*) {
  ToneJob job;
  for (;;) {
    if (xQueueReceive(toneQ, &job, portMAX_DELAY) == pdTRUE) playTone(job);
  }
}

void audioInit() {
  // Enable the speaker amplifier via the IO expander created in displayInit().
  if (expander) {
    expander->pinMode(AMP_EN_PIN, OUTPUT);
    expander->digitalWrite(AMP_EN_PIN, HIGH);
    delay(10);
  }
  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_DIN, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
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
  if (!audioReady || settings().volume == 0) return;
  ToneJob job = { freq, dur };
  xQueueSend(toneQ, &job, 0);   // zero timeout: drop if full, never block
}
