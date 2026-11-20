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

typedef enum { Nsv2Entry, FolderEntry, ZipEntry } EntryKind;

static bool add_entry(Catalog *catalog, const char *path, const char *name, const char *folder,
                      EntryKind kind, const char *tag, bool deletable)
{
    bool raw_folder = kind == FolderEntry;
    bool modified = kind != Nsv2Entry;
    SaveIdentity source = { .title = catalog->title };
    if (kind == Nsv2Entry) {
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
    entry->zip = kind == ZipEntry;
    entry->deletable = deletable;
    snprintf(entry->tag, sizeof entry->tag, "%s", tag);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    snprintf(entry->name, sizeof entry->name, "%s", name);
    snprintf(entry->folder, sizeof entry->folder, "%s", folder);
    return true;
}

static bool add_backup(Catalog *catalog, const char *path, const char *name,
                       const char *folder, bool raw_folder)
{
    const char *tag = !raw_folder ? "" : !strcmp(folder, "editados") ? "EDITADOS" : "CARPETA";
    return add_entry(catalog, path, name, folder, raw_folder ? FolderEntry : Nsv2Entry, tag, !raw_folder);
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t length = strlen(name), tail = strlen(suffix);
    return length > tail && !strcasecmp(name + length - tail, suffix);
}

/* Carpeta del juego en backups: "Nombre [TID]" o "TID". */
static bool own_game_folder(const char *folder, uint64_t title)
{
    char id[20];
    snprintf(id, sizeof id, "%016" PRIX64, title);
    if (!strcasecmp(folder, id))
        return true;
    snprintf(id, sizeof id, "[%016" PRIX64 "]", title);
    return has_suffix(folder, id);
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
            } else if (mode == Archives && has_suffix(name, ".zip")) {
                contents->archives = true;
                if (own_game_folder(game_folder, catalog->title) &&
                    !add_entry(catalog, child, name, game_folder, ZipEntry, "ZIP", true))
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

/* Compara nombres de juego ignorando mayusculas, espacios y simbolos. */
static void plain_name(char *out, size_t capacity, const char *name)
{
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && used + 1 < capacity; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z'))
            out[used++] = (char)*p;
        else if (*p >= 'A' && *p <= 'Z')
            out[used++] = (char)(*p - 'A' + 'a');
    out[used] = '\0';
}

/* Respaldos de otra herramienta: cada subcarpeta es una partida y cada .zip tambien. */
static bool add_tool_folder(Catalog *catalog, const char *path, const char *label, const char *tag,
                            bool zips)
{
    DIR *dir = opendir(path);
    if (!dir)
        return errno == ENOENT;
    bool complete = true;
    struct dirent *entry;
    char child[BackupPathSize], zip_tag[24];
    snprintf(zip_tag, sizeof zip_tag, "%s ZIP", tag);
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= BackupNameSize)
            continue;
        int length = snprintf(child, sizeof child, "%s/%s", path, name);
        struct stat info;
        if (length < 0 || (size_t)length >= sizeof child || lstat(child, &info) != 0) {
            complete = false;
            continue;
        }
        if (S_ISDIR(info.st_mode))
            complete = add_entry(catalog, child, name, label, FolderEntry, tag, false) && complete;
        else if (zips && S_ISREG(info.st_mode) && has_suffix(name, ".zip"))
            complete = add_entry(catalog, child, name, label, ZipEntry, zip_tag, false) && complete;
    }
    closedir(dir);
    return complete;
}

/* JKSV: <raiz>/<Nombre del juego>/  o  <raiz>/<TID>/ */
static bool scan_jksv(Catalog *catalog, const char *root, const char *title_name)
{
    DIR *dir = opendir(root);
    if (!dir)
        return errno == ENOENT;
    char wanted[256], found[256], id[17], child[BackupPathSize];
    plain_name(wanted, sizeof wanted, title_name ? title_name : "");
    snprintf(id, sizeof id, "%016" PRIX64, catalog->title);
    bool complete = true;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (name[0] == '.')
            continue;
        plain_name(found, sizeof found, name);
        if (!(wanted[0] && !strcmp(found, wanted)) && strcasecmp(name, id))
            continue;
        int length = snprintf(child, sizeof child, "%s/%s", root, name);
        if (length > 0 && (size_t)length < sizeof child)
            complete = add_tool_folder(catalog, child, name, "JKSV", true) && complete;
    }
    closedir(dir);
    return complete;
}

/* Checkpoint: <raiz>/0x<TID> <Nombre>/<fecha> <usuario>/ */
static bool scan_checkpoint(Catalog *catalog, const char *root)
{
    DIR *dir = opendir(root);
    if (!dir)
        return errno == ENOENT;
    char id[20], child[BackupPathSize];
    snprintf(id, sizeof id, "0x%016" PRIX64, catalog->title);
    bool complete = true;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (strncasecmp(name, id, 18) || (name[18] && name[18] != ' '))
            continue;
        int length = snprintf(child, sizeof child, "%s/%s", root, name);
        if (length > 0 && (size_t)length < sizeof child)
            complete = add_tool_folder(catalog, child, name, "CHECKPOINT", false) && complete;
    }
    closedir(dir);
    return complete;
}

bool catalog_load(const CatalogSources *sources, BackupEntry **entries, size_t *count)
{
    Catalog catalog = { .title = sources->title };
    FolderContents contents = {0};
    bool complete = catalog_walk(&catalog, sources->root, "", 0, Archives, &contents);
    if (sources->imports && !catalog_walk(&catalog, sources->imports, "importar", 0, Imports, &contents))
        complete = false;
    if (sources->edits && !catalog_walk(&catalog, sources->edits, "editados", 0, Edits, &contents))
        complete = false;
    if (sources->jksv && !scan_jksv(&catalog, sources->jksv, sources->title_name))
        complete = false;
    if (sources->checkpoint && !scan_checkpoint(&catalog, sources->checkpoint))
        complete = false;
    if (catalog.count > 1)
        qsort(catalog.entries, catalog.count, sizeof *catalog.entries, compare_backups);
    *entries = catalog.entries;
    *count = catalog.count;
    return complete;
}
