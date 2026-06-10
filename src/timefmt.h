#ifndef LATER_TIMEFMT_H_
#define LATER_TIMEFMT_H_

#include <stddef.h>
#include <time.h>

/*
 * Accepted inputs:
 *   +1d2h30m, +30s, +0m        relative to now
 *   HH:MM or HH:MM:SS          today (or tomorrow if already past)
 *   YYYY-MM-DDTHH:MM:SS        absolute local time (must be in the future)
 *
 * On error, write a message into errbuf and return -1.
 */
int timefmt_parse_time(const char *input, time_t *out, char *errbuf, size_t errsz);

/* "YYYY-MM-DD HH:MM:SS" in local time. */
void timefmt_format_time(time_t t, char *buf, size_t n);

/* "1h 2m 3s", "0s", or "5m ago" for negative durations. */
void timefmt_format_duration(long secs, char *buf, size_t n);

#endif // LATER_TIMEFMT_H_
