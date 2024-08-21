/** ffbase: date/time functions
2020, Simon Zolin
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/string.h> // optional

/*
fftime_add fftime_sub
fftime_valid
fftime_join1
fftime_split1
fftime_tostr1
fftime_fromstr1
*/

/** Time value */
typedef struct fftime {
	ffint64 sec;
	ffuint nsec;
} fftime;

/** Seconds passed until 1970 */
#define FFTIME_1970_SECONDS  62135596800ULL

/** Set time from milliseconds */
static inline void fftime_from_msec(fftime *t, ffuint64 msec)
{
	t->sec = msec / 1000;
	t->nsec = (msec % 1000) * 1000000;
}

/** Add time value */
static inline void fftime_add(fftime *t, const fftime *add)
{
	t->sec += add->sec;
	t->nsec += add->nsec;
	if (t->nsec >= 1000000000) {
		t->nsec -= 1000000000;
		t->sec++;
	}
}

/** Subtract time value */
static inline void fftime_sub(fftime *t, const fftime *sub)
{
	t->sec -= sub->sec;
	t->nsec -= sub->nsec;
	if ((int)t->nsec < 0) {
		t->nsec += 1000000000;
		t->sec--;
	}
}

/** Compare time values */
static inline int fftime_cmp(const fftime *t1, const fftime *t2)
{
	if (t1->sec == t2->sec) {
		if (t1->nsec == t2->nsec)
			return 0;
		else if (t1->nsec < t2->nsec)
			return -1;
	} else if (t1->sec < t2->sec)
		return -1;
	return 1;
}

static inline int fftime_cmp_val(fftime t1, fftime t2) { return fftime_cmp(&t1, &t2); }


/** Date/time parts */
typedef struct ffdatetime {
	int year;
	ffuint month; //1..12
	ffuint day; //1..31

	ffuint hour; //0..23
	ffuint minute;  //0..59
	ffuint second;  //0..59
	ffuint nanosecond; //0..999,999,999

	ffuint weekday; //0..6 (0:Sunday)
	ffuint yearday; //1..366
} ffdatetime;

/** Return TRUE if date and time values are in allowed range
Note: 'weekday' and 'yearday' are not checked */
static inline int fftime_valid(const ffdatetime *dt)
{
	if (!(dt->hour <= 23
		&& dt->minute <= 59
		&& dt->second <= 59
		&& dt->nanosecond <= 999999999))
		return 0;

	static const ffbyte month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int leap = dt->year%4 == 0 && (dt->year%100 != 0 || dt->year%400 == 0);
	if (!(dt->year != 0
		&& dt->month-1 < 12
		&& (dt->day-1 < month_days[dt->month - 1]
			|| (dt->day == 29 && dt->month == 2 && leap))))
		return 0;

	return 1;
}

/** Join time parts into a time value (after Jan 1, 1 AD)
Note: the time values in 'dt' are allowed to overflow
'weekday' and 'yearday' values are not read
If either 'year', 'month' or 'day' is 0, only time values are read and the date values are skipped
'year' < 0 isn't supported */
static inline void fftime_join1(fftime *t, const ffdatetime *dt)
{
	t->sec = (ffint64)dt->hour*60*60 + dt->minute*60 + dt->second;
	t->nsec = dt->nanosecond;
	if (t->nsec >= 1000000000) {
		t->sec += t->nsec % 1000000000;
		t->nsec /= 1000000000;
	}

	if (dt->year <= 0 || dt->month == 0 || dt->day == 0)
		return;

	ffuint year = dt->year;
	ffuint mon = dt->month;
	if (mon > 12) {
		mon--;
		year += mon / 12;
		mon = (mon % 12) + 1;
	}
	mon -= 2; // Jan -> Mar
	if ((int)mon <= 0) {
		mon += 12;
		year--;
	}

	ffuint64 days = year*365 + year/4 - year/100 + year/400 - 365 // number of days passed since 1 AD
		+ (367 * mon / 12 - 30) + 31+28 // number of days since new year (Mar 1)
		+ dt->day-1;
	t->sec += days*60*60*24;
}

