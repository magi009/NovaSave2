#ifndef NOVASAVE_CATALOG_H
#define NOVASAVE_CATALOG_H

#include "archive.h"
#include <stddef.h>

enum { BackupPathSize = 512, BackupNameSize = 256, GameFolderSize = 256 };

typedef struct {
    char path[BackupPathSize];
    char name[BackupNameSize];
    char folder[GameFolderSize];
    SaveIdentity source;
    bool modified;
    bool raw_folder;
} BackupEntry;

bool catalog_load(const char *root, const char *imports, const char *edits, uint64_t title, BackupEntry **entries, size_t *count);

#endif
