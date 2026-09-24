/**
 * @file ws281x_spi.c
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-08-01
 *
 * @copyright Ai-Thinker co.,ltd (c) 2025
 *
 */
#include "ws281x_spi.h"

#ifdef WS281X_SPI_MODE
#include "hosal_spi.h"
#include "bl602_spi.h"
#include "bl_irq.h"
#include "bl_dma.h"
#include "bl602_dma.h"
#include "hosal_dma.h"
#include "FreeRTOS.h"
#include "bl602_glb.h"
#include "blog.h"
#include "bl602_gpio.h"

#define LED_T0 0xC0
#define LED_T1 0xFC

#define DEMO_SPI_ID 0
#define DEMO_SPI_CLK_HZ 6666666
#define DEMO_DMA_LLI_CNT 2
static uint8_t *led_buffer = NULL;
static volatile int spi_dma_txing;
static hosal_dma_chan_t spi_dma_chan;
static DMA_LLI_Ctrl_Type spi_dma_lli[DEMO_DMA_LLI_CNT];
/**
 * @brief SPI DMA发送数据
 *
 * @param arg
 * @param flag
 */
static void ws281x_spi_dma_int_handler(void *arg, uint32_t flag)
{
	// bl_dma_int_clear(spi_dma_chan);
	// puts("spi_dma_int_handler\r\n");

	spi_dma_txing = 0;

	return;
}
/**
 * @brief SPI DMA发送数据
 *
 */
static void inline ws281x_spi_dma_whait_txdone(void)
{
	while (spi_dma_txing)
	{
		blog_warn("spi_dma_txing\r\n");
		;
	}
}
/**
 * @brief SPI DMA发送数据
 *
 * @param data
 * @param length
 * @return int
 */
static int ws281x_spi_dma_lli_list_init(uint8_t *data, uint32_t length)
{
	struct DMA_Control_Reg dmactrl;

	dmactrl.SBSize = DMA_BURST_SIZE_1;
	dmactrl.DBSize = DMA_BURST_SIZE_1;
	dmactrl.SWidth = DMA_TRNS_WIDTH_8BITS;
	dmactrl.DWidth = DMA_TRNS_WIDTH_8BITS;
	dmactrl.Prot = 0;
	dmactrl.SLargerD = 0;

	dmactrl.TransferSize = length;
	dmactrl.I = 0;

	dmactrl.SI = DMA_MINC_ENABLE;
	dmactrl.DI = DMA_MINC_DISABLE;

	spi_dma_lli[0].srcDmaAddr = (uint32_t)(data);
	spi_dma_lli[0].destDmaAddr = (uint32_t)(SPI_BASE + SPI_FIFO_WDATA_OFFSET);
	spi_dma_lli[0].dmaCtrl = dmactrl;
	spi_dma_lli[0].nextLLI = 0;

	return 0;
}
/**
 * @brief SPI DMA发送数据
 *
 * @param data
 * @param length
 * @return int
 */
static int ws281x_spi_send_by_dma(uint8_t *data, uint32_t length)
{
	DMA_LLI_Cfg_Type txllicfg;

	// if (length > LLI_BUFF_SIZE)
	//     assert(0);

	txllicfg.dir = DMA_TRNS_M2P;
	txllicfg.srcPeriph = DMA_REQ_NONE;
	txllicfg.dstPeriph = DMA_REQ_SPI_TX;

	ws281x_spi_dma_lli_list_init(data, length);

	DMA_LLI_Init(spi_dma_chan, &txllicfg);
	DMA_LLI_Update(spi_dma_chan, (uint32_t)spi_dma_lli);
	hosal_dma_irq_callback_set(spi_dma_chan, ws281x_spi_dma_int_handler, NULL);
	hosal_dma_chan_start(spi_dma_chan);

	return 0;
}
static void ws281x_spi_gpio_init(uint8_t pin)
{
	GLB_GPIO_Func_Init(GPIO_FUN_SPI, (GLB_GPIO_Type *)&pin, 1);
	GLB_Set_SPI_0_ACT_MOD_Sel(GLB_SPI_PAD_ACT_AS_MASTER);
}
void ws281x_spi_init(ws2812_strip_t *ws2812_strip)
{
	ws281x_spi_gpio_init(ws2812_strip->pin);

	SPI_CFG_Type spiCfg = {
		DISABLE,					  /* De-glitch function */
		ENABLE,						  /* Master continuous transfer mode */
		SPI_BYTE_INVERSE_BYTE0_FIRST, /* The byte 0 is sent first in SPI transfer */
		SPI_BIT_INVERSE_MSB_FIRST,	  /* MSB is sent first in SPI transfer */
		SPI_CLK_PHASE_INVERSE_0,	  /* SPI clock phase */
		SPI_CLK_POLARITY_LOW,		  /* SPI clock plarity */
		SPI_FRAME_SIZE_8};

	SPI_FifoCfg_Type fifoCfg = {
		1,		/* SPI tx FIFO threshold */
		0,		/* SPI rx FIFO threshold */
		ENABLE, /* Enable or disable tx dma req/ack interface */
		DISABLE /* Enable or disable rx dma req/ack interface */
	};

	spi_dma_chan = hosal_dma_chan_request(0);

	SPI_Disable(DEMO_SPI_ID, SPI_WORK_MODE_MASTER);
	// SPI_IntMask(DEMO_SPI_ID, SPI_INT_ALL, MASK);
	SPI_Init(DEMO_SPI_ID, &spiCfg);
	SPI_FifoConfig(DEMO_SPI_ID, &fifoCfg);
	SPI_SetClock(DEMO_SPI_ID, DEMO_SPI_CLK_HZ);
	SPI_Enable(DEMO_SPI_ID, SPI_WORK_MODE_MASTER);

	led_buffer = pvPortMalloc(ws2812_strip->led_count * 3 * 8);

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
	blog_info("ws2812 SPI Mode init success, led_count:%d, pin:%d", ws2812_strip->led_count, ws2812_strip->pin);
}