/** Split time value (after Jan 1, 1 AD) into date/time parts

Algorithm:
. Get day of week (1/1/1 was Monday).
. Get year and the days passed since its Mar 1:
  . Get days passed since Mar 1, 1 AD
  . Get approximate year (days / ~365.25).
  . Get days passed during the year.
. Get month and its day:
  . Get month by year day
  . Get year days passed before this month
  . Get day of month
. Shift New Year from Mar 1 to Jan 1
  . If year day is within Mar..Dec, add 2 months
  . If year day is within Jan..Feb, add 2 months and increment year
*/
static inline void fftime_split1(ffdatetime *dt, const fftime *t)
{
	ffuint64 sec = t->sec;
	dt->nanosecond = t->nsec;

	ffuint days = sec / (60*60*24);
	dt->weekday = (1 + days) % 7;

	sec = sec % (60*60*24);
	dt->hour = sec / (60*60);
	dt->minute = (sec % (60*60)) / 60;
	dt->second = sec % 60;

	days += 306; // 306: days from Mar before Jan
	ffuint year = 1 + days * 400 / (400*365 + 400/4 - 400/100 + 400/400);
	ffuint yday = days - (year*365 + year/4 - year/100 + year/400);
	if ((int)yday < 0) {
		int leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
		yday += 365 + leap;
		year--;
	}

	ffuint mon = (yday + 31) * 10 / 306; // get month by year day (1: March)
	ffuint mday = 367 * mon / 12 - 30; // get year days passed before this month (1: March)
	mday = yday - mday;
	dt->day = mday + 1;

	if (yday >= 306) {
		year++;
		mon -= 10;
		yday -= 306;
	} else {
		mon += 2;
		int leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
		yday += 31 + 28 + leap;
	}

	dt->year = year;
	dt->month = mon;
	dt->yearday = yday + 1;
}

enum FFTIME_FMT {
	// date
	FFTIME_DATE_YMD = 1, // yyyy-MM-dd
	FFTIME_DATE_WDMY, // Wed, dd Sep yyyy
	FFTIME_DATE_DMY, // dd-MM-yyyy
	FFTIME_DATE_MDY, // MM/dd/yyyy

	// time
	FFTIME_HMS = 0x10, // hh:mm:ss
	FFTIME_HMS_MSEC = 0x20, // hh:mm:ss.msc
	FFTIME_HMS_GMT = 0x30, // hh:mm:ss GMT
	FFTIME_HMS_MSEC_VAR = 0x40, // [[h:]m:]s[.ms] (optional hour, minute and millisecond)
	FFTIME_HMS_USEC = 0x50, // hh:mm:ss.mcrsec

	// date & time:
	FFTIME_YMD = FFTIME_DATE_YMD | FFTIME_HMS, // yyyy-MM-dd hh:mm:ss (ISO 8601)
	FFTIME_WDMY = FFTIME_DATE_WDMY | FFTIME_HMS_GMT, // Wed, dd Sep yyyy hh:mm:ss GMT (RFC1123)
};

#ifdef _FFBASE_STRFORMAT_H

