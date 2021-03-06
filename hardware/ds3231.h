#ifndef __DS3231_H__
#define __DS3231_H__

#include "core.h"

#define DS3231_SLAVER_ADDR		0x68

enum set_type {
	SET_SECOND = 0,
	SET_MINUTES,
	SET_HOUR,
	SET_DAY,
	SET_DATE,
	SET_MONTH,
	SET_YEAR,
};

union timekeeping {
	struct {
		char sec;
		char min;
		char hour;
		char day;
		char date;
		char month;
		char year;
	} time;
	char times[7];
};

char ds3231_init(void);
char ds3231_read_times(union timekeeping *timekeeping);
char ds3231_read_time(enum set_type setting, char *value);
char ds3231_set_times(union timekeeping *timekeeping);
char ds3231_set_time(enum set_type setting, char value);
char ds3231_read_temperature(char *integer, char *decimals);

#endif