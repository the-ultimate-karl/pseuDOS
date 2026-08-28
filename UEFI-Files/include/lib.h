#ifndef LIB_H
#define LIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Memory operations */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

/* String operations */
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *trim(char *str);
void to_lowercase(char *str);

/* Formatting */
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);

#endif /* LIB_H */
