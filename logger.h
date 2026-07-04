#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

// ANSI Colour Escape Sequences
#define ANSI_COLOUR_RED "\x1b[31m"
#define ANSI_COLOUR_GREEN "\x1b[32m"
#define ANSI_COLOUR_BLUE "\x1b[34m"
#define ANSI_COLOUR_RESET "\x1b[0m"
#define ANSI_BOLD "\x1b[1m"

// Helper function to get current time
static inline void get_timestamp(char *buffer, size_t max_len)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, max_len, "%Y-%m-%d %H:%M:%S", tm_info);
}

static inline void LOG_ERROR(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_RED "[%s] [ERROR] " ANSI_COLOUR_RESET, ts);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static inline void LOG_SUCCESS(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_GREEN "[%s] [SUCCESS] " ANSI_COLOUR_RESET, ts);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static inline void LOG_INFO(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_BLUE "[%s] [INFO] " ANSI_COLOUR_RESET, ts);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static inline void LOG_MSG(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf("[%s] [MSG] ", ts);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

#endif