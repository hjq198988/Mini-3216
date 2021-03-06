#include "fb_core.h"
#include "fb_mem.h"
#include "config.h"
#include "font.h"
#include "adc.h"
#include "eeprom.h"
#include "ds3231.h"
#include "i2c.h"
#include "usart.h"
#include "lm75.h"
#include "touch_key.h"
#include "delay.h"
#include "timer.h"
#include "buzzer.h"
#include <string.h>

#define ADC_CHANNEL			6
#define DEFAULT_BRIGHTNESS		110
#define NIGHT_MODE_BRIGHTNESS		1
#define NIGHT_MODE_FAIR_FACTOR		100

sbit is_rotate		= P1 ^ 0;

struct menu {
	const char *name;
	struct menu xdata *child;
	struct menu xdata *sibling_next, *sibling_prev;
	void *private;
	void (code *operate)(void *private);
	unsigned char (code *fb_load)(unsigned char offset);
};

struct user_data {
	union timekeeping timekeeping;
	struct fb_info fb_info;
	bool night_mode;
	struct {
		unsigned char brightness;
	} settings;
	unsigned char offset;
	bool force_update;
	char key;
};

static struct menu xdata root_menu;
static struct menu xdata set_hour_menu;
static struct menu xdata set_minute_menu;
static struct menu xdata *current;

static struct user_data pdata user_data;

static void user_data_init(struct user_data *user)
{
	memset(user, 0, sizeof(*user));
	user->fb_info.brightness = DEFAULT_BRIGHTNESS;
	user->settings.brightness = DEFAULT_BRIGHTNESS;
	user->fb_info.fair = false;
	user->force_update = true;
}

static void pca_init(void)
{
	CCON	= 0;
	CL	= 0;
	CH	= 0;
	CMOD	= 0x00;
	CCAPM0	= 0x21;
	CCAPM1	= 0x31;
	CR	= 1;
}

static void local_irq_disable(void)
{
	EA = 0;
}

static void local_irq_enable(void)
{
	EA = 1;
}

static unsigned char fb_load_temperature(unsigned char offset)
{
	char str[] = {
		' ', '-', ' ', '-', ' ', '.',
		' ', '-', ' ', 'c', '\0',
	};
	char integer, decimals;
	char *p = str + 1;

	if (lm75_read_temperature(&integer, &decimals) &&
	    ds3231_read_temperature(&integer, &decimals))
		goto err;

	if (integer > -10) {
		if (integer > 9)
			str[1] = integer / 10 + '0';
		else if (integer >= 0)
			str[1] = NONE_ASCII;
		else
			integer = ~integer + 1;

		str[3] = integer % 10 + '0';
		str[7] = decimals / 10 + '0';
	} else {
		integer = ~integer + 1;
		str[1] = ' ';
		str[5] = integer / 10 + '0';
		str[7] = integer % 10 + '0';
		p = str;
	}
err:
	return fb_set_string(offset, p);
}

static bool should_show_temperature(struct user_data pdata *user)
{
	return !user->night_mode;
}

static bool should_chime(union timekeeping *timekeeping)
{
	return timekeeping->time.sec == 0x58 &&
	       timekeeping->time.min == 0x59 &&
	       timekeeping->time.hour > 0x07 && timekeeping->time.hour < 0x23;
}

