/**
 * @brief xcmd — 串口命令协议 (#X 行协议)
 *
 * 协议行以 "#X" 开头,与 printf 日志区分;值字段为 UTF-8 字节的 hex。
 * 命令: #XPING / #XVER / #XCFG,ssid,pwd,city / #XCITY,city / #XSTA
 *       / #XMON,0|1 / #XLAMP,0-4
 *       / #XWXAPI,hex(amap|qweather)           天气源切换
 *       / #XWXKEY,hex(provider),hex(key)[,hex(cred)]  天气 API Key 下发
 * 响应: #XA,OK|ERR,CMD[,hex data]; 事件: #XEV,EV[,data]
 */
#ifndef XCMD_H
#define XCMD_H

void xcmd_init(void);                     /* 打开 /dev/ttyS0 并注册读回调 */
void xcmd_send_event(const char *ev, const char *data); /* 推 #XEV 事件 */
const char *xcmd_ip_str(void);            /* 当前 STA IP 点分串(内部静态缓冲) */

#endif /* XCMD_H */
