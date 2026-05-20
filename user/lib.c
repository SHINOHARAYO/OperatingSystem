#include "lib.h"
#include "ns_proto.h"
#include <stdarg.h>

static int uart_cap = -1;

static int get_uart_cap(void) {
    if (uart_cap < 0) {
        uart_cap = ns_resolve("uart");
    }
    return uart_cap;
}

static void _flush_buf(const char *buf, int len) {
    int offset = 0;
    while (offset < len) {
        uint64_t chunks[IPC_INLINE_WORDS] = {0};
        char *dst = (char *)chunks;
        int i;
        for (i = 0; i < (int)IPC_INLINE_BYTES && offset < len; i++, offset++) {
            dst[i] = buf[offset];
        }
        int cap = get_uart_cap();
        if (cap < 0) {
            return;
        }

        sys_ipc_call((uint32_t)cap, 0, (uint64_t)i, chunks);
    }
}

static inline void _put(char *buf, int *pos, char c) {
    buf[(*pos)++] = c;
}

static inline void _maybe_flush(char *buf, int *pos) {
    if (*pos >= 240) {
        _flush_buf(buf, *pos);
        *pos = 0;
    }
}

static void _fmt_uint(char *buf, int *pos, uint64_t val, unsigned int base) {
    const char *digits = "0123456789abcdef";
    if (val == 0) { _put(buf, pos, '0'); return; }
    char tmp[24]; int n = 0;
    while (val) { tmp[n++] = digits[val % base]; val /= base; }
    for (int i = n - 1; i >= 0; i--) _put(buf, pos, tmp[i]);
}


void *memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int value, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    if (len) _flush_buf(s, len);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buf[256];
    int pos = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            _put(buf, &pos, *p);
            _maybe_flush(buf, &pos);
            continue;
        }
        p++;
        if (!*p) break;

        switch (*p) {
            case 'd': {
                int val = va_arg(args, int);
                if (val < 0) { _put(buf, &pos, '-'); val = -val; }
                _fmt_uint(buf, &pos, (uint64_t)(unsigned int)val, 10);
                break;
            }
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                _fmt_uint(buf, &pos, (uint64_t)val, 10);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                _fmt_uint(buf, &pos, (uint64_t)val, 16);
                break;
            }
            case 'l': {
                p++;
                if (*p == 'u') {
                    uint64_t val = va_arg(args, uint64_t);
                    _fmt_uint(buf, &pos, val, 10);
                } else if (*p == 'x') {
                    uint64_t val = va_arg(args, uint64_t);
                    _fmt_uint(buf, &pos, val, 16);
                } else if (*p == 'd') {
                    int64_t val = va_arg(args, int64_t);
                    if (val < 0) { _put(buf, &pos, '-'); val = -val; }
                    _fmt_uint(buf, &pos, (uint64_t)val, 10);
                } else {
                    _put(buf, &pos, '%');
                    _put(buf, &pos, 'l');
                    _put(buf, &pos, *p);
                }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) { _put(buf, &pos, *s++); _maybe_flush(buf, &pos); }
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                _put(buf, &pos, c);
                break;
            }
            case '%': {
                _put(buf, &pos, '%');
                break;
            }
            default: {
                _put(buf, &pos, '%');
                _put(buf, &pos, *p);
                break;
            }
        }
        _maybe_flush(buf, &pos);
    }

    if (pos > 0) _flush_buf(buf, pos);
    va_end(args);
}
