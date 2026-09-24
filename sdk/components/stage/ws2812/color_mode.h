/**
 * @file color_mode.h
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-07-10
 *
 * @copyright Copyright (c) 2025
 *
 */

#ifndef COLOR_MODE_H
#define COLOR_MODE_H
#include <stdint.h>
typedef struct
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
} color_t;

typedef struct
{
	float h; // 色相 (0-360)
	float s; // 饱和度 (0-1)
	float v; // 亮度 (0-1)
} hsv_color_t;
/**
 * @brief 将HSV颜色转换为RGB颜色
 *
 * @param hsv
 * @return color_t
 */
color_t hsv_to_rgb(hsv_color_t hsv);
/**
 * @brief 将RGB颜色转换为HSV颜色
 *
 * @param rgb
 * @return hsv_color_t
 */
hsv_color_t rgb_to_hsv(color_t rgb);
/**
 * @brief 颜色渐变模式
 *
 * @param start 起始颜色
 * @param end 结束颜色
 * @param steps 渐变步数
 * @param updateCallback 更新回调函数
 * @param userData 用户数据
 */
void smoothcolorTransition(color_t start, color_t end, int steps, void (*updateCallback)(color_t, void *), void *userData);
/**
 * @brief 生成360度渐变颜色
 *
 * @param steps
 * @param brightness
 * @param callback
 * @param data
 */
void generate360Gradient(int steps, float brightness, void (*callback)(color_t, int, void *), void *data);
/**
 * @brief 颜色轮模式
 *
 * @param speed  速度
 * @param updateColorOutput  更新颜色输出函数
 * @param userData  用户数据
 */
void animateColorWheel(uint8_t speed, void (*updateColorOutput)(color_t, void *), void *userData);
#endif // COLOR_MODE_H