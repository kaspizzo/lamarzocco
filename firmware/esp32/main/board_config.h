#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

/** Display geometry and pixel format for the round controller panel. */
#define LM_CTRL_LCD_H_RES 360
#define LM_CTRL_LCD_V_RES 360
#define LM_CTRL_LCD_BPP 16
#define LM_CTRL_LCD_HOST SPI2_HOST
#define LM_CTRL_LCD_DRAW_BUF_ROWS 20
#define LM_CTRL_LCD_SPI_MAX_TRANSFER_SZ (LM_CTRL_LCD_H_RES * LM_CTRL_LCD_DRAW_BUF_ROWS * LM_CTRL_LCD_BPP / 8)

/** ST77916 display bus and control pins. */
#define LM_CTRL_LCD_SCL GPIO_NUM_11
#define LM_CTRL_LCD_CS GPIO_NUM_12
#define LM_CTRL_LCD_D0 GPIO_NUM_13
#define LM_CTRL_LCD_D1 GPIO_NUM_14
#define LM_CTRL_LCD_D2 GPIO_NUM_15
#define LM_CTRL_LCD_D3 GPIO_NUM_16
#define LM_CTRL_LCD_RST GPIO_NUM_17
#define LM_CTRL_LCD_TE GPIO_NUM_18
#define LM_CTRL_LCD_BL GPIO_NUM_21

/** CST816S touch controller I2C bus and GPIO assignments. */
#define LM_CTRL_TOUCH_HOST I2C_NUM_0
#define LM_CTRL_TOUCH_SDA GPIO_NUM_9
#define LM_CTRL_TOUCH_SCL GPIO_NUM_10
#define LM_CTRL_TOUCH_INT GPIO_NUM_7
#define LM_CTRL_TOUCH_RST GPIO_NUM_8

/** Physical outer ring encoder pins and direction correction. */
#define LM_CTRL_KNOB_A GPIO_NUM_2
#define LM_CTRL_KNOB_B GPIO_NUM_1
#define LM_CTRL_KNOB_DIRECTION (-1)
#define LM_CTRL_ENCODER_ENABLED 1

/** Optional local battery telemetry exposed by the controller board. */
#define LM_CTRL_BATTERY_ADC_GPIO GPIO_NUM_6
/* JC3636K718 v1.3 routes ETA6003 STAT only to the charge LED, not to the ESP32. */
#define LM_CTRL_BATTERY_CHARGE_GPIO GPIO_NUM_NC
#define LM_CTRL_BATTERY_CHARGE_ACTIVE_LEVEL 0
#define LM_CTRL_BATTERY_SAMPLE_INTERVAL_MS 30000UL
#define LM_CTRL_BATTERY_VOLTAGE_DIVIDER 3.0f
#define LM_CTRL_BATTERY_LOW_PERCENT 20

/** WS2812 LED ring on the controller face. */
#define LM_CTRL_LED_RING_GPIO GPIO_NUM_0
#define LM_CTRL_LED_RING_COUNT 13
