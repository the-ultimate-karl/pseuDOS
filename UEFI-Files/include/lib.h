#ifndef LIB_H
#define LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Dynamic Memory Heap Allocation */
void heap_init(uint64_t heap_start, size_t heap_size);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t num, size_t size);
void *krealloc(void *ptr, size_t new_size);
size_t heap_get_total(void);
size_t heap_get_used(void);

/* Memory operations */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

/* String operations */
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
char *trim(char *str);
void to_lowercase(char *str);
long strtol(const char *nptr, char **endptr, int base);

/* Formatted print routines */
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);

#endif /* LIB_H */
