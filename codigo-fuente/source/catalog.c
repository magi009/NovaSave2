#define _POSIX_C_SOURCE 200809L
#include "catalog.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { MaxFolderDepth = 6 };
typedef enum { Archives, Imports, Edits, NamedEdits } CatalogMode;
typedef struct { bool loose_files; bool archives; } FolderContents;

typedef struct {
    uint64_t title;
    BackupEntry *entries;
    size_t count;
    size_t capacity;
} Catalog;

static bool add_backup(Catalog *catalog, const char *path, const char *name,
                       const char *folder, bool raw_folder)
{
    bool modified = raw_folder;
    SaveIdentity source = { .title = catalog->title };
    if (!raw_folder) {
        FILE *file = fopen(path, "rb");
        if (!file)
            return false;
        bool match = archive_identity(file, &source, &modified) && source.title == catalog->title;
        fclose(file);
        if (!match)
            return true;
    }

    if (catalog->count == catalog->capacity) {
        size_t next = catalog->capacity ? 2 * catalog->capacity : 16;
        BackupEntry *entries = realloc(catalog->entries, next * sizeof *entries);
        if (!entries)
            return false;
        catalog->entries = entries;
        catalog->capacity = next;
    }
    BackupEntry *entry = &catalog->entries[catalog->count++];
    entry->source = source;
    entry->modified = modified;
    entry->raw_folder = raw_folder;
    snprintf(entry->path, sizeof entry->path, "%s", path);
    snprintf(entry->name, sizeof entry->name, "%s", name);
    snprintf(entry->folder, sizeof entry->folder, "%s", folder);
    return true;
}

static bool catalog_walk(Catalog *catalog, const char *path, const char *game_folder,
                         unsigned depth, CatalogMode mode, FolderContents *contents)
{
    DIR *dir = opendir(path);
    if (!dir)
        return depth == 0 && errno == ENOENT;
    bool complete = true;
    char title_id[17];
    snprintf(title_id, sizeof title_id, "%016" PRIX64, catalog->title);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
#ifdef __SWITCH2__
            complete = complete && (errno == 0 || errno == ENOENT);
#else
            complete = complete && errno == 0;
#endif
            break;
        }
        const char *name = entry->d_name;
        size_t size = strlen(name);
        if (!strcmp(name, ".") || !strcmp(name, "..") ||
            size >= BackupNameSize || strpbrk(name, ":\\"))
            continue;
        char child[BackupPathSize];
        int length = snprintf(child, sizeof child, "%s/%s", path, name);
        if (length < 0 || (size_t)length >= sizeof child) {
            complete = false;
            continue;
        }
        struct stat info;
        if (lstat(child, &info) != 0) {
            complete = false;
            continue;
        }
        if (S_ISDIR(info.st_mode) && depth < MaxFolderDepth) {
            bool add_folder = mode == NamedEdits ||
                (mode == Edits && size == 49 && name[16] == '-' &&
                 !strncasecmp(name, title_id, 16) &&
                 strspn(name + 17, "0123456789abcdefABCDEF") == 32);
            if (mode == Imports) {
                if (!add_backup(catalog, child, name, game_folder, true))
                    complete = false;
                continue;
            }
            CatalogMode next = mode == Edits && !strcasecmp(name, title_id)
                ? NamedEdits : Archives;
            const char *label = depth == 0 && mode == Archives ? name : game_folder;
            FolderContents child_contents = {0};
            if (!catalog_walk(catalog, child, label, depth + 1, next, &child_contents))
                complete = false;
            contents->loose_files |= child_contents.loose_files;
            contents->archives |= child_contents.archives;
            if (add_folder && child_contents.loose_files && !child_contents.archives &&
                !add_backup(catalog, child, name, game_folder, true))
                complete = false;
        } else if (S_ISREG(info.st_mode)) {
            if (size > 5 && !strcasecmp(name + size - 5, ".nsv2")) {
                contents->archives = true;
                if (!add_backup(catalog, child, name, game_folder, false))
                    complete = false;
            } else {
                contents->loose_files = true;
            }
        }
    }
    if (closedir(dir) != 0)
        complete = false;
    return complete;
}

static int compare_backups(const void *a, const void *b)
{
    const BackupEntry *left = a, *right = b;
    int order = strcmp(right->name, left->name);
    return order ? order : strcmp(left->path, right->path);
}

bool catalog_load(const char *root, const char *imports, const char *edits, uint64_t title, BackupEntry **entries, size_t *count)
{
    Catalog catalog = { .title = title };
    FolderContents contents = {0};
    bool complete = catalog_walk(&catalog, root, "", 0, Archives, &contents);
    if (imports && !catalog_walk(&catalog, imports, "importar", 0, Imports, &contents))
        complete = false;
    if (edits && !catalog_walk(&catalog, edits, "editados", 0, Edits, &contents))
        complete = false;
    if (catalog.count > 1)
        qsort(catalog.entries, catalog.count, sizeof *catalog.entries, compare_backups);
    *entries = catalog.entries;
    *count = catalog.count;
    return complete;
}
