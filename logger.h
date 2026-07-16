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

// SPEC 3.7: Bookkeeping - Persist to Master Cluster Log
static inline void write_to_disk_log(const char *level, const char *ts, const char *fmt, va_list args)
{
    // POSIX append mode ("a") is atomic enough for our cluster logging needs
    FILE *log_file = fopen("nfs_cluster.log", "a");
    if (log_file)
    {
        fprintf(log_file, "[%s] [%s] ", ts, level);
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fclose(log_file);
    }
}

static inline void LOG_ERROR(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_RED "[%s] [ERROR] " ANSI_COLOUR_RESET, ts);

    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);

    write_to_disk_log("ERROR", ts, fmt, args_copy);

    va_end(args_copy);
    va_end(args);
}

static inline void LOG_SUCCESS(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_GREEN "[%s] [SUCCESS] " ANSI_COLOUR_RESET, ts);

    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);

    write_to_disk_log("SUCCESS", ts, fmt, args_copy);

    va_end(args_copy);
    va_end(args);
}

static inline void LOG_INFO(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf(ANSI_BOLD ANSI_COLOUR_BLUE "[%s] [INFO] " ANSI_COLOUR_RESET, ts);

    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);

    write_to_disk_log("INFO", ts, fmt, args_copy);

    va_end(args_copy);
    va_end(args);
}

static inline void LOG_MSG(const char *fmt, ...)
{
    char ts[20];
    get_timestamp(ts, sizeof(ts));
    printf("[%s] [MSG] ", ts);

    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);

    write_to_disk_log("MSG", ts, fmt, args_copy);

    va_end(args_copy);
    va_end(args);
}

#endif