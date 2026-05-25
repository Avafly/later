#ifndef LATER_TIMEFMT_H
#define LATER_TIMEFMT_H

#include <stddef.h>
#include <time.h>

/*
 * Accepted inputs:
 *   +1d2h30m, +30s, +0m         relative to now
 *   HH:MM or HH:MM:SS           today (or tomorrow if already past)
 *   YYYY-MM-DDTHH:MM:SS         absolute local time (must be in the future)
 *
 * On error, writes a message into errbuf and returns -1.
 */
int parse_time(const char *input, time_t *out, char *errbuf, size_t errsz);

/* "YYYY-MM-DD HH:MM:SS" in local time. */
void format_time(time_t t, char *buf, size_t n);

/* "1h 2m 3s", "0s", or "5m ago" for negative durations. */
void format_duration(long seconds, char *buf, size_t n);

#endif
