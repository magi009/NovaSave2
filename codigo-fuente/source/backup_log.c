#define _POSIX_C_SOURCE 200809L
#include "backup_log.h"
#include "storage_error.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { LogPathSize = 800, EntryLimit = 5000 };
static FILE *log_file;
static char log_path[LogPathSize];
static char final_path[LogPathSize];
static unsigned events, entries;
static uint64_t entry_bytes;
static struct timespec started;
static int log_error;

void backup_log_event(const char *format, ...)
{
    if (!log_file || log_error)
        return;
    int saved_errno = errno;
    static char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    for (char *p = message; *p; p++) {
        unsigned char byte = (unsigned char)*p;
        if (byte < 32 || byte == 127)
            *p = '?';
    }
    struct timespec now = started;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed = ((int64_t)now.tv_sec - started.tv_sec) * 1000 +
                      (now.tv_nsec - started.tv_nsec) / 1000000;
    if (fprintf(log_file, "%06u +%" PRId64 " ms %s\n", ++events, elapsed, message) < 0 ||
        fflush(log_file) != 0)
        log_error = errno ? errno : EIO;
    errno = saved_errno;
}

void backup_log_start(const char *directory, const char *action, uint64_t title)
{
    int saved_errno = errno;
    log_path[0] = final_path[0] = '\0';
    events = entries = 0;
    entry_bytes = 0;
    log_error = 0;
    started = (struct timespec){0};
    clock_gettime(CLOCK_MONOTONIC, &started);
    time_t now = time(NULL);
    for (unsigned slot = 0; slot < 1000; slot++) {
        int size = snprintf(log_path, sizeof log_path, "%s/diagnostico-%" PRIdMAX "-%03u.log",
                            directory, (intmax_t)now, slot);
        if (size < 0 || (size_t)size >= sizeof log_path)
            break;
        int fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            break;
        }
        log_file = fdopen(fd, "w");
        if (!log_file) {
            close(fd);
            unlink(log_path);
        }
        break;
    }
    if (log_file)
        backup_log_event("INICIO NovaSave2 0.4.13 operacion=%s juego=%016" PRIX64, action, title);
    else
        log_path[0] = '\0';
    errno = saved_errno;
}

void backup_log_destination(const char *archive_path)
{
    if (!log_file || final_path[0])
        return;
    const char *extension = strrchr(archive_path, '.');
    if (!extension || strcmp(extension, ".nsv2") ||
        (size_t)(extension - archive_path) + sizeof ".log" > sizeof final_path)
        return;
    snprintf(final_path, sizeof final_path, "%.*s.log",
             (int)(extension - archive_path), archive_path);
}

void backup_log_entry(const char *operation, const char *path, uint64_t bytes)
{
    if (!log_file)
        return;
    entry_bytes += bytes;
    if (++entries <= EntryLimit)
        backup_log_event("%s bytes=%" PRIu64 " ruta=%s", operation, bytes, path);
    else if (entries == EntryLimit + 1)
        backup_log_event("Limite de detalle por entrada alcanzado. Continuan los errores y el total.");
}

const char *backup_log_finish(const char *status)
{
    if (!log_file)
        return NULL;
    backup_log_event("FIN entradas=%u bytes_de_entradas=%" PRIu64 " mensaje=%s",
                     entries, entry_bytes, status);
    int close_result = fclose(log_file);
    log_file = NULL;
    if (close_result != 0 || log_error)
        return NULL;

    struct stat existing;
    if (final_path[0] && lstat(final_path, &existing) != 0 && storage_path_missing(errno) &&
        rename(log_path, final_path) == 0)
        return final_path;
    return log_path;
}
