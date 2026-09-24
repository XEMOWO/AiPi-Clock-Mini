/**
 * @file color_mode.c
 * @author Seahi-Mo (seahi-mo@foxmail.com)
 * @brief
 * @version 0.1
 * @date 2025-07-10
 *
 * @copyright Copyright (c) 2025
 *
 */
#include "color_mode.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
/**
 * @brief 将HSV颜色转换为RGB颜色
 *
 * @param hsv
 * @return color_t_t
 */
color_t hsv_to_rgb(hsv_color_t hsv)
{
	float h = hsv.h;
	float s = hsv.s;
	float v = hsv.v;

	int hi = (int)(h / 60) % 6;
	float f = h / 60.0 - hi;

	float p = v * (1 - s);
	float q = v * (1 - f * s);
	float t = v * (1 - (1 - f) * s);

	color_t rgb;
	switch (hi)
	{
	case 0:
		rgb.r = v * 255;
		rgb.g = t * 255;
		rgb.b = p * 255;
		break;
	case 1:
		rgb.r = q * 255;
		rgb.g = v * 255;
		rgb.b = p * 255;
		break;
	case 2:
		rgb.r = p * 255;
		rgb.g = v * 255;
		rgb.b = t * 255;
		break;
	case 3:
		rgb.r = p * 255;
		rgb.g = q * 255;
		rgb.b = v * 255;
		break;
	case 4:
		rgb.r = t * 255;
		rgb.g = p * 255;
		rgb.b = v * 255;
		break;
	case 5:
		rgb.r = v * 255;
		rgb.g = p * 255;
		rgb.b = q * 255;
		break;
	}

	// 添加舍入处理，减少精度损失
	rgb.r = (uint8_t)(rgb.r + 0.5f);
	rgb.g = (uint8_t)(rgb.g + 0.5f);
	rgb.b = (uint8_t)(rgb.b + 0.5f);

	return rgb;
}
/**
 * @brief 将RGB颜色转换为HSV颜色
 *
 * @param rgb
 * @param hsv
 */
hsv_color_t rgb_to_hsv(color_t rgb)
{
	float r = rgb.r / 255.0f;
	float g = rgb.g / 255.0f;
	float b = rgb.b / 255.0f;

	float max = fmax(r, fmax(g, b));
	float min = fmin(r, fmin(g, b));
	float delta = max - min;
	hsv_color_t hsv;
	// 避免除零错误（虽然已经检查过delta）
	float divisor = delta > 0 ? delta : 1e-6f;

	if (delta == 0)
	{
		hsv.h = 0;
	}
	else if (max == r)
	{
		// 使用更稳定的公式，减少边界跳变
		hsv.h = 60 * (fmod((g - b) / divisor, 6));
		if (hsv.h < 0)
			hsv.h += 360;
	}
	else if (max == g)
	{
		hsv.h = 60 * ((b - r) / divisor + 2);
	}
	else
	{
		hsv.h = 60 * ((r - g) / divisor + 4);
	}

	hsv.s = (max == 0) ? 0 : (delta / max);
	hsv.v = max;

	return hsv;
}

// 计算两个颜色之间的插值（不使用 A 通道）
color_t interpolatecolor_t(color_t start, color_t end, float t)
{
	color_t result;
	// 确保 t 在 0 到 1 的范围内
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;

	// 使用线性插值计算每个通道的值
	result.r = (unsigned char)(start.r + (end.r - start.r) * t);
	result.g = (unsigned char)(start.g + (end.g - start.g) * t);
	result.b = (unsigned char)(start.b + (end.b - start.b) * t);

	return result;
}

// 1. 线性缓动（无加速）
static float easeLinear(float t)
{
	return t;
}

// 2. 二次方缓动（先慢后快）
static float easeInQuad(float t)
{
	return t * t;
}

// 3. 二次方缓动（先快后慢）
static float easeOutQuad(float t)
{
	return -t * (t - 2);
}

// 4. 二次方缓动（先慢中间快后慢）
static float easeInOutQuad(float t)
{
	if (t < 0.5f)
		return 2 * t * t;
	return -1 + (4 - 2 * t) * t;
}

// 5. 三次方缓动（快速加速）
static float easeInCubic(float t)
{
	return t * t * t;
}

// 6. 三次方缓动（快速减速）
static float easeOutCubic(float t)
{
	t--;
	return t * t * t + 1;
}

// 7. 弹性缓动（结束时有回弹效果）
static float easeOutElastic(float t)
{
	if (t == 0 || t == 1)
		return t;
	return pow(2, -10 * t) * sin((t * 10 - 0.75) * (2 * M_PI) / 3) + 1;
}

// 8. 弹跳缓动（结束时有弹跳效果）
float easeOutBounce(float t)
{
	if (t < 1 / 2.75)
	{
		return 7.5625 * t * t;
	}
	else if (t < 2 / 2.75)
	{
		t -= 1.5 / 2.75;
		return 7.5625 * t * t + 0.75;
	}
	else if (t < 2.5 / 2.75)
	{
		t -= 2.25 / 2.75;
		return 7.5625 * t * t + 0.9375;
	}
	else
	{
		t -= 2.625 / 2.75;
		return 7.5625 * t * t + 0.984375;
	}
}

// 实现平滑的颜色过渡
void smoothcolorTransition(color_t start, color_t end, int steps, void (*updateCallback)(color_t, void *), void *userData)
{
	if (updateCallback == NULL)
	{
		return;
	}
	for (int i = 0; i <= steps; i++)
	{
		// 计算当前的插值比例
		float t = (float)i / steps;

		// 应用缓动函数使过渡更平滑
		float easedT = easeInOutQuad(t); // 线性缓动，可替换为其他缓动函数

		// 计算当前颜色
		color_t current = interpolatecolor_t(start, end, easedT);
		// 调用回调函数更新颜色
		updateCallback(current, userData);
	}
}