static const char _fftime_months[][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char _fftime_week_days[][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/** Convert date/time to string
flags: enum FFTIME_FMT
Return N of bytes written;  0 on error */
static inline ffsize fftime_tostr1(const ffdatetime *dt, char *dst, ffsize cap, ffuint flags)
{
	int i = 0, r = 0;

	switch (flags & 0x0f) {
	case FFTIME_DATE_YMD:
		i = ffs_format(dst, cap, "%04u-%02u-%02u"
			, dt->year, dt->month, dt->day);
		break;

	case FFTIME_DATE_WDMY:
		i = ffs_format(dst, cap, "%s, %02u %s %04u"
			, _fftime_week_days[dt->weekday], dt->day, _fftime_months[dt->month - 1], dt->year);
		break;

	case FFTIME_DATE_MDY:
		i = ffs_format(dst, cap, "%02u/%02u/%04u"
			, dt->month, dt->day, dt->year);
		break;

	case FFTIME_DATE_DMY:
		i = ffs_format(dst, cap, "%02u.%02u.%04u"
			, dt->day, dt->month, dt->year);
		break;

	case 0:
		break; // no date

	default:
		return 0; // unknown date format
	}

	if (i < 0)
		return 0;

	if (!!(flags & 0x0f) && !!(flags & 0xf0) && (ffuint)i < cap)
		dst[i++] = ' ';

	switch (flags & 0xf0) {
	case FFTIME_HMS:
		r = ffs_format(dst + i, cap - i, "%02u:%02u:%02u"
			, dt->hour, dt->minute, dt->second);
		break;

	case FFTIME_HMS_MSEC:
		r = ffs_format(dst + i, cap - i, "%02u:%02u:%02u.%03u"
			, dt->hour, dt->minute, dt->second, dt->nanosecond / 1000000);
		break;

	case FFTIME_HMS_USEC:
		r = ffs_format(dst + i, cap - i, "%02u:%02u:%02u.%06u"
			, dt->hour, dt->minute, dt->second, dt->nanosecond / 1000);
		break;

	case FFTIME_HMS_GMT:
		r = ffs_format(dst + i, cap - i, "%02u:%02u:%02u GMT"
			, dt->hour, dt->minute, dt->second);
		break;

	case 0:
		break; // no time

	default:
		return 0; // unknown time format
	}

	if (r < 0)
		return 0;
	return i + r;
}

static int _fftime_date_fromstr(ffdatetime *dt, ffstr *str, ffuint flags)
{
	int r;

	switch (flags & 0x0f) {
	case FFTIME_DATE_YMD:
		if (0 > (r = ffstr_matchfmt(str, "%4u-%2u-%2u"
			, &dt->year, &dt->month, &dt->day)))
			return -1;

		dt->weekday = 0;
		break;

	case FFTIME_DATE_MDY:
		if (0 > (r = ffstr_matchfmt(str, "%u/%u/%4u"
			, &dt->year, &dt->month, &dt->day)))
			return -1;

		dt->weekday = 0;
		break;

	case FFTIME_DATE_DMY:
		if (0 > (r = ffstr_matchfmt(str, "%2u.%2u.%4u"
			, &dt->year, &dt->month, &dt->day)))
			return -1;

		dt->weekday = 0;
		break;

	case FFTIME_DATE_WDMY: {
		ffstr wd, mon;
		if (0 > (r = ffstr_matchfmt(str, "%3S, %2u %3S %4u"
			, &wd, &dt->day, &mon, &dt->year)))
			return -1;

		char s[4];
		s[3] = '\0';
		ffmem_copy(s, mon.ptr, 3);
		int i = ffarrint32_find((ffuint*)_fftime_months, FF_COUNT(_fftime_months), *(ffuint*)s);
		if (i < 0)
			return -1;
		dt->month = 1 + i;

		ffmem_copy(s, wd.ptr, 3);
		i = ffarrint32_find((ffuint*)_fftime_week_days, FF_COUNT(_fftime_week_days), *(ffuint*)s);
		if (i >= 0)
			dt->weekday = i;
		break;
	}

	case 0:
		return 0; // no date

	default:
		return -1;
	}

	if (r == 0)
		str->len = 0;
	else
		ffstr_shift(str, r-1);
	return 0;
}

static int _fftime_time_fromstr(ffdatetime *dt, ffstr *str, ffuint flags)
{
	int r;
	ffuint val;

	switch (flags & 0xf0) {
	case FFTIME_HMS:
		if (0 > (r = ffstr_matchfmt(str, "%2u:%2u:%2u"
			, &dt->hour, &dt->minute, &dt->second)))
			return -1;
		break;

	case FFTIME_HMS_MSEC:
		if (0 > (r = ffstr_matchfmt(str, "%2u:%2u:%2u.%3u"
			, &dt->hour, &dt->minute, &dt->second, &dt->nanosecond)))
			return -1;
		dt->nanosecond *= 1000000;
		break;

	case FFTIME_HMS_GMT:
		if (0 > (r = ffstr_matchfmt(str, "%2u:%2u:%2u GMT"
			, &dt->hour, &dt->minute, &dt->second)))
			return -1;
		break;

	case FFTIME_HMS_MSEC_VAR:
		if (0 == (r = ffs_toint(str->ptr, str->len, &val, FFS_INT32)))
			return -1;
		ffstr_shift(str, r);
		if (!(str->len != 0 && str->ptr[0] == ':')) {
			dt->second = val;
			goto msec;
		}
		ffstr_shift(str, 1);
		dt->minute = val;

		if (0 == (r = ffs_toint(str->ptr, str->len, &val, FFS_INT32)))
			return -1;
		ffstr_shift(str, r);
		if (!(str->len != 0 && str->ptr[0] == ':')) {
			dt->second = val;
			goto msec;
		}
		ffstr_shift(str, 1);
		dt->hour = dt->minute;
		dt->minute = val;

		if (0 == (r = ffs_toint(str->ptr, str->len, &val, FFS_INT32)))
			return -1;
		ffstr_shift(str, r);
		dt->second = val;

msec:
		if (str->len != 0 && str->ptr[0] == '.') {
			ffstr_shift(str, 1);
			if (0 == (r = ffs_toint(str->ptr, str->len, &val, FFS_INT32)))
				return -1;
			dt->nanosecond = val * 1000000;
			ffstr_shift(str, r);
		}
		return 0;

	case 0:
		return 0; // no time

	default:
		return -1;
	}

	if (r == 0)
		str->len = 0;
	else
		ffstr_shift(str, r-1);
	return 0;
}

/** Convert string to date/time
flags: enum FFTIME_FMT
Return the number of processed bytes;
  0 on error */
static inline ffsize fftime_fromstr1(ffdatetime *dt, const char *s, ffsize len, ffuint flags)
{
	ffdatetime dt2 = {};
	ffstr str = FFSTR_INITN(s, len);

	if (0 != _fftime_date_fromstr(&dt2, &str, flags))
		goto end;

	if ((flags & 0x0f) && (flags & 0xf0)) {
		if (str.len == 0 || ffstr_popfront(&str) != ' ')
			goto end;
	}

	if (0 != _fftime_time_fromstr(&dt2, &str, flags))
		goto end;

	*dt = dt2;
	return len - str.len;

end:
	return 0;
}

#endif // _FFBASE_STRFORMAT_H
