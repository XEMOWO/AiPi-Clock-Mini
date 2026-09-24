/**
 * @file ir_ws2812.c
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-07-22
 *
 * @copyright Ai-Thinker co.,ltd (c) 2025
 *
 */
#include "ws2812.h"
#include "math.h"
#include "FreeRTOS.h"
#include "task.h"
#include "blog.h"

#if defined WS281X_IR_MODE
#pragma message "ws2812 using IR mode"

#include "ws281x_ir.h"
#define _WS281X_FUNC_DEFINE(_func, ...) ws281x_ir_##_func(__VA_ARGS__)
#elif defined WS281X_SPI_MODE
#pragma message "ws2812 using SPI mode"

#include "ws281x_spi.h"
#define _WS281X_FUNC_DEFINE(_func, ...) ws281x_spi_##_func(__VA_ARGS__)

#else
#error "Please select a ws281x mode,CONFIG_WS2812_MODE=SPI_MODE or IR_MODE"

#endif

ws2812_strip_t *ws2812_strip_dev = NULL;
void ws2812_init(ws2812_strip_t *ws2812_strip)
{
	if (ws2812_strip == NULL || ws2812_strip->led_count == 0)
	{
		blog_error("ws2812_strip is NULL or led_count is 0");
		return;
	}
	_WS281X_FUNC_DEFINE(init, ws2812_strip);

	if (ws2812_strip->dev == NULL)
	{
		ws2812_strip->dev = pvPortMalloc(sizeof(ws2812_dev_t) * ws2812_strip->led_count);
		ws2812_strip->brightness = 0.5;
		for (uint8_t i = 0; i < ws2812_strip->led_count; i++)
		{
			ws2812_strip->dev[i].index = i;
			ws2812_strip->dev[i].brightness = &ws2812_strip->brightness;
			ws2812_strip->dev[i].color.r = 0;
			ws2812_strip->dev[i].color.g = 0;
			ws2812_strip->dev[i].color.b = 0;
		}
	}

	if (ws2812_strip_dev == NULL)
	{
		ws2812_strip_dev = ws2812_strip;
	}
}
/**
 * @brief 清空所有LED
 *
 */
void ws2812_show_leds(void)
{
	_WS281X_FUNC_DEFINE(show_leds);
}
/**
 * @brief 设置单个LED的颜色
 *
 * @param index
 * @param r
 * @param g
 * @param b
 */
void ws2812_set_pixel_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	_WS281X_FUNC_DEFINE(set_pixel_color, index, r, g, b);
}
/**
 * @brief 设置单个LED的亮度
 *
 * @param index
 * @param brightness
 */
void ws2812_set_pixel_brightness(uint8_t index, float brightness)
{
	// 更新所有LED以应用新亮度
	hsv_color_t hsv = rgb_to_hsv(ws2812_strip_dev->dev[index].color);
	// 使用原始颜色的HSV，仅调整亮度（强制最大值以保持颜色）
	hsv.v = brightness;
	// hsv.s = ;
	color_t rgb = hsv_to_rgb(hsv);
	ws2812_set_pixel_color(index, rgb.r, rgb.g, rgb.b);
	// ws2812_show_leds();
}
void ws2812_set_all_pixels_color(uint8_t r, uint8_t g, uint8_t b, float brightness)
{
	color_t rgb = {r, g, b};
	for (uint8_t i = 0; i < ws2812_strip_dev->led_count; i++)
	{
		rgb.r = r;
		rgb.g = g;
		rgb.b = b;
		hsv_color_t hsv = rgb_to_hsv(rgb);
		// 使用原始颜色的HSV，仅调整亮度（强制最大值以保持颜色）
		hsv.v = brightness;
		rgb = hsv_to_rgb(hsv);
		ws2812_set_pixel_color(i, rgb.r, rgb.g, rgb.b);
	}
	ws2812_show_leds();
}

// 设置全局亮度（改进低亮度处理）
void ws2812_set_global_brightness(float brightness)
{
	// 更新所有LED以应用新亮度
	ws2812_strip_dev->brightness = brightness;
	for (uint8_t i = 0; i < ws2812_strip_dev->led_count; i++)
	{
		hsv_color_t hsv = rgb_to_hsv(ws2812_strip_dev->dev[i].color);
		// 使用原始颜色的HSV，仅调整亮度（强制最大值以保持颜色）
		hsv.v = ws2812_strip_dev->brightness;
		color_t rgb = hsv_to_rgb(hsv);
		ws2812_set_pixel_color(i, rgb.r, rgb.g, rgb.b);
	}
	ws2812_show_leds();
}
void ws2812_set_pixel_color_hsv(uint8_t index, uint8_t h, uint8_t s, uint8_t v)
{
	hsv_color_t hsv = {h, s, v};
	color_t rgb = hsv_to_rgb(hsv);
	ws2812_set_pixel_color(index, rgb.r, rgb.g, rgb.b);
}

// 设置灯珠数量
void ws2812_set_led_count(uint8_t count)
{
	ws2812_strip_dev->led_count = count;
}

uint8_t ws2812_get_led_count(void)
{
	return ws2812_strip_dev->led_count;
}