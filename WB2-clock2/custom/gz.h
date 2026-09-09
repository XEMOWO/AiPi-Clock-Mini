/**
 * @file gz.h
 * @brief 极简 gzip/DEFLATE 解压(和风天气响应强制 gzip, 固件需自行解压)
 */
#ifndef GZ_H
#define GZ_H

#include <stdint.h>

/**
 * @brief 解压一段 gzip 数据到 out
 * @param in     gzip 压缩数据
 * @param inlen  压缩数据长度
 * @param out    输出缓冲
 * @param outlen 输出缓冲容量
 * @return 解压后字节数; <0 出错(输入非法/输出空间不足)
 */
int gz_inflate(const uint8_t *in, int inlen, uint8_t *out, int outlen);

#endif /* GZ_H */
