#include "timefmt.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_relative(const char *input, time_t *out, char *errbuf, size_t errsz)
{
    // +<num>(d|h|m|s)... in any combination, each unit at most once
    if (input[0] != '+')
    {
        snprintf(errbuf, errsz, "Relative time must start with '+'");
        return -1;
    }
    const char *p = input + 1;
    if (!*p)
    {
        snprintf(errbuf, errsz, "Empty relative time");
        return -1;
    }

    // 10 years; also bounds the arithmetic below
    const long max_delay = 10L * 365 * 86400;
    long secs = 0;
    int seen_d = 0, seen_h = 0, seen_m = 0, seen_s = 0;
    while (*p)
    {
        if (!isdigit((unsigned char)*p))
        {
            snprintf(errbuf, errsz, "Invalid relative time :%s", input);
            return -1;
        }
        char *end;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || v < 0)
        {
            snprintf(errbuf, errsz, "Invalid relative time: %s", input);
            return -1;
        }
        char unit = *end;
        long mult;
        switch (unit)
        {
            case 'd':
                if (seen_d)
                    goto dup;
                seen_d = 1;
                mult = 86400;
                break;
            case 'h':
                if (seen_h)
                    goto dup;
                seen_h = 1;
                mult = 3600;
                break;
            case 'm':
                if (seen_m)
                    goto dup;
                seen_m = 1;
                mult = 60;
                break;
            case 's':
                if (seen_s)
                    goto dup;
                seen_s = 1;
                mult = 1;
                break;
            default:
                snprintf(errbuf, errsz, "Invalid unit '%c' in: %s", unit, input);
                return -1;
        }
        if (v > (max_delay - secs) / mult)
        {
            snprintf(errbuf, errsz, "Relative time too large: %s", input);
            return -1;
        }
        secs += v * mult;
        p = end + 1;
    }

    *out = time(NULL) + secs;
    return 0;

dup:
    snprintf(errbuf, errsz, "Duplicate unit in: %s", input);
    return -1;
}

static int parse_iso(const char *input, time_t *out, char *errbuf, size_t errsz)
{
    int y, mo, d, h, mi, s;
    int consumed = 0;
    if (sscanf(input, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &consumed) != 6 ||
        input[consumed] != '\0')
    {
        snprintf(errbuf, errsz, "Invalid ISO time: %s", input);
        return -1;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 ||
        s > 59)
    {
        snprintf(errbuf, errsz, "Invalid date/time values: %s", input);
        return -1;
    }
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1)
    {
        snprintf(errbuf, errsz, "Invalid date/time: %s", input);
        return -1;
    }
    // mktime normalizes out-of-range dates (e.g. Feb 30 -> Mar 2); reject those
    if (tm.tm_year != y - 1900 || tm.tm_mon != mo - 1 || tm.tm_mday != d)
    {
        snprintf(errbuf, errsz, "Invalid date/time values: %s", input);
        return -1;
    }
    if (t <= time(NULL))
    {
        snprintf(errbuf, errsz, "Time %s has already passed", input);
        return -1;
    }
    *out = t;
    return 0;
}

static int parse_clock(const char *input, time_t *out, char *errbuf, size_t errsz)
{
    int h, m, s = 0;
    int consumed = 0;
    if (sscanf(input, "%d:%d:%d%n", &h, &m, &s, &consumed) == 3 && input[consumed] == '\0')
    {
        // matched HH:MM:SS
    }
    else
    {
        consumed = 0;
        s = 0;
        if (sscanf(input, "%d:%d%n", &h, &m, &consumed) != 2 || input[consumed] != '\0')
        {
            snprintf(errbuf, errsz, "Invalid time: %s", input);
            return -1;
        }
    }
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59)
    {
        snprintf(errbuf, errsz, "Invalid time values: %s", input);
        return -1;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t <= now)
    {
        tm.tm_mday += 1;
        tm.tm_isdst = -1;
        t = mktime(&tm);
        if (t == (time_t)-1)
        {
            snprintf(errbuf, errsz, "Cannot compute tomorrow for: %s", input);
            return -1;
        }
    }
    *out = t;
    return 0;
}

int timefmt_parse_time(const char *input, time_t *out, char *errbuf, size_t errsz)
{
    if (!input || !*input)
    {
        snprintf(errbuf, errsz, "Empty time string");
        return -1;
    }
    if (input[0] == '+')
        return parse_relative(input, out, errbuf, errsz);
    if (strchr(input, 'T'))
        return parse_iso(input, out, errbuf, errsz);
    return parse_clock(input, out, errbuf, errsz);
}

void timefmt_format_time(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void timefmt_format_duration(long secs, char *buf, size_t n)
{
    if (secs == 0)
    {
        snprintf(buf, n, "0s");
        return;
    }
    int neg = secs < 0;
    if (neg)
        secs = -secs;

    long d = secs / 86400;
    long h = (secs % 86400) / 3600;
    long m = (secs % 3600) / 60;
    long s = secs % 60;

    size_t off = 0;
    if (d > 0)
    {
        int ret = snprintf(buf + off, off < n ? n - off : 0, "%ldd ", d);
        if (ret > 0)
            off = (off + (size_t)ret < n) ? off + (size_t)ret : n - 1;
    }
    if (h > 0)
    {
        int ret = snprintf(buf + off, off < n ? n - off : 0, "%ldh ", h);
        if (ret > 0)
            off = (off + (size_t)ret < n) ? off + (size_t)ret : n - 1;
    }
    if (m > 0)
    {
        int ret = snprintf(buf + off, off < n ? n - off : 0, "%ldm ", m);
        if (ret > 0)
            off = (off + (size_t)ret < n) ? off + (size_t)ret : n - 1;
    }
    if (s > 0)
    {
        int ret = snprintf(buf + off, off < n ? n - off : 0, "%lds", s);
        if (ret > 0)
            off = (off + (size_t)ret < n) ? off + (size_t)ret : n - 1;
    }

    if (off > 0 && buf[off - 1] == ' ')
        buf[--off] = '\0';
    if (neg)
        snprintf(buf + off, off < n ? n - off : 0, " ago");
}