static void fb_load_times(void *priv)
{
	static pdata char sec_old = 0xff, min_old = 0xff, hour_old = 0xff;
	pdata struct user_data *user = priv;
	pdata union timekeeping *timekeeping = &user->timekeeping;
	pdata struct fb_info *fb_info = &user->fb_info;
	bool force = user->force_update;
	char str[5];
	char half_low;
	static bool is_temp = false;
	unsigned char offset = 0;

	if (ds3231_read_times(timekeeping))
		return;

	if (sec_old == timekeeping->time.sec &&
	    min_old == timekeeping->time.min &&
	    hour_old == timekeeping->time.hour && !force)
		return;

	if (should_chime(timekeeping) && !user->night_mode)
		buzzer_chime();

	if (force)
		user->force_update = false;

	sec_old = timekeeping->time.sec;
	if (hour_old != timekeeping->time.hour || force) {
		hour_old = timekeeping->time.hour;
		str[0] = hour_old / 16 + '0';
		str[1] = ' ';
		str[2] = hour_old % 16 + '0';
		str[3] = ' ';
		str[4] = '\0';
		offset += fb_set_string(offset, str);
	} else {
		offset += 14;
	}

	if (sec_old & BIT(0))
		str[0] = '^';
	else
		str[0] = 'v';
	str[1] = ' ';
	str[2] = '\0';
	offset += fb_set_string(offset, str);

	if (min_old != timekeeping->time.min || force) {
		min_old = timekeeping->time.min;
		str[0] = min_old / 16 + '0';
		str[1] = ' ';
		str[2] = min_old % 16 + '0';
		str[3] = '\0';
		offset += fb_set_string(offset, str);
	} else {
		offset += 13;
	}

	half_low = sec_old & 0x0f;
	if (half_low > 2 && half_low < 5 && should_show_temperature(user)) {
		if (!is_temp) {
			unsigned char brightness = fb_info->brightness;

			fb_info->brightness >>= 1;
			fb_info->brightness += 1;
			fb_load_temperature(offset);
			fb_scan(fb_info, 64, 1);
			fb_info->brightness = brightness;
			is_temp = true;
		}
		fb_info->offset = MATRIXS_COLUMNS;
		user->offset = 0;
	} else {
		if (is_temp) {
			unsigned char brightness = fb_info->brightness;

			fb_info->brightness >>= 1;
			fb_info->brightness += 1;
			fb_scan_reverse(fb_info, 64, 1);
			fb_info->brightness = brightness;
			is_temp = false;
		}
		fb_info->offset = 0;
		user->offset = MATRIXS_COLUMNS;
	}
}

static unsigned char fb_load_time(unsigned char offset, enum set_type type,
				  const char *s)
{
	char value;
	unsigned char offset_old = offset;
	char str[] = { ' ', '-', ' ', '-', ' ', ' ', '\0', };

	if (ds3231_read_time(type, &value))
		return 0;

	str[1] = value / 16 + '0';
	str[3] = value % 16 + '0';
	offset += fb_set_string(offset, str);
	offset += fb_set_string(offset, s);

	return offset_old - offset;
}

static unsigned char fb_load_hour(unsigned char offset)
{
	return fb_load_time(offset, SET_HOUR, "时");
}

static unsigned char fb_load_minute(unsigned char offset)
{
	return fb_load_time(offset, SET_MINUTES, "分");
}

static void key_delay(struct fb_info *fb_info)
{
	char i;

	buzzer_key();
	for (i = 0; i < 20; i++)
		fb_show(fb_info);
}

#define HOUR_MAX		24
#define MINUTE_MAX		60

static void set_hour(void *priv)
{
	char value;
	pdata struct user_data *user = priv;
	pdata struct fb_info *fb_info = &user->fb_info;
	char key = user->key;

	if (ds3231_read_time(SET_HOUR, &value))
		return;
	value = value / 16 * 10 + value % 16;

	switch (key) {
	case KEY_RIGHT:
		if (++value == HOUR_MAX)
			value = 0;
		key_delay(fb_info);
		break;
	case KEY_LEFT:
		if (--value == -1)
			value = HOUR_MAX - 1;
		key_delay(fb_info);
		break;
	default:
		return;
	}

	ds3231_set_time(SET_HOUR, value / 10 * 16 + value % 10);
	fb_load_hour(fb_info->offset);
}

static void set_minute(void *priv)
{
	char value;
	pdata struct user_data *user = priv;
	pdata struct fb_info *fb_info = &user->fb_info;
	char key = user->key;

	if (ds3231_read_time(SET_MINUTES, &value))
		return;
	value = value / 16 * 10 + value % 16;

	switch (key) {
	case KEY_RIGHT:
		if (++value == MINUTE_MAX)
			value = 0;
		key_delay(fb_info);
		break;
	case KEY_LEFT:
		if (--value == -1)
			value = MINUTE_MAX - 1;
		key_delay(fb_info);
		break;
	default:
		return;
	}

	ds3231_set_time(SET_MINUTES, value / 10 * 16 + value % 10);
	ds3231_set_time(SET_SECOND, 0);
	fb_load_minute(fb_info->offset);
}

#define ROOT_MENU_NAME		"root"

static bool is_root_menu(struct menu xdata *entry)
{
	return !strcmp(entry->name, ROOT_MENU_NAME);
}

static void menu_init(void)
{
	memset(&root_menu, 0, sizeof(root_menu));
	root_menu.name = ROOT_MENU_NAME;
	/* root_menu.child = &set_hour_menu; */
	root_menu.private = &user_data;
	root_menu.operate = fb_load_times;

	memset(&set_hour_menu, 0, sizeof(set_hour_menu));
	set_hour_menu.private = &user_data;
	set_hour_menu.child = &set_minute_menu;
	set_hour_menu.fb_load = fb_load_hour;
	set_hour_menu.operate = set_hour;

	memset(&set_minute_menu, 0, sizeof(set_minute_menu));
	set_minute_menu.private = &user_data;
	set_minute_menu.child = &root_menu;
	set_minute_menu.fb_load = fb_load_minute;
	set_minute_menu.operate = set_minute;

	current = &root_menu;
}

