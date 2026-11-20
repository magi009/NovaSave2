#ifndef NOVASAVE_BACKUP_LOG_H
#define NOVASAVE_BACKUP_LOG_H

#include <stdint.h>

void backup_log_start(const char *directory, const char *action, uint64_t title);
void backup_log_destination(const char *archive_path);
void backup_log_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
void backup_log_entry(const char *operation, const char *path, uint64_t bytes);
const char *backup_log_finish(const char *status);

#endif
