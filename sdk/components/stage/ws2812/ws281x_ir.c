/**
 * @file ws281x_ir.c
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-08-01
 *
 * @copyright Ai-Thinker co.,ltd (c) 2025
 *
 */
#include "ws281x_ir.h"
#include "blog.h"
#ifdef WS281X_IR_MODE
/**
 * @brief ws2812_strip_dev
 *
 */
static void ws281x_ir_gpio_init(void)
{

	GLB_GPIO_Type ir_pin = IR_PIN_TX;

	GLB_GPIO_Func_Init(GPIO_FUN_ANALOG, &ir_pin, 1);
	GLB_IR_LED_Driver_Enable();
}
/**
 * @brief ws2812_strip初始化
 *
 * @param ws2812_strip
 */
void ws281x_ir_init(ws2812_strip_t *ws2812_strip)
{

	if (ws2812_strip == NULL || ws2812_strip->led_count == 0)
	{
		blog_error("ws2812_strip is NULL or led_count is 0");
		return;
	}
	ws281x_ir_gpio_init();
	IR_LEDInit(HBN_XCLK_CLK_RC32M, 1, 2, 8, 16, 16, 4);
	vTaskDelay(pdMS_TO_TICKS(1)); // 等待初始化完成
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
	ws2812_strip_dev = ws2812_strip;
	blog_info("ws2812 IR Mode init success");
}
/**
 * @brief 反转8位数据的位顺序,通过红外发送时需要将数据反转
 *
 * @param high_data
 * @return uint8_t
 */
static uint8_t reverse_bits(uint8_t high_data)
{
	uint8_t low_data = 0;
	for (uint8_t i = 0; i < 8; i++)
	{
		low_data <<= 1;				 // 低位数据左移，腾出最低位
		low_data |= (high_data & 1); // 取高位数据的最低位，放到低位数据的最低位
		high_data >>= 1;			 // 高位数据右移，处理下一位
	}
	return low_data;
}
/**
 * @brief 设置指定LED的颜色
 *
 * @param index
 * @param r
 * @param g
 * @param b
 */
void ws281x_ir_set_pixel_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	if (index < ws2812_strip_dev->led_count)
	{
		ws2812_strip_dev->dev[index].color.r = (uint16_t)reverse_bits(r);
		ws2812_strip_dev->dev[index].color.g = (uint16_t)reverse_bits(g);
		ws2812_strip_dev->dev[index].color.b = (uint16_t)reverse_bits(b);
	}
}
/**
 * @brief
 *
 */
void ws281x_ir_show_leds(void)
{
	for (uint8_t i = 0; i < ws2812_strip_dev->led_count; i++)
	{
		uint32_t brg_color = (ws2812_strip_dev->dev[i].color.b << 16) | (ws2812_strip_dev->dev[i].color.r << 8) | ws2812_strip_dev->dev[i].color.g;
		IR_SendCommand(0, brg_color);
	}
	vTaskDelay(pdMS_TO_TICKS(1));
}

#endif
