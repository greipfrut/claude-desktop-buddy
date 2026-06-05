#pragma once
#include <Arduino.h>

// Initialize ES8311 codec + I2S + speaker amp, and spawn the audio task.
// Safe to call once from setup() after displayInit(). On failure the module
// logs to serial and beep() becomes a no-op (audio absence never crashes).
void audioInit();

// Enqueue a tone of `freq` Hz for `dur` ms. Non-blocking: returns immediately,
// the audio task plays it. Silent when settings().volume == 0. Drops the tone
// if the queue is full (never blocks the render loop).
void beep(uint16_t freq, uint16_t dur);
