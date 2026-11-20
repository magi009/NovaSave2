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
    bool raw_folder;        /* carpeta con archivos sueltos */
    bool zip;               /* ZIP con la partida en la raiz (JKSV o NovaSave2) */
    bool deletable;         /* archivo propio de NovaSave2 */
    char tag[16];           /* EDITADOS, CARPETA, JKSV, CHECKPOINT, ZIP... */
} BackupEntry;

typedef struct {
    const char *root;          /* sdmc:/switch/NovaSave2/backups */
    const char *imports;       /* .../importar/<TID> */
    const char *edits;         /* .../editados */
    const char *jksv;          /* sdmc:/JKSV */
    const char *checkpoint;    /* sdmc:/switch/Checkpoint/saves */
    const char *title_name;    /* nombre del juego, para las carpetas de JKSV */
    uint64_t title;
} CatalogSources;

bool catalog_load(const CatalogSources *sources, BackupEntry **entries, size_t *count);

#endif
