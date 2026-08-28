#include "lib.h"

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2 || n == 0) return 0;
    while (n-- && *s1 && *s2) {
        if (*s1 != *s2) {
            return *(const unsigned char *)s1 - *(const unsigned char *)s2;
        }
        s1++;
        s2++;
    }
    return 0;
}

int strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return s1 ? 1 : (s2 ? -1 : 0);
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) {
            return (unsigned char)c1 - (unsigned char)c2;
        }
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    if (!dest || !src) return dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    if (!dest || !src) return dest;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    if (!dest || !src) return dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *trim(char *str) {
    if (!str) return NULL;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = 0;
        end--;
    }
    return str;
}

void to_lowercase(char *str) {
    if (!str) return;
    while (*str) {
        if (*str >= 'A' && *str <= 'Z') {
            *str += 32;
        }
        str++;
    }
}

/* Number to string formatting helper */
static int utoa(uint64_t val, char *buf, int base, int uppercase, int min_width, char pad) {
    char tmp[65];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }

    int len = 0;
    while (i < min_width && (len + i) < min_width) {
        buf[len++] = pad;
    }
    while (i > 0) {
        buf[len++] = tmp[--i];
    }
    buf[len] = '\0';
    return len;
}

static int itoa_s(int64_t val, char *buf, int min_width, char pad) {
    if (val < 0) {
        buf[0] = '-';
        return 1 + utoa((uint64_t)(-val), buf + 1, 10, 0, min_width > 1 ? min_width - 1 : 0, pad);
    }
    return utoa((uint64_t)val, buf, 10, 0, min_width, pad);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    if (!str || size == 0) return 0;

    size_t out_idx = 0;
    const char *p = format;

    while (*p && out_idx + 1 < size) {
        if (*p != '%') {
            str[out_idx++] = *p++;
            continue;
        }

        p++; /* Skip '%' */
        char pad = ' ';
        int width = 0;

        if (*p == '0') {
            pad = '0';
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        int is_long = 0;
        if (*p == 'l') {
            is_long++;
            p++;
            if (*p == 'l') {
                is_long++;
                p++;
            }
        }

        char num_buf[65];
        int num_len = 0;

        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t slen = strlen(s);
                while (width > (int)slen && out_idx + 1 < size) {
                    str[out_idx++] = ' ';
                    width--;
                }
                while (*s && out_idx + 1 < size) {
                    str[out_idx++] = *s++;
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                str[out_idx++] = c;
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = (is_long >= 2) ? va_arg(ap, int64_t) : (is_long == 1 ? va_arg(ap, long) : va_arg(ap, int));
                num_len = itoa_s(v, num_buf, width, pad);
                for (int i = 0; i < num_len && out_idx + 1 < size; i++) {
                    str[out_idx++] = num_buf[i];
                }
                break;
            }
            case 'u': {
                uint64_t v = (is_long >= 2) ? va_arg(ap, uint64_t) : (is_long == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                num_len = utoa(v, num_buf, 10, 0, width, pad);
                for (int i = 0; i < num_len && out_idx + 1 < size; i++) {
                    str[out_idx++] = num_buf[i];
                }
                break;
            }
            case 'x':
            case 'p': {
                uint64_t v = (*p == 'p' || is_long >= 2) ? va_arg(ap, uint64_t) : (is_long == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                num_len = utoa(v, num_buf, 16, 0, width, pad);
                for (int i = 0; i < num_len && out_idx + 1 < size; i++) {
                    str[out_idx++] = num_buf[i];
                }
                break;
            }
            case 'X': {
                uint64_t v = (is_long >= 2) ? va_arg(ap, uint64_t) : (is_long == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                num_len = utoa(v, num_buf, 16, 1, width, pad);
                for (int i = 0; i < num_len && out_idx + 1 < size; i++) {
                    str[out_idx++] = num_buf[i];
                }
                break;
            }
            case '%': {
                str[out_idx++] = '%';
                break;
            }
            default:
                str[out_idx++] = *p;
                break;
        }
        p++;
    }

    str[out_idx] = '\0';
    return (int)out_idx;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}