static bool interface_switching(struct user_data pdata *user, char key)
{
	struct fb_info pdata *fb_info = &user->fb_info;
	struct menu xdata *current_old = current;
	bool success = false;

	switch (key) {
	case KEY_ENTER:
		if (is_root_menu(current) || !current->child)
			break;

		buzzer_enter();
		current = current->child;
		if (is_root_menu(current)) {
			user->offset = 0;
			user->force_update = true;
			break;
		}

		if (current->fb_load)
			user->offset += current->fb_load(fb_info->offset +
							 MATRIXS_COLUMNS);
		else
			user->offset += fb_set_string(user->offset,
						      current->name);
		fb_scan(fb_info, 64, 2);
		fb_info->offset += MATRIXS_COLUMNS;
		break;
	case KEY_LEFT:
		if (!current->sibling_prev)
			break;
		buzzer_key();
		user->offset += fb_set_string(user->offset,
					current->sibling_prev->name);
		fb_scan_reverse(fb_info, 64, 1);
		fb_info->offset -= MATRIXS_COLUMNS;
		current = current->sibling_prev;
		break;
	case KEY_RIGHT:
		if (!current->sibling_next)
			break;
		buzzer_key();
		user->offset += fb_set_string(user->offset,
					current->sibling_next->name);
		fb_scan(fb_info, 64, 1);
		fb_info->offset += MATRIXS_COLUMNS;
		current = current->sibling_next;
		break;
	case KEY_LEFT | KEY_RIGHT:
		/* special for root menu and enter the settings menu */
		if (!is_root_menu(current))
			break;

		if (!current->child)
			break;
		buzzer_enter();
		current = current->child;
		user->offset += fb_set_string(user->offset, current->name);
		fb_scan(fb_info, 64, 1);
		fb_info->offset += MATRIXS_COLUMNS;
		break;
	case KEY_LEFT | KEY_ENTER:
		/* special for root menu and enter the setup time menu */
		if (!is_root_menu(current))
			break;

		buzzer_enter();
		current = &set_hour_menu;
		fb_info->offset = fb_scan_string(fb_info, 5, "设置时间");
		if (current->fb_load)
			current->fb_load(fb_info->offset + 32);
		fb_scan(fb_info, 64, 5);
		fb_info->offset += 32;
		break;
	default:
		return false;
	}

	if (current_old != current)
		success = true;

	return success;
}

void main(void)
{
	struct fb_info pdata *fb_info = &user_data.fb_info;

	buzzer_power_on();
	uart_init();
	user_data_init(&user_data);
	fb_clear(0, 64);
	i2c_init();
	ds3231_init();
	font_sort();
	menu_init();
	pca_init();
	adc_init(ADC_CHANNEL);
	timer0_init();
	timer1_init();
	local_irq_enable();

	while (1) {
		if (touch_key_read(&user_data.key)) {
			bool success = interface_switching(&user_data,
							   user_data.key);

			while (success && touch_key_read(&user_data.key))
				fb_show(fb_info);
		}
		if (current && current->operate)
			current->operate(current->private);
		fb_show(fb_info);
	}
}

/* Timer0 interrupt routine */
void timer0_isr() interrupt 1 using 2
{
	static char adc_cnt = 0;

	if (++adc_cnt == 20) {
		adc_start(ADC_CHANNEL);
		adc_cnt = 0;
	}
}

/* ADC interrupt routine */
void adc_isr(void) interrupt 5 using 1
{
	unsigned char result;

	ADC_CONTR &= ~ADC_FLAG;
	result = ADC_RES;
	if (result > 0xf0) {
		user_data.night_mode = true;
		user_data.fb_info.fair = NIGHT_MODE_FAIR_FACTOR;
		user_data.fb_info.brightness = NIGHT_MODE_BRIGHTNESS;
	} else if (result < 0xc8) {
		user_data.night_mode = false;
		user_data.fb_info.brightness = user_data.settings.brightness;
		user_data.fb_info.fair = false;
	}
}

void pca_isr(void) interrupt 7 using 2
{
	if (CCF0) {
		CCF0 = 0;
	} if (CCF1) {
		CCF1 = 0;
		user_data.fb_info.rotate = !is_rotate;
	}
}
