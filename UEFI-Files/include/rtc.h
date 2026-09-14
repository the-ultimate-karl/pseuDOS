#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stddef.h>

/* Real-Time Clock Datetime Structure */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
} rtc_datetime_t;

/* RTC API */
void rtc_init(void);
int rtc_get_datetime(rtc_datetime_t *dt);
void rtc_format_date(char *buf, size_t size, const rtc_datetime_t *dt);
void rtc_format_time(char *buf, size_t size, const rtc_datetime_t *dt);
void rtc_format_datetime(char *buf, size_t size, const rtc_datetime_t *dt);

#endif /* RTC_H */