/**
 * @brief SPI发送数据
 *
 * @param index
 * @param r
 * @param g
 * @param b
 */
static void _ws281x_spi_set_pixel_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	// 增加越界检查
	if (index >= ws2812_strip_dev->led_count)
	{
		blog_error("LED index %d out of range (max %d)", index, ws2812_strip_dev->led_count);
		return;
	}

	uint32_t base = index * 24; // 每个灯珠24字节（3色×8位）
	// 确保不越界（base + 23 < 总长度）
	if (base + 23 >= ws2812_strip_dev->led_count * 24)
	{
		blog_error("LED buffer overflow for index %d", index);
		return;
	}
	// 颜色顺序: B G R (适配 Ai-WB2 板载 WS2812)
	for (int32_t i = 0; i < 8; i++)
	{
		led_buffer[base + i] = ((b >> (7 - i)) & 0x01) ? LED_T1 : LED_T0;
	}
	for (int32_t i = 0; i < 8; i++)
	{
		led_buffer[base + 8 + i] = ((g >> (7 - i)) & 0x01) ? LED_T1 : LED_T0;
	}
	for (int32_t i = 0; i < 8; i++)
	{
		led_buffer[base + 16 + i] = ((r >> (7 - i)) & 0x01) ? LED_T1 : LED_T0;
	}
}
/**
 * @brief
 *
 */
void ws281x_spi_sync_all_leds(void)
{
	if (ws2812_strip_dev == NULL || led_buffer == NULL)
		return;
	// printf("RGB:");
	for (uint8_t i = 0; i < ws2812_strip_dev->led_count; i++)
	{
		// 从dev数组同步颜色到led_buffer
		uint8_t r = ws2812_strip_dev->dev[i].color.r;
		uint8_t g = ws2812_strip_dev->dev[i].color.g;
		uint8_t b = ws2812_strip_dev->dev[i].color.b;

		// 复用现有函数更新缓冲区
		// printf(" %02X ", r << 8 | g << 16 | b);
		_ws281x_spi_set_pixel_color(i, r, g, b);
	}
	// printf("\r\n");
}
/**
 * @brief 设置单个灯珠颜色
 *
 * @param index
 * @param r
 * @param g
 * @param b
 */
void ws281x_spi_set_pixel_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
	if (index >= ws2812_strip_dev->led_count)
		return;
	// 1. 更新dev数组中的颜色状态
	ws2812_strip_dev->dev[index].color.r = r;
	ws2812_strip_dev->dev[index].color.g = g;
	ws2812_strip_dev->dev[index].color.b = b;
}
/**
 * @brief 从dev数组同步所有灯珠颜色到led_buffer
 */

void ws281x_spi_show_leds(void)
{
	ws281x_spi_sync_all_leds();
	ws281x_spi_dma_whait_txdone();
	// blog_warn_hexdump("led_buffer", led_buffer, ws2812_strip_dev->led_count * 24);
	if (led_buffer != NULL)
		ws281x_spi_send_by_dma(led_buffer, ws2812_strip_dev->led_count * 24);
}

#endif // !__WS281X_IR_H__