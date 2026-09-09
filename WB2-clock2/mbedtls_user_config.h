/**
 * @file mbedtls_user_config.h — AiPi-Clock-Mini 项目级 mbedtls 覆盖
 *
 * 通过顶层 Makefile 的 EXTRA_CFLAGS 传入
 *   -D MBEDTLS_USER_CONFIG_FILE="mbedtls_user_config.h"
 * 在 mbedtls_sample_config.h 末尾 (USER_CONFIG_FILE 段) 生效。
 *
 * 背景: mbedtls 每个 SSL 连接默认分配 in_buf+out_buf 各 16384B(共 32KB),
 * 而 WiFi 等模块占用后 FreeRTOS 堆 (heap_5, linker 全 RAM) 只剩 ~14KB,
 * mbedtls_ssl_setup 分配失败导致和风 HTTPS 请求失败。
 */
#ifndef MBEDTLS_USER_CONFIG_H
#define MBEDTLS_USER_CONFIG_H

/* 4096 -> 8192: 和风 geo/now 把整条证书链 (4 条 DER ~6.9KB) 放在一条
 * TLS record 里下发, ssl_msg.c:1897 对 record 明文长度 > IN_CONTENT_LEN
 * 直接报 -0x7100 BAD_INPUT_DATA。8192 可容纳整条证书消息。
 * OUT 1024 足够: 客户端消息 (ClientHello 192B / 证书请求 <300B)。 */
#define MBEDTLS_SSL_IN_CONTENT_LEN  8192
#define MBEDTLS_SSL_OUT_CONTENT_LEN 1024

/* 关键: 关掉 KEEP_PEER_CERTIFICATE, ssl_tls.c 证书链解析走
 * mbedtls_x509_crt_parse_der_nocopy() 零拷贝分支 —— raw.p 直接指向
 * in_buf, 不再每张证书 calloc ~1.7KB 拷贝 DER。握手期间 in_buf 持有
 * 证书消息期间完成全部解析, 之后 (VERIFY_NONE, 无 renegotiation, 无
 * session resumption) 不再访问 peer_cert, own_buffer=0 也保证
 * mbedtls_x509_crt_free 不会释放 in_buf。x509 峰值 8.8KB -> 2KB。 */
#undef MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

#endif /* MBEDTLS_USER_CONFIG_H */
