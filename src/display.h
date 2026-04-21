#pragma once
#include <Arduino.h>
#include <SPI.h>

#define LCD_WIDTH   240
#define LCD_HEIGHT  320

#define SPIFreq                        80000000
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_MOSI           45
#define EXAMPLE_PIN_NUM_SCLK           40
#define EXAMPLE_PIN_NUM_LCD_CS         42
#define EXAMPLE_PIN_NUM_LCD_DC         41
#define EXAMPLE_PIN_NUM_LCD_RST        39

#define LCD_Backlight_PIN   5
#define BL_PWM_CHANNEL      1
#define BL_PWM_FREQ         20000
#define BL_PWM_RES          10

#define PWR_KEY_Input_PIN   6
#define PWR_Control_PIN     7

extern uint8_t LCD_Backlight;

void PWR_Init(void);
void Backlight_Init(void);
void Set_Backlight(uint8_t Light);

void LCD_Init(void);
void LCD_SetCursor(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t* color);
