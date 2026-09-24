/**
 * @file ws281x_ir.h
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-08-01
 *
 * @copyright Ai-Thinker co.,ltd (c) 2025
 *
 */
#ifndef __WS281X_IR_H__
#define __WS281X_IR_H__
#include "stdio.h"
#include "stdint.h"
#include "ws2812.h"
#ifdef WS281X_IR_MODE
#include <bl602_glb.h>
#include <bl602_gpio.h>
#include <bl602_ir.h>
#define IR_PIN_TX GLB_GPIO_PIN_11
void ws281x_ir_init(ws2812_strip_t *ws2812_strip);
void ws281x_ir_set_pixel_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void ws281x_ir_show_leds(void);
#endif // !__WS281X_IR_H__
#endif // !__WS281X_IR_H__