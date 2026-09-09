/**
 * @file gz.c
 * @brief 极简 gzip/DEFLATE 解压实现
 *
 * 和风天气 API 自 2022-03 起强制 Gzip 压缩响应, 固件必须自行解压。
 * 本模块只实现解压所需的最小集: gzip 头解析 + 存储块/固定 Huffman/
 * 动态 Huffman + LZ77, 不校验 CRC(数据来自受信服务器, 省掉 1KB CRC 表)。
 * 算法与 Mark Adler 的 puff.c 同源思路; 已在主机端用真实载荷
 * (和风响应) + python zlib 生成的随机 gzip 数据全量对拍验证。
 */
#include <string.h>
#include "gz.h"

/* ---- LZ77 长度/距离基表 (RFC1951) ---- */

static const uint16_t lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577
};
static const uint8_t dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13
};
/* 码长编码的符号顺序 (RFC1951 §3.2.7) */
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

#define GZ_MAXBITS 15
#define GZ_MAX_SYM 320   /* 最大符号数: litlen 288 / dist 32 / clen 19 */

/* ---- 位流状态 ---- */

typedef struct {
    const uint8_t *in;   /* 剩余输入 */
    int inlen, inpos;
    uint32_t bitbuf;     /* 当前字节 */
    int bitcnt;          /* 剩余有效位 */
    uint8_t *out;
    int outlen, outpos;
} gz_t;

/* 读 1 位 (LSB first) */
static int gz_bit(gz_t *g, int *b)
{
    if (g->bitcnt == 0) {
        if (g->inpos >= g->inlen) return -1;
        g->bitbuf = g->in[g->inpos++];
        g->bitcnt = 8;
    }
    *b = (int)(g->bitbuf & 1);
    g->bitbuf >>= 1;
    g->bitcnt--;
    return 0;
}

/* 读 n 位整数值 (LSB first, 用于 LEN/extra bits 等数值字段) */
static int gz_bits(gz_t *g, int n, int *v)
{
    int x = 0;

    for (int i = 0; i < n; i++) {
        int b;
        if (gz_bit(g, &b)) return -1;
        x |= b << i;
    }
    *v = x;
    return 0;
}

/* 跳到字节边界 (存储块前) */
static void gz_align(gz_t *g)
{
    g->bitcnt = 0;
}

/* ---- Huffman 解码表 ---- */

typedef struct {
    short count[GZ_MAXBITS + 1]; /* 每个码长的符号数 */
    short symbol[GZ_MAX_SYM];    /* 按 (码长, 符号) 规范序排列 */
} huff_t;

/* 由码长序列建表; 0=成功, <0=码过长/过量订阅 */
static int huff_build(huff_t *h, const uint8_t *lens, int n)
{
    int left = 1, offs[GZ_MAXBITS + 1];

    memset(h, 0, sizeof(*h));
    for (int i = 0; i < n; i++) {
        if (lens[i] > GZ_MAXBITS) return -1;
        if (lens[i]) h->count[lens[i]]++;
    }
    for (int len = 1; len <= GZ_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;   /* 过量订阅 */
    }
    offs[1] = 0;
    for (int len = 1; len < GZ_MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = i;
    return 0;
}

/* 解码一个符号 (MSB first) */
static int huff_decode(gz_t *g, const huff_t *h, int *sym)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= GZ_MAXBITS; len++) {
        int b;
        if (gz_bit(g, &b)) return -1;
        code |= b;   /* 位拼进当前码字, 末尾统一左移 (规范码 MSB 优先) */
        if (code - h->count[len] < first) {
            *sym = h->symbol[index + code - first];
            return 0;
        }
        index += h->count[len];
        first += h->count[len];
        first <<= 1;
        code <<= 1;
    }
    return -1;   /* 无匹配码 */
}

/* ---- 解压核心 ---- */

static int inflate_codes(gz_t *g, const huff_t *lit, const huff_t *dist)
{
    for (;;) {
        int sym;
        if (huff_decode(g, lit, &sym)) return -1;
        if (sym < 256) {
            if (g->outpos >= g->outlen) return -1;
            g->out[g->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;   /* 块结束 */
        } else {
            int i = sym - 257, ds, d, len;
            if (i >= 29) return -1;
            if (gz_bits(g, lext[i], &len)) return -1;
            len += lbase[i];
            if (huff_decode(g, dist, &ds) || ds >= 30) return -1;
            if (gz_bits(g, dext[ds], &d)) return -1;
            d += dbase[ds];
            if (d > g->outpos) return -1;   /* 距离超过已输出 */
            for (int k = 0; k < len; k++) {
                if (g->outpos >= g->outlen) return -1;
                g->out[g->outpos] = g->out[g->outpos - d];  /* 重叠拷贝 */
                g->outpos++;
            }
        }
    }
}

/* 解一个(或连续多个) DEFLATE 块 */
static int inflate_block(gz_t *g)
{
    for (;;) {
        int bfinal, btype;
        if (gz_bit(g, &bfinal) || gz_bits(g, 2, &btype)) return -1;

        if (btype == 0) {   /* 存储块 */
            int len, nlen;
            gz_align(g);
            if (gz_bits(g, 16, &len) || gz_bits(g, 16, &nlen)) return -1;
            if ((len ^ 0xffff) != nlen) return -1;
            for (int i = 0; i < len; i++) {
                if (g->inpos >= g->inlen || g->outpos >= g->outlen) return -1;
                g->out[g->outpos++] = g->in[g->inpos++];
            }
        } else if (btype == 1) {   /* 固定 Huffman */
            uint8_t lits[288], dists[32];
            huff_t lh, dh;
            for (int i = 0; i < 144; i++) lits[i] = 8;
            for (int i = 144; i < 256; i++) lits[i] = 9;
            for (int i = 256; i < 280; i++) lits[i] = 7;
            for (int i = 280; i < 288; i++) lits[i] = 8;
            memset(dists, 5, sizeof(dists));
            if (huff_build(&lh, lits, 288) || huff_build(&dh, dists, 32)) return -1;
            if (inflate_codes(g, &lh, &dh)) return -1;
        } else if (btype == 2) {   /* 动态 Huffman */
            int hlit, hdist, hclen;
            uint8_t lens[19], lds[316];
            huff_t ch, lh, dh;
            int idx = 0, prev = 0;
            if (gz_bits(g, 5, &hlit) || gz_bits(g, 5, &hdist) ||
                gz_bits(g, 4, &hclen)) return -1;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            memset(lens, 0, sizeof(lens));
            for (int i = 0; i < hclen; i++) {
                int v;
                if (gz_bits(g, 3, &v)) return -1;
                lens[clen_order[i]] = (uint8_t)v;
            }
            if (huff_build(&ch, lens, 19)) return -1;
            while (idx < hlit + hdist) {   /* 读码长(含重复码) */
                int sym;
                if (huff_decode(g, &ch, &sym)) return -1;
                if (sym < 16) {
                    lds[idx++] = (uint8_t)sym;
                    prev = sym;
                } else if (sym == 16) {   /* 重复上一个码长 3~6 次 */
                    int v;
                    if (gz_bits(g, 2, &v)) return -1;
                    for (int k = 0; k < 3 + v; k++) {
                        if (idx >= 316) return -1;
                        lds[idx++] = (uint8_t)prev;
                    }
                } else if (sym == 17) {   /* 3~10 个零 */
                    int v;
                    if (gz_bits(g, 3, &v)) return -1;
                    for (int k = 0; k < 3 + v; k++) {
                        if (idx >= 316) return -1;
                        lds[idx++] = 0;
                    }
                    prev = 0;
                } else {   /* 18: 11~138 个零 */
                    int v;
                    if (gz_bits(g, 7, &v)) return -1;
                    for (int k = 0; k < 11 + v; k++) {
                        if (idx >= 316) return -1;
                        lds[idx++] = 0;
                    }
                    prev = 0;
                }
            }
            if (huff_build(&lh, lds, hlit) || huff_build(&dh, lds + hlit, hdist))
                return -1;
            if (inflate_codes(g, &lh, &dh)) return -1;
        } else {
            return -1;   /* btype==3 保留 */
        }
        if (bfinal) return 0;
    }
}

/* ---- 对外接口 ---- */

int gz_inflate(const uint8_t *in, int inlen, uint8_t *out, int outlen)
{
    gz_t g;
    int p = 10;
    uint8_t flg;

    if (inlen < 10 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return -1;                      /* 非 gzip (或方法非 deflate) */
    flg = in[3];
    if (flg & 0x04) {                   /* FEXTRA */
        if (p + 2 > inlen) return -1;
        p += 2 + (in[p] | (in[p + 1] << 8));
    }
    if (flg & 0x08) {                   /* FNAME */
        while (p < inlen && in[p]) p++;
        p++;
    }
    if (flg & 0x10) {                   /* FCOMMENT */
        while (p < inlen && in[p]) p++;
        p++;
    }
    if (flg & 0x02) p += 2;             /* FHCRC */
    if (p >= inlen) return -1;

    g.in = in + p;
    g.inlen = inlen - p;
    g.inpos = 0;
    g.bitbuf = 0;
    g.bitcnt = 0;
    g.out = out;
    g.outlen = outlen;
    g.outpos = 0;
    if (inflate_block(&g)) return -1;
    return g.outpos;                    /* 解压后字节数 (忽略尾部 CRC/ISIZE) */
}
