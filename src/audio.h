#pragma once
#include <Arduino.h>

// Initialize ES8311 codec + I2S + speaker amp, and spawn the audio task.
// Safe to call once from setup() after displayInit(). On failure the module
// logs to serial and beep() becomes a no-op (audio absence never crashes).
void audioInit();

// Enqueue a tone of `freq` Hz for `dur` ms. Non-blocking: returns immediately,
// the audio task plays it. Silent when the volume is 0 (off). Drops the tone
// if the queue is full (never blocks the render loop).
void beep(uint16_t freq, uint16_t dur);

// Current audio settings, provided by the translation unit that owns stats.h
// (main.cpp). audio.cpp must NOT include stats.h: that header holds file-static
// state, so a second includer gets a private copy that's never loaded from NVS
// nor updated by the settings UI (the audio task would read a frozen volume).
// These accessors route audio.cpp to the one true Settings instance.
uint8_t audioVolume();    // 0 = off, 1..4 = louder
bool    audioSineWave();  // false = square, true = sine
