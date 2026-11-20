#ifndef NOVASAVE_ARCHIVE_H
#define NOVASAVE_ARCHIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t title;
    uint64_t user_low;
    uint64_t user_high;
} SaveIdentity;

typedef struct {
    const char *operation;
    char path[768];
    int system_error;
    uint32_t native_error;
} ArchiveOutputError;

const ArchiveOutputError *archive_output_error(void);
/* Metodo por tramos para rutas mas largas que el limite de la consola.
 * Requiere la partida montada con escritura; nunca confirma por si mismo. */
void archive_set_relocation(bool allowed);
bool archive_needs_relocation(void);
unsigned archive_relocations(void);
bool archive_backup(const char *root, const char *destination, SaveIdentity id);
bool archive_backup_modified(const char *root, const char *destination, SaveIdentity id);
bool archive_identity(FILE *file, SaveIdentity *id, bool *modified);
bool archive_matches(FILE *file, SaveIdentity id, bool *modified);
bool archive_read(FILE *file, SaveIdentity id, const char *restore_root);
#ifdef __SWITCH2__
typedef struct {
    char path[768];
    const char *operation;
    uint32_t result;
    unsigned stages_committed;
} ArchiveRestoreError;

bool archive_restore_switch(FILE *file, SaveIdentity id, FILE *previous, SaveIdentity previous_id,
                            const char *device, bool staged,
                            ArchiveRestoreError *error);
#endif

bool archive_verify_restored(FILE *file, SaveIdentity id, const char *root);
bool archive_copy_for_profile(FILE *file, SaveIdentity source, SaveIdentity target,
                              const char *destination);
bool archive_clear(const char *root);

#endif
