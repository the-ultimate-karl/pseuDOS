#include "rtc.h"
#include "io.h"
#include "lib.h"

#define CMOS_ADDR_PORT 0x70
#define CMOS_DATA_PORT 0x71

#define RTC_REG_SECONDS      0x00
#define RTC_REG_MINUTES      0x02
#define RTC_REG_HOURS        0x04
#define RTC_REG_DAY_OF_MONTH 0x07
#define RTC_REG_MONTH        0x08
#define RTC_REG_YEAR         0x09
#define RTC_REG_STATUS_A     0x0A
#define RTC_REG_STATUS_B     0x0B
#define RTC_REG_CENTURY      0x32

#define BCD_TO_BIN(val) ((((val) >> 4) * 10) + ((val) & 0x0F))

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR_PORT, reg & 0x7F);
    return inb(CMOS_DATA_PORT);
}

static int rtc_is_updating(void) {
    outb(CMOS_ADDR_PORT, RTC_REG_STATUS_A);
    return (inb(CMOS_DATA_PORT) & 0x80);
}

void rtc_init(void) {
    /* Ensure RTC is functioning and read initial value */
    rtc_datetime_t dt;
    rtc_get_datetime(&dt);
}

int rtc_get_datetime(rtc_datetime_t *dt) {
    if (!dt) return -1;

    /* Wait if RTC update is currently in progress (bounded timeout) */
    int timeout = 10000;
    while (rtc_is_updating() && --timeout > 0) {
        io_wait();
    }

    uint8_t sec   = cmos_read(RTC_REG_SECONDS);
    uint8_t min   = cmos_read(RTC_REG_MINUTES);
    uint8_t hour  = cmos_read(RTC_REG_HOURS);
    uint8_t day   = cmos_read(RTC_REG_DAY_OF_MONTH);
    uint8_t month = cmos_read(RTC_REG_MONTH);
    uint8_t year  = cmos_read(RTC_REG_YEAR);
    uint8_t cent  = cmos_read(RTC_REG_CENTURY);
    uint8_t reg_b = cmos_read(RTC_REG_STATUS_B);

    /* Check if values are in BCD format (Bit 2 of Register B: 0 = BCD, 1 = Binary) */
    if (!(reg_b & 0x04)) {
        sec   = BCD_TO_BIN(sec);
        min   = BCD_TO_BIN(min);
        hour  = ((hour & 0x7F) != hour) ? (BCD_TO_BIN(hour & 0x7F) | 0x80) : BCD_TO_BIN(hour);
        day   = BCD_TO_BIN(day);
        month = BCD_TO_BIN(month);
        year  = BCD_TO_BIN(year);
        if (cent != 0 && cent != 0xFF) {
            cent = BCD_TO_BIN(cent);
        }
    }

    /* Convert 12-hour clock to 24-hour clock if necessary (Bit 1 of Register B: 0 = 12h, 1 = 24h) */
    if (!(reg_b & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    /* Calculate 4-digit year */
    uint16_t full_year;
    if (cent >= 19 && cent <= 22) {
        full_year = (cent * 100) + year;
    } else {
        full_year = 2000 + year;
    }

    dt->year    = full_year;
    dt->month   = month;
    dt->day     = day;
    dt->hours   = hour;
    dt->minutes = min;
    dt->seconds = sec;

    return 0;
}

void rtc_format_date(char *buf, size_t size, const rtc_datetime_t *dt) {
    if (!buf || size == 0) return;
    if (!dt) {
        strncpy(buf, "1970-01-01", size - 1);
        buf[size - 1] = '\0';
        return;
    }
    snprintf(buf, size, "%04u-%02u-%02u", dt->year, dt->month, dt->day);
}

void rtc_format_time(char *buf, size_t size, const rtc_datetime_t *dt) {
    if (!buf || size == 0) return;
    if (!dt) {
        strncpy(buf, "00:00:00", size - 1);
        buf[size - 1] = '\0';
        return;
    }
    snprintf(buf, size, "%02u:%02u:%02u", dt->hours, dt->minutes, dt->seconds);
}

void rtc_format_datetime(char *buf, size_t size, const rtc_datetime_t *dt) {
    if (!buf || size == 0) return;
    if (!dt) {
        strncpy(buf, "1970-01-01 00:00:00", size - 1);
        buf[size - 1] = '\0';
        return;
    }
    snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u",
        dt->year, dt->month, dt->day, dt->hours, dt->minutes, dt->seconds);
}
