#include <switch.h>
#include "archive.h"
#include "backup_log.h"
#include "storage_error.h"
#include "catalog.h"
#include "titledata.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define APP_ROOT "sdmc:/switch/NovaSave2"
#define BACKUP_ROOT APP_ROOT "/backups"
#define EDIT_ROOT APP_ROOT "/editados"
#define IMPORT_ROOT APP_ROOT "/importar"
#define SAVE_ROOT "novasave:/"

typedef struct {
    SaveIdentity id;
    AccountUid uid;
    char title[72];
    char profile[40];
} SaveEntry;

static SaveEntry *saves;
static size_t save_count, save_index;
static BackupEntry *backups;
static size_t backup_count, backup_index;
static char status[160];
static char detail[256];
static char log_notice[256];
typedef enum {
    NoAction, BackupAction, ProfileAction, RestoreAction, StagedRestoreAction,
    JournalAction, PrepareAction, DeleteAction
} PendingAction;
typedef struct { SaveEntry save; bool has_save; } TargetProfile;
static TargetProfile profiles[ACC_USER_LIST_SIZE];
static size_t profile_count, profile_index;
static PendingAction pending;
static bool preparing_profile;
static bool confirm_yes;

static void report(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(status, sizeof status, format, args);
    va_end(args);
    detail[0] = '\0';
}

static void finish_backup_log(void)
{
    backup_log_event("PANTALLA mensaje=%s detalle=%s", status, detail);
    const char *path = backup_log_finish(status);
    if (path)
        snprintf(log_notice, sizeof log_notice, "Log: %.245s", path + strlen("sdmc:/"));
    else
        snprintf(log_notice, sizeof log_notice, "No se pudo guardar el log en la microSD.");
}

static void show_message(const char *message)
{
    consoleClear();
    printf("\x1b[36;1mNovaSave2\x1b[0m\n\n%s\n", message);
    consoleUpdate(NULL);
}

static void copy_label(char *dest, size_t capacity, const char *src, size_t length)
{
    size_t i = 0;
    while (i < length && i + 1 < capacity && src[i]) {
        unsigned char byte = (unsigned char)src[i];
        dest[i++] = byte >= 32 && byte != 127 ? (char)byte : '?';
    }
    if (i && i < length && ((unsigned char)src[i] & 0xc0) == 0x80) {
        do {
            i--;
        } while (i && ((unsigned char)src[i] & 0xc0) == 0x80);
    }
    dest[i] = '\0';
}

static const NacpLanguageEntry *title_language(NacpStruct *nacp)
{
    NacpLanguageEntry *language = NULL;
    if (nacp->titles_data_format == 0) {
        if (R_SUCCEEDED(nacpGetLanguageEntry(nacp, &language)) && language && language->name[0])
            return language;
        for (size_t i = 0; i < 16; i++)
            if (nacp->lang_data.lang[i].name[0])
                return &nacp->lang_data.lang[i];
    }
    return NULL;
}

static bool read_title(SaveEntry *save, NsApplicationControlData *control, u64 actual)
{
    if (actual < sizeof control->nacp)
        return false;
    NacpStruct *nacp = &control->nacp;
    if (nacp->titles_data_format == 1) {
        NacpLanguageEntry entries[32] = {0};
        unsigned size = nacp->lang_data.compressed_data.buffer_size;
        if (size > sizeof nacp->lang_data.compressed_data.buffer)
            return false;
        size_t decoded = titledata_inflate(nacp->lang_data.compressed_data.buffer, size,
                                          entries, sizeof entries);
        if (!decoded || decoded % sizeof entries[0])
            return false;
        memcpy(nacp->lang_data.lang, entries, sizeof nacp->lang_data.lang);
        nacp->titles_data_format = 0;
        const NacpLanguageEntry *language = title_language(nacp);
        if (!language) {
            for (size_t i = 16; i < decoded / sizeof entries[0]; i++) {
                if (entries[i].name[0]) {
                    language = &entries[i];
                    break;
                }
            }
        }
        if (!language)
            return false;
        copy_label(save->title, sizeof save->title, language->name, sizeof language->name);
        return true;
    }
    const NacpLanguageEntry *language = title_language(nacp);
    if (!language)
        return false;
    copy_label(save->title, sizeof save->title, language->name, sizeof language->name);
    return true;
}

static const char *known_title(uint64_t id)
{
    switch (id) {
    case 0x0100F43008C44000: return "Pokemon Legends: Z-A";
    case 0x0400486024DEC000: return "The Legend of Zelda: Ocarina of Time";
    case 0x040057C02159A000: return "Nintendo Switch 2 Welcome Tour";
    case 0x040074A01BF12000: return "Donkey Kong Bananza";
    default: return NULL;
    }
}

static bool title_alias(SaveEntry *save, FILE *aliases)
{
    if (!aliases)
        return false;
    rewind(aliases);
    char line[384], id[17];
    snprintf(id, sizeof id, "%016" PRIX64, save->id.title);
    while (fgets(line, sizeof line, aliases)) {
        if (strlen(line) < 18 || line[16] != '=' || strncasecmp(line, id, 16))
            continue;
        char *name = line + 17;
        size_t length = strcspn(name, "\r\n");
        if (!length)
            continue;
        copy_label(save->title, sizeof save->title, name, length);
        return true;
    }
    return false;
}

static void resolve_labels(SaveEntry *save, NsApplicationControlData *control,
                           bool account, FILE *aliases)
{
    snprintf(save->title, sizeof save->title, "Juego %016" PRIX64, save->id.title);
    snprintf(save->profile, sizeof save->profile, "Usuario ...%08" PRIX32,
             (uint32_t)save->id.user_low);
    if (!title_alias(save, aliases)) {
        u64 actual = 0;
        bool named = false;
        if (control) {
            for (unsigned index = 0; index < 2 && !named; index++) {
                memset(control, 0, sizeof *control);
                if (R_SUCCEEDED(nsGetApplicationControlData2(NsApplicationControlSource_Storage,
                        save->id.title, control, sizeof *control, 1, index, &actual, NULL)))
                    named = read_title(save, control, actual);
            }
            if (!named) {
                memset(control, 0, sizeof *control);
                if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                        save->id.title, control, sizeof *control, &actual)))
                    named = read_title(save, control, actual);
            }
        }
        const char *known = named ? NULL : known_title(save->id.title);
        if (known)
            copy_label(save->title, sizeof save->title, known, strlen(known));
    }

    AccountProfile profile;
    if (account && R_SUCCEEDED(accountGetProfile(&profile, save->uid))) {
        AccountProfileBase base;
        if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base)) && base.nickname[0])
            copy_label(save->profile, sizeof save->profile, base.nickname, sizeof base.nickname);
        accountProfileClose(&profile);
    }
}

static int compare_saves(const void *a, const void *b)
{
    const SaveEntry *left = a, *right = b;
    int title = strcmp(left->title, right->title);
    return title ? title : strcmp(left->profile, right->profile);
}

static void load_saves(void)
{
    free(saves);
    saves = NULL;
    save_count = save_index = 0;
    FsSaveDataInfoReader reader;
    Result result = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if (R_FAILED(result)) {
        report("No se pudo listar las partidas (0x%08" PRIX32 ").", (uint32_t)result);
        return;
    }

    bool ns = R_SUCCEEDED(nsInitialize());
    FILE *aliases = fopen(APP_ROOT "/titulos.txt", "rb");
    bool account = R_SUCCEEDED(accountInitialize(AccountServiceType_System));
    NsApplicationControlData *control = ns ? calloc(1, sizeof *control) : NULL;
    bool memory_error = false;
    size_t capacity = 0;
    for (;;) {
        FsSaveDataInfo page[16];
        s64 count = 0;
        result = fsSaveDataInfoReaderRead(&reader, page, sizeof page / sizeof page[0], &count);
        if (R_FAILED(result) || count <= 0)
            break;
        if (count > (s64)(sizeof page / sizeof page[0])) {
            report("La consola devolvio una lista de partidas invalida.");
            goto finish;
        }

        for (s64 i = 0; i < count; i++) {
            const FsSaveDataInfo *info = &page[i];
            if (info->save_data_type != FsSaveDataType_Account ||
                info->save_data_rank != FsSaveDataRank_Primary ||
                !info->application_id || !accountUidIsValid(&info->uid))
                continue;
            if (save_count == capacity) {
                size_t next = capacity ? 2 * capacity : 32;
                SaveEntry *list = realloc(saves, next * sizeof *list);
                if (!list) {
                    memory_error = true;
                    break;
                }
                saves = list;
                capacity = next;
            }
            SaveEntry *save = &saves[save_count++];
            save->id = (SaveIdentity){info->application_id, info->uid.uid[0], info->uid.uid[1]};
            save->uid = info->uid;
            resolve_labels(save, control, account, aliases);
        }
        if (memory_error)
            break;
    }
    if (memory_error)
        report("Memoria insuficiente para completar la lista.");
    else if (R_FAILED(result))
        report("Lista incompleta (0x%08" PRIX32 ").", (uint32_t)result);
    else if (!save_count)
        report("No se encontraron partidas de usuario accesibles.");
    else
        report("%zu partidas de usuario.", save_count);
finish:
    if (save_count > 1)
        qsort(saves, save_count, sizeof *saves, compare_saves);
    free(control);
    if (aliases)
        fclose(aliases);
    if (ns)
        nsExit();
    if (account)
        accountExit();
    fsSaveDataInfoReaderClose(&reader);
}

static void report_storage(const char *operation, const char *path, int system_error, uint32_t native)
{
    backup_log_event("ALMACENAMIENTO paso=%s errno=%d fs=0x%08" PRIX32 " ruta=%s",
                     operation, system_error, native, path);
    report("Fallo al %s. FS 0x%08" PRIX32 ", errno %d.", operation, native, system_error);
    size_t length = strlen(path);
    if (length > 250)
        snprintf(detail, sizeof detail, "...%s", path + length - 247);
    else
        snprintf(detail, sizeof detail, "%s", path);
}

static void report_archive_failure(void)
{
    const ArchiveOutputError *error = archive_output_error();
    if (error->operation)
        report_storage(error->operation, error->path, error->system_error, error->native_error);
    else
        report("No se pudo crear el respaldo.");
}

static bool ensure_dir(const char *path)
{
    backup_log_event("PREPARAR CARPETA ruta=%s", path);
    struct stat details;
    if (stat(path, &details) == 0) {
        if (S_ISDIR(details.st_mode))
            return true;
        report_storage("abrir carpeta", path, ENOTDIR, 0);
        return false;
    }
    if (!storage_path_missing(errno)) {
        report_storage("consultar carpeta", path, errno, storage_native_error(errno));
        return false;
    }
    if (mkdir(path, 0700) == 0)
        return true;
    int system_error = errno;
    uint32_t native = storage_native_error(system_error);
    if (stat(path, &details) == 0 && S_ISDIR(details.st_mode))
        return true;
    report_storage("crear carpeta", path, system_error, native);
    return false;
}

static void game_directory(char *out, size_t capacity, const SaveEntry *save)
{
    char name[sizeof save->title];
    size_t length = strlen(save->title);
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)save->title[i];
        name[i] = byte < 32 || strchr("<>:\"/\\|?*", byte) ? '_' : (char)byte;
    }
    while (length && (name[length - 1] == ' ' || name[length - 1] == '.'))
        length--;
    name[length] = '\0';
    char id[17];
    snprintf(id, sizeof id, "%016" PRIX64, save->id.title);
    if (!name[0] || (!strncmp(name, "Juego ", 6) && !strcmp(name + 6, id)))
        snprintf(out, capacity, BACKUP_ROOT "/%s", id);
    else
        snprintf(out, capacity, BACKUP_ROOT "/%s [%s]", name, id);
}

static void save_folder(char *out, size_t capacity, const SaveEntry *save)
{
    char game_folder[144];
    game_directory(game_folder, sizeof game_folder, save);
    snprintf(out, capacity, "%s/%016" PRIX64 "%016" PRIX64,
             game_folder, save->id.user_high, save->id.user_low);
}

static bool ensure_backup_folder(const SaveEntry *save)
{
    char folder[192], game_folder[144];
    save_folder(folder, sizeof folder, save);
    game_directory(game_folder, sizeof game_folder, save);
    return ensure_dir("sdmc:/switch") &&
           ensure_dir("sdmc:/switch/NovaSave2") &&
           ensure_dir(BACKUP_ROOT) && ensure_dir(game_folder) && ensure_dir(folder);
}

static int available_name(const char *path)
{
    struct stat existing;
    if (stat(path, &existing) == 0)
        return 0;
    if (storage_path_missing(errno))
        return 1;
    report_storage("consultar nombre de salida", path, errno, storage_native_error(errno));
    return -1;
}

static bool new_backup_path(char *out, size_t capacity, const SaveEntry *save, const char *suffix)
{
    const time_t now = time(NULL);
    struct tm date;
    char timestamp[32] = "sin-fecha";
    if (localtime_r(&now, &date))
        strftime(timestamp, sizeof timestamp, "%Y%m%d-%H%M%S", &date);

    char folder[192];
    save_folder(folder, sizeof folder, save);
    for (unsigned i = 0; i < 1000; i++) {
        int n = snprintf(out, capacity, "%s/%s-%03u%s.nsv2",
                         folder, timestamp, i, suffix);
        if (n < 0 || (size_t)n >= capacity)
            return false;
        char partial[BackupPathSize];
        int length = snprintf(partial, sizeof partial, "%s.partial", out);
        if (length < 0 || (size_t)length >= sizeof partial)
            return false;
        int final_state = available_name(out);
        if (final_state < 0)
            return false;
        if (final_state == 0)
            continue;
        int partial_state = available_name(partial);
        if (partial_state < 0)
            return false;
        if (partial_state > 0) {
            char log_path[BackupPathSize];
            snprintf(log_path, sizeof log_path, "%.*s.log", n - 5, out);
            int log_state = available_name(log_path);
            if (log_state < 0)
                return false;
            if (log_state == 0)
                continue;
            backup_log_destination(out);
            backup_log_event("RUTA DE SALIDA ruta=%s", out);
            return true;
        }
    }
    report("No se encontro un nombre de respaldo disponible.");
    return false;
}

static bool same_user(SaveIdentity a, SaveIdentity b)
{
    return a.user_low == b.user_low && a.user_high == b.user_high;
}

static bool add_profile(const SaveEntry *save, bool has_save)
{
    for (size_t i = 0; i < profile_count; i++)
        if (same_user(profiles[i].save.id, save->id))
            return true;
    if (profile_count == ACC_USER_LIST_SIZE)
        return false;
    profiles[profile_count++] = (TargetProfile){*save, has_save};
    return true;
}

static void load_profiles(const SaveEntry *game)
{
    profile_count = profile_index = 0;
    for (size_t i = 0; i < save_count; i++)
        if (saves[i].id.title == game->id.title)
            add_profile(&saves[i], true);

    if (R_SUCCEEDED(accountInitialize(AccountServiceType_System))) {
        AccountUid uids[ACC_USER_LIST_SIZE];
        s32 count = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count))) {
            for (s32 i = 0; i < count && i < ACC_USER_LIST_SIZE; i++) {
                SaveEntry entry = *game;
                entry.uid = uids[i];
                entry.id.user_low = uids[i].uid[0];
                entry.id.user_high = uids[i].uid[1];
                if (!accountUidIsValid(&entry.uid))
                    continue;
                snprintf(entry.profile, sizeof entry.profile, "Usuario ...%08" PRIX32,
                         (uint32_t)entry.id.user_low);
                AccountProfile account;
                if (R_SUCCEEDED(accountGetProfile(&account, entry.uid))) {
                    AccountProfileBase base;
                    if (R_SUCCEEDED(accountProfileGet(&account, NULL, &base)) && base.nickname[0])
                        copy_label(entry.profile, sizeof entry.profile, base.nickname, sizeof base.nickname);
                    accountProfileClose(&account);
                }
                add_profile(&entry, false);
            }
        }
        accountExit();
    }

    for (size_t i = 0; i < profile_count; i++)
        if (same_user(profiles[i].save.id, game->id))
            profile_index = i;
}

static void load_backups(const SaveEntry *save)
{
    free(backups);
    backup_index = 0;
    char imports[120];
    snprintf(imports, sizeof imports, IMPORT_ROOT "/%016" PRIX64, save->id.title);
    bool complete = catalog_load(BACKUP_ROOT, imports, EDIT_ROOT, save->id.title, &backups, &backup_count);
    if (!complete)
        report("Lectura incompleta de respaldos en microSD.");
    else if (!backup_count) {
        report("No se encontraron respaldos del juego en la microSD.");
        snprintf(detail, sizeof detail, "Carpetas editadas: editados/%016" PRIX64 "/<nombre>/",
                 save->id.title);
    } else
        report("%zu respaldos en la microSD.", backup_count);
}

static void open_edits(const SaveEntry *save)
{
    load_backups(save);
    for (size_t i = 0; i < backup_count; i++) {
        if (backups[i].raw_folder) {
            backup_index = i;
            report("Selecciona una carpeta. ZR crea la copia, A la restaura.");
            return;
        }
    }
    report("No se encontraron carpetas con archivos sueltos para este juego.");
    snprintf(detail, sizeof detail, "switch/NovaSave2/editados/%016" PRIX64 "/<nombre>/",
             save->id.title);
}

static void select_created(const char *path)
{
    load_backups(&saves[save_index]);
    for (size_t i = 0; i < backup_count; i++) {
        if (!strcmp(path, backups[i].path)) {
            backup_index = i;
            break;
        }
    }
}

/* Respalda la partida. Si tiene rutas mas largas que el limite de la consola,
 * la monta con escritura y usa el metodo por tramos: mueve carpetas, lee y las
 * devuelve. Nunca confirma: al desmontar, la consola descarta cualquier cambio. */
static unsigned last_backup_tramos;

static bool backup_save(const SaveEntry *save, const char *path, Result *mount_result)
{
    last_backup_tramos = 0;
    archive_set_relocation(false);
    backup_log_event("MONTAR PARTIDA EN SOLO LECTURA ruta=%s", SAVE_ROOT);
    Result result = fsdevMountSaveDataReadOnly("novasave", save->id.title, save->uid);
    backup_log_event("RESULTADO DE MONTAJE fs=0x%08" PRIX32, (uint32_t)result);
    *mount_result = result;
    if (R_FAILED(result))
        return false;
    bool ok = archive_backup(SAVE_ROOT, path, save->id);
    int unmount = fsdevUnmountDevice("novasave");
    backup_log_event("DESMONTAR PARTIDA rc=%d", unmount);
    if (ok || !archive_needs_relocation())
        return ok;

    backup_log_event("RUTAS LARGAS: REINTENTAR POR TRAMOS (escritura sin confirmar)");
    result = fsdevMountSaveData("novasave", save->id.title, save->uid);
    backup_log_event("RESULTADO DE MONTAJE PARA TRAMOS fs=0x%08" PRIX32, (uint32_t)result);
    *mount_result = result;
    if (R_FAILED(result))
        return false;
    archive_set_relocation(true);
    ok = archive_backup(SAVE_ROOT, path, save->id);
    unsigned moved = archive_relocations();
    last_backup_tramos = moved;
    archive_set_relocation(false);
    unmount = fsdevUnmountDevice("novasave");
    backup_log_event("DESMONTAR SIN CONFIRMAR rc=%d tramos=%u", unmount, moved);
    return ok;
}

static void make_backup(const SaveEntry *save)
{
    char path[BackupPathSize];
    if (!ensure_backup_folder(save) || !new_backup_path(path, sizeof path, save, ""))
        return;

    Result result = 0;
    bool ok = backup_save(save, path, &result);
    if (R_FAILED(result)) {
        report("No se pudo abrir la partida (0x%08" PRIX32 ").", (uint32_t)result);
        return;
    }
    if (ok) {
        if (last_backup_tramos)
            report("Respaldo creado y verificado por tramos (rutas largas).");
        else
            report("Respaldo creado y verificado.");
        snprintf(detail, sizeof detail, "Archivo guardado: %.230s", path + strlen("sdmc:/"));
    } else {
        report_archive_failure();
    }
}

static bool restore_mounted(const SaveEntry *save, FILE *input, SaveIdentity source, bool staged)
{
    char recovery[BackupPathSize];
    backup_log_event("RESPALDAR PARTIDA ACTUAL DEL PERFIL DE DESTINO");
    if (!ensure_backup_folder(save) || !new_backup_path(recovery, sizeof recovery, save, "-previo")) {
        report("Cancelado. No se pudo verificar la copia previa del perfil de destino.");
        return false;
    }
    archive_set_relocation(true);
    bool previous_ok = archive_backup(SAVE_ROOT, recovery, save->id);
    backup_log_event("COPIA PREVIA tramos=%u", archive_relocations());
    archive_set_relocation(false);
    if (!previous_ok) {
        report("Cancelado. No se pudo verificar la copia previa del perfil de destino.");
        return false;
    }

    bool committed = false;
    FILE *previous = fopen(recovery, "rb");
    ArchiveRestoreError error;
    if (!previous) {
        report("No se pudo releer la copia previa. Sin confirmar.");
    } else if (!archive_restore_switch(input, source, previous, save->id, "novasave", staged, &error)) {
        backup_log_event("ERROR RESTAURACION paso=%s etapas=%u fs=0x%08" PRIX32 " ruta=%s",
                         error.operation ? error.operation : "verificar", error.stages_committed,
                         error.result, error.path);
        if (error.stages_committed)
            report("PARCIAL: %u etapas guardadas. Fallo al %s (0x%08" PRIX32 ") %.48s",
                   error.stages_committed, error.operation ? error.operation : "verificar",
                   error.result, error.path);
        else if (error.operation)
            report("Sin confirmar. Fallo al %s (0x%08" PRIX32 ") %.65s",
                   error.operation, error.result, error.path);
        else
            report("Sin confirmar. No se pudo verificar el respaldo durante la copia.");
    } else {
        Result result = staged ? 0 : fsdevCommitDevice("novasave");
        committed = R_SUCCEEDED(result);
        if (R_VALUE(result) == MAKERESULT(2, 6706))
            report("2002-6706. Registro lleno. En Confirmar, usa Y Revisar registro.");
        else if (!committed)
            report("No se pudo confirmar la escritura (0x%08" PRIX32 ").", (uint32_t)result);
    }
    if (previous)
        fclose(previous);
    snprintf(detail, sizeof detail, "Copia previa: %.232s", recovery + strlen("sdmc:/"));
    return committed;
}

static FILE *open_verified(const BackupEntry *backup)
{
    backup_log_event("ABRIR NSV2 DE ORIGEN ruta=%s", backup->path);
    FILE *file = fopen(backup->path, "rb");
    bool modified;
    if (!file || !archive_matches(file, backup->source, &modified) ||
        modified != backup->modified || !archive_read(file, backup->source, NULL)) {
        int system_error = !file || ferror(file) ? errno : 0;
        backup_log_event("ERROR al verificar NSV2 de origen errno=%d fs=0x%08" PRIX32 " ruta=%s",
                         system_error, storage_native_error(system_error), backup->path);
        if (file)
            fclose(file);
        report("PELIGRO: respaldo danado, alterado o reemplazado.");
        snprintf(detail, sizeof detail, "Usa archivos editados en editados/ e importalos con ZR.");
        return NULL;
    }
    return file;
}

static bool make_export_folder(char *out, size_t capacity, const BackupEntry *backup)
{
    char parent[128];
    snprintf(parent, sizeof parent, EDIT_ROOT "/%016" PRIX64, backup->source.title);
    if (!ensure_dir(APP_ROOT) || !ensure_dir(EDIT_ROOT) || !ensure_dir(parent))
        return false;

    char name[49];
    copy_label(name, sizeof name, backup->name, strlen(backup->name) - 5);
    for (char *p = name; *p; p++)
        if (strchr("<>:\"/\\|?*", *p))
            *p = '_';
    size_t length = strlen(name);
    while (length && (name[length - 1] == ' ' || name[length - 1] == '.'))
        name[--length] = '\0';

    for (unsigned n = 1; n <= 999; n++) {
        int size = snprintf(out, capacity, "%s/%s-%03u", parent,
                            name[0] ? name : "respaldo", n);
        if (size < 0 || (size_t)size >= capacity)
            return false;
        if (mkdir(out, 0700) == 0)
            return true;
        if (errno != EEXIST)
            return false;
    }
    return false;
}

static void export_backup(const BackupEntry *backup)
{
    FILE *file = open_verified(backup);
    if (!file)
        return;
    char folder[192], root[200];
    if (!make_export_folder(folder, sizeof folder, backup)) {
        report("No se pudo crear la carpeta de salida en la microSD.");
        fclose(file);
        return;
    }
    snprintf(root, sizeof root, "%s/", folder);
    if (archive_read(file, backup->source, root)) {
        select_created(folder);
        report("Desempaquetado: archivos sueltos en la microSD.");
        snprintf(detail, sizeof detail, "%s", folder + strlen("sdmc:/"));
    } else {
        if (archive_clear(root))
            rmdir(folder);
        report("No se pudieron extraer todos los archivos.");
    }
    fclose(file);
}

static void delete_backup(void)
{
    if (unlink(backups[backup_index].path) != 0) {
        report("No se pudo borrar el archivo seleccionado de la microSD.");
        return;
    }
    size_t selected = backup_index;
    load_backups(&saves[save_index]);
    if (backup_count)
        backup_index = selected < backup_count ? selected : backup_count - 1;
    report("Respaldo borrado de la microSD.");
}

static FILE *stage_import(const SaveEntry *save, BackupEntry *backup)
{
    char root[BackupPathSize + 2];
    snprintf(root, sizeof root, "%s/", backup->path);
    backup_log_event("PREPARAR CARPETA IMPORTADA ruta=%s", root);
    if (!ensure_backup_folder(save) ||
        !new_backup_path(backup->path, sizeof backup->path, save, "-importado"))
        return NULL;
    if (!archive_backup_modified(root, backup->path, save->id)) {
        report_archive_failure();
        return NULL;
    }
    backup->source = save->id;
    return open_verified(backup);
}

static void prepare_for_profile(const SaveEntry *save, const BackupEntry *backup)
{
    if (backup->source.title != save->id.title)
        return;
    char path[BackupPathSize];
    if (!ensure_backup_folder(save) || !new_backup_path(path, sizeof path, save, "-perfil"))
        return;
    bool ok;
    if (backup->raw_folder) {
        char root[BackupPathSize + 2];
        snprintf(root, sizeof root, "%s/", backup->path);
        backup_log_event("PREPARAR CARPETA EDITADA ruta=%s", root);
        ok = archive_backup_modified(root, path, save->id);
    } else {
        FILE *input = open_verified(backup);
        if (!input)
            return;
        ok = archive_copy_for_profile(input, backup->source, save->id, path);
        fclose(input);
    }
    if (!ok) {
        report_archive_failure();
        return;
    }
    select_created(path);
    report("Copia NSV2 verificada para %.36s. Lista para restaurar.", save->profile);
    snprintf(detail, sizeof detail, "%.245s", path + strlen("sdmc:/"));
}

static NsApplicationControlData *load_save_control(const SaveEntry *save)
{
    Result result = nsInitialize();
    if (R_FAILED(result)) {
        report("No se pudo leer la configuracion del juego (0x%08" PRIX32 ").", (uint32_t)result);
        return NULL;
    }
    NsApplicationControlData *control = calloc(1, sizeof *control);
    if (!control) {
        nsExit();
        report("Memoria insuficiente para leer la configuracion de la partida.");
        return NULL;
    }

    u64 actual = 0;
    result = nsGetApplicationControlData2(NsApplicationControlSource_Storage,
            save->id.title, control, sizeof *control, 0, 0, &actual, NULL);
    if (R_FAILED(result) || actual < sizeof control->nacp) {
        actual = 0;
        result = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                save->id.title, control, sizeof *control, &actual);
    }
    nsExit();

    if (R_FAILED(result) || actual < sizeof control->nacp) {
        free(control);
        report("No se pudo leer la configuracion de la partida (0x%08" PRIX32 ").",
               (uint32_t)result);
        return NULL;
    }
    return control;
}

static bool create_profile_save(const SaveEntry *save)
{
    NsApplicationControlData *control = load_save_control(save);
    if (!control)
        return false;
    const NacpStruct *nacp = &control->nacp;
    u64 size = nacp->user_account_save_data_size;
    u64 journal = nacp->user_account_save_data_journal_size;
    bool valid = size > 0 && size <= INT64_MAX && journal <= INT64_MAX && nacp->save_data_owner_id;
    if (!valid) {
        free(control);
        report("El juego no dio los datos para crear la partida. Guarda una vez con ese perfil.");
        return false;
    }
    FsSaveDataAttribute attr = {
        .application_id = save->id.title,
        .uid = save->uid,
        .save_data_type = FsSaveDataType_Account,
        .save_data_rank = FsSaveDataRank_Primary,
    };
    FsSaveDataCreationInfo info = {
        .save_data_size = (s64)size,
        .journal_size = (s64)journal,
        .available_size = 0x4000,
        .owner_id = nacp->save_data_owner_id,
        .save_data_space_id = FsSaveDataSpaceId_User,
    };
    const FsSaveDataMetaInfo meta = { .size = 0x40060, .type = FsSaveDataMetaType_Thumbnail };
    free(control);
    Result result = fsCreateSaveDataFileSystem(&attr, &info, &meta);
    if (R_FAILED(result)) {
        report("No se pudo crear la partida (0x%08" PRIX32 "). Guarda una vez con ese perfil.",
               (uint32_t)result);
        return false;
    }
    return true;
}

typedef struct {
    u64 save_id;
    s64 data_size;
    s64 current_journal;
    s64 requested_journal;
} JournalPlan;

static JournalPlan journal_plan;

static bool read_journal(const SaveEntry *save, JournalPlan *plan)
{
    const FsSaveDataFilter filter = {
        .filter_by_application_id = true,
        .filter_by_save_data_type = true,
        .filter_by_user_id = true,
        .save_data_rank = FsSaveDataRank_Primary,
        .attr = {
            .application_id = save->id.title,
            .uid = save->uid,
            .save_data_type = FsSaveDataType_Account,
        },
    };
    FsSaveDataInfoReader reader;
    Result rc = fsOpenSaveDataInfoReaderWithFilter(&reader, FsSaveDataSpaceId_User, &filter);
    if (R_FAILED(rc)) {
        report("No se pudo localizar la partida (0x%08" PRIX32 ").", (uint32_t)rc);
        return false;
    }
    FsSaveDataInfo info[2];
    s64 count = 0;
    rc = fsSaveDataInfoReaderRead(&reader, info, 2, &count);
    fsSaveDataInfoReaderClose(&reader);
    if (R_FAILED(rc) || count != 1 || !info[0].save_data_id ||
        info[0].application_id != save->id.title || info[0].save_data_type != FsSaveDataType_Account ||
        info[0].save_data_rank != FsSaveDataRank_Primary ||
        info[0].uid.uid[0] != save->uid.uid[0] || info[0].uid.uid[1] != save->uid.uid[1]) {
        report("La partida del perfil no se pudo identificar de forma unica (0x%08" PRIX32 ").", (uint32_t)rc);
        return false;
    }
    FsSaveDataExtraData extra;
    rc = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof extra,
                                                          FsSaveDataSpaceId_User, info[0].save_data_id);
    if (R_FAILED(rc) || extra.data_size <= 0 || extra.journal_size < 0) {
        report("No se pudo leer el tamano del registro (0x%08" PRIX32 ").", (uint32_t)rc);
        return false;
    }
    *plan = (JournalPlan){.save_id = info[0].save_data_id, .data_size = extra.data_size,
                          .current_journal = extra.journal_size};
    return true;
}

static bool plan_journal(const SaveEntry *save, JournalPlan *plan)
{
    if (!read_journal(save, plan))
        return false;
    NsApplicationControlData *control = load_save_control(save);
    if (!control)
        return false;
    u64 limit = control->nacp.user_account_save_data_journal_size_max;
    u64 base = control->nacp.user_account_save_data_journal_size;
    free(control);
    if (limit < base)
        limit = base;
    if (limit > INT64_MAX || limit <= (u64)plan->current_journal) {
        report("Registro: %" PRIu64 " bytes. Limite del juego: %" PRIu64 " bytes.",
               (uint64_t)plan->current_journal, (uint64_t)limit);
        snprintf(detail, sizeof detail, "El juego no ofrece un aumento de registro utilizable.");
        return false;
    }
    plan->requested_journal = (s64)limit;
    return true;
}

static void extend_journal(const SaveEntry *save, const JournalPlan *expected)
{
    JournalPlan current;
    if (!plan_journal(save, &current))
        return;
    if (current.save_id != expected->save_id || current.data_size != expected->data_size ||
        current.current_journal != expected->current_journal ||
        current.requested_journal != expected->requested_journal) {
        report("La partida o su configuracion cambio. Abre de nuevo la opcion de registro.");
        return;
    }
    char recovery[BackupPathSize];
    if (!ensure_backup_folder(save) ||
        !new_backup_path(recovery, sizeof recovery, save, "-antes-de-ampliar")) {
        report("No se pudo preparar la copia previa en la microSD.");
        return;
    }
    Result rc = 0;
    bool saved = backup_save(save, recovery, &rc);
    if (R_FAILED(rc)) {
        report("No se pudo abrir el destino para respaldarlo (0x%08" PRIX32 ").", (uint32_t)rc);
        return;
    }
    if (!saved) {
        report("Cancelado. No se pudo respaldar o desmontar la partida.");
        return;
    }
    rc = fsExtendSaveDataFileSystem(FsSaveDataSpaceId_User, current.save_id,
                                   current.data_size, current.requested_journal);
    if (R_FAILED(rc)) {
        report("La consola no amplio el registro (0x%08" PRIX32 ").", (uint32_t)rc);
    } else {
        JournalPlan updated;
        if (!read_journal(save, &updated) || updated.save_id != current.save_id ||
            updated.current_journal < current.requested_journal || updated.data_size < current.data_size)
            report("La consola acepto la ampliacion, pero no pude verificar el tamano.");
        else
            report("Registro verificado: %" PRIu64 " bytes. Pulsa A para restaurar.",
                   (uint64_t)updated.current_journal);
    }
    snprintf(detail, sizeof detail, "Copia previa: %.232s", recovery + strlen("sdmc:/"));
}

static void verify_committed(const SaveEntry *save, FILE *input, SaveIdentity source)
{
    char recovery[sizeof detail];
    snprintf(recovery, sizeof recovery, "%s", detail);
    archive_set_relocation(false);
    Result result = fsdevMountSaveDataReadOnly("novasave", save->id.title, save->uid);
    bool matches = false;
    if (R_SUCCEEDED(result)) {
        matches = archive_verify_restored(input, source, SAVE_ROOT);
        fsdevUnmountDevice("novasave");
        if (!matches && archive_needs_relocation()) {
            backup_log_event("RELEER POR TRAMOS (escritura sin confirmar)");
            result = fsdevMountSaveData("novasave", save->id.title, save->uid);
            if (R_SUCCEEDED(result)) {
                archive_set_relocation(true);
                matches = archive_verify_restored(input, source, SAVE_ROOT);
                archive_set_relocation(false);
                fsdevUnmountDevice("novasave");
            }
        }
    }
    if (R_FAILED(result)) {
        report("Escritura confirmada. Fallo al releer el destino (0x%08" PRIX32 ").",
               (uint32_t)result);
    } else {
        if (matches)
            report("Guardado y verificado en %.36s.", save->profile);
        else
            report("Escritura confirmada. Los archivos releidos no coinciden con el respaldo.");
    }
    snprintf(detail, sizeof detail, "%s", recovery);
}

static void restore_backup(const TargetProfile *profile, const BackupEntry *selected, bool staged)
{
    const SaveEntry *save = &profile->save;
    if (selected->source.title != save->id.title) {
        report("El respaldo corresponde a otro juego.");
        return;
    }
    BackupEntry backup = *selected;
    FILE *input = backup.raw_folder ? stage_import(save, &backup) : open_verified(&backup);
    if (!input)
        return;
    Result result = fsdevMountSaveData("novasave", save->id.title, save->uid);
    // FS PathNotFound (module 2, description 1).
    if (!profile->has_save && R_VALUE(result) == MAKERESULT(2, 1)) {
        if (!create_profile_save(save)) {
            fclose(input);
            return;
        }
        result = fsdevMountSaveData("novasave", save->id.title, save->uid);
    }
    if (R_FAILED(result)) {
        report("No se pudo abrir el perfil de destino para escribir (0x%08" PRIX32 ").", (uint32_t)result);
    } else {
        bool committed = restore_mounted(save, input, backup.source, staged);
        fsdevUnmountDevice("novasave");
        if (committed)
            verify_committed(save, input, backup.source);
    }
    fclose(input);
}

static size_t select_index(size_t selected, size_t count, u64 keys)
{
    if ((keys & HidNpadButton_Up) && selected > 0)
        return selected - 1;
    if ((keys & HidNpadButton_Down) && selected + 1 < count)
        return selected + 1;
    return selected;
}

static void draw_confirmation(const char *question)
{
    printf("%s  %s\nIzq/Der Elegir   A Confirmar   B Volver\n",
           question, confirm_yes ? "No    [Si]" : "[No]    Si");
}

static void draw(bool browsing)
{
    consoleClear();
    printf("\x1b[36;1mNovaSave2\x1b[0m   Partidas guardadas\n\n");
    if (pending == ProfileAction) {
        printf("%s\n\n", preparing_profile ? "Crear copia NSV2 para un perfil" : "Selecciona el perfil de destino");
        for (size_t i = 0; i < profile_count; i++) {
            const TargetProfile *profile = &profiles[i];
            printf("%s %.36s  (...%08" PRIX32 ")%s\n", i == profile_index ? ">" : " ",
                   profile->save.profile, (uint32_t)profile->save.id.user_low,
                   profile->has_save ? "" : "  [Crear partida]");
        }
        printf("\nA Elegir perfil   B Volver\n");
    } else if (browsing) {
        printf("%.60s  /  %.24s\n", saves[save_index].title, saves[save_index].profile);
        if (backup_count) {
            const char *folder = backups[backup_index].folder;
            char label[GameFolderSize];
            copy_label(label, sizeof label, folder, strlen(folder));
            printf("Carpeta del juego en microSD: %.56s\n", label[0] ? label : "backups");
        }
        printf("\n");
        size_t start = backup_index / 10 * 10;
        for (size_t i = start; i < backup_count && i < start + 10; i++) {
            char label[BackupNameSize];
            copy_label(label, sizeof label, backups[i].name, strlen(backups[i].name));
            const BackupEntry *entry = &backups[i];
            const char *tag = "";
            if (entry->raw_folder)
                tag = !strcmp(entry->folder, "editados") ? "  [EDITADOS]" : "  [CARPETA]";
            else if (entry->modified)
                tag = "  [MODIFICADO]";
            printf("%s  %.56s%s\n", i == backup_index ? ">" : " ", label, tag);
        }
        if (backup_count) {
            char selected[BackupNameSize];
            const char *name = backups[backup_index].name;
            copy_label(selected, sizeof selected, name, strlen(name));
            printf("\nArchivo seleccionado (%zu/%zu)\n%s\n",
                   backup_index + 1, backup_count, selected);
        }
        if (pending == DeleteAction) {
            printf("\n\x1b[31;1mBorrar permanentemente este archivo de la microSD.\x1b[0m\n"
                   "A Borrar   B Cancelar\n");
        } else if (pending == PrepareAction) {
            printf("\nCrear copia para %.36s en la microSD.\n"
                   "El juego podria rechazar los datos al recuperarlos.\n",
                   profiles[profile_index].save.profile);
            draw_confirmation("Crear esta copia?");
        } else if (pending == JournalAction) {
            printf("\nAumentar el registro de %.36s en la memoria interna.\n"
                   "Actual: %" PRIu64 " bytes. Nuevo: %" PRIu64 " bytes.\n"
                   "El aumento es permanente. Primero se hara una copia previa.\n"
                   "A Ampliar registro   B Cancelar\n",
                   profiles[profile_index].save.profile, (uint64_t)journal_plan.current_journal,
                   (uint64_t)journal_plan.requested_journal);
        } else if (pending == RestoreAction || pending == StagedRestoreAction) {
            const SaveEntry *target = &profiles[profile_index].save;
            printf("\nDestino: %.36s  (...%08" PRIX32 ")\n", target->profile,
                   (uint32_t)target->id.user_low);
            if (backups[backup_index].raw_folder ||
                !same_user(target->id, backups[backup_index].source))
                printf("El juego podria rechazar datos importados o de otro perfil.\n");
            if (backups[backup_index].modified)
                printf("\n\x1b[31;1mRespaldo modificado: se intentara guardar en el perfil elegido.\n"
                       "El juego podria rechazar los datos o perder progreso.\x1b[0m\n");
            if (!profiles[profile_index].has_save)
                printf("Se pedira crear la partida para este perfil si falta.\n");
            printf("\nSe guardara una copia previa antes de escribir.\n");
            if (pending == StagedRestoreAction)
                printf("Si una etapa falla, pueden quedar cambios parciales.\n");
            else
                printf("X Por etapas   Y Revisar registro\n");
            draw_confirmation("Restaurar esta copia?");
        } else if (backup_count && backups[backup_index].raw_folder) {
            printf("\nA Restaurar carpeta   ZR Guardar copia NSV2   B Volver\n");
        } else if (backup_count) {
            printf("\nA Restaurar   Y Desempaquetar   ZR Crear copia para perfil\nX Borrar   B Volver\n");
        } else {
            printf("\nB Volver\n");
        }
    } else {
        size_t start = save_index / 10 * 10;
        for (size_t i = start; i < save_count && i < start + 10; i++)
            printf("%s  %.46s   /   %.24s\n", i == save_index ? ">" : " ",
                   saves[i].title, saves[i].profile);
        if (save_count)
            printf("\n%zu/%zu   ID %016" PRIX64 "   Usuario ...%08" PRIX32 "\n",
                   save_index + 1, save_count, saves[save_index].id.title,
                   (uint32_t)saves[save_index].id.user_low);
        if (pending == BackupAction) {
            printf("\nCrear respaldo en la microSD de %.46s / %.24s.\n"
                   "Si la copia falla, abre el registro para ver el motivo.\n",
                   saves[save_index].title, saves[save_index].profile);
            draw_confirmation("Crear este respaldo?");
        } else {
            printf("\nA Respaldar   X Respaldos   Y Actualizar   ZR Abrir editados\n");
        }
    }
    printf("\n%s\n", status);
    if (detail[0])
        printf("%s\n", detail);
    if (log_notice[0])
        printf("%s\n", log_notice);
    consoleUpdate(NULL);
}

int main(void)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    PadRepeater repeat;
    padRepeaterInitialize(&repeat, 18, 4);
    show_message("Leyendo partidas...");
    load_saves();
    bool browsing = false;
    draw(browsing);
    while (appletMainLoop()) {
        svcSleepThread(12000000);
        padUpdate(&pad);
        u64 keys = padGetButtonsDown(&pad);
        u64 direction = padGetButtons(&pad) & (HidNpadButton_Up | HidNpadButton_Down);
        if (direction == (HidNpadButton_Up | HidNpadButton_Down) || (pending != NoAction && pending != ProfileAction))
            direction = 0;
        padRepeaterUpdate(&repeat, direction);
        u64 movement = direction & (keys | padRepeaterGetButtons(&repeat));
        if (keys & HidNpadButton_Plus)
            break;
        if (!keys && !movement)
            continue;
        log_notice[0] = '\0';
        PendingAction previous = pending;

        if (pending == ProfileAction) {
            if (keys & HidNpadButton_B) {
                pending = NoAction;
                report("Selecciona el respaldo que quieres restaurar.");
            } else if ((keys & HidNpadButton_A) && profile_count) {
                pending = preparing_profile ? PrepareAction : RestoreAction;
                report("Confirma el archivo y el perfil de destino.");
            } else {
                profile_index = select_index(profile_index, profile_count, movement);
            }
        } else if (pending != NoAction) {
            bool needs_confirmation = pending == BackupAction || pending == PrepareAction ||
                                      pending == RestoreAction || pending == StagedRestoreAction;
            if (keys & HidNpadButton_B) {
                if (pending == StagedRestoreAction || pending == JournalAction)
                    pending = RestoreAction;
                else
                    pending = (pending == RestoreAction || pending == PrepareAction) ? ProfileAction : NoAction;
            } else if (pending == RestoreAction && (keys & HidNpadButton_X)) {
                pending = StagedRestoreAction;
            } else if (pending == RestoreAction && (keys & HidNpadButton_Y)) {
                if (plan_journal(&profiles[profile_index].save, &journal_plan)) {
                    pending = JournalAction;
                    report("Revisa los tamanos antes de confirmar.");
                }
            } else if (needs_confirmation && (keys & (HidNpadButton_Left | HidNpadButton_Right))) {
                confirm_yes = (keys & HidNpadButton_Left) == 0;
            } else if (keys & HidNpadButton_A) {
                if (needs_confirmation && !confirm_yes) {
                    report("Elige Si y despues pulsa A para continuar.");
                    draw(browsing);
                    continue;
                }
                if (pending == JournalAction) {
                    show_message("Respaldando y solicitando la ampliacion del registro...");
                    Result lock = appletLockExit();
                    if (R_SUCCEEDED(lock)) {
                        backup_log_start(APP_ROOT, "respaldo antes de ampliar", profiles[profile_index].save.id.title);
                        extend_journal(&profiles[profile_index].save, &journal_plan);
                        finish_backup_log();
                        appletUnlockExit();
                    } else {
                        report("No se pudo iniciar la ampliacion (0x%08" PRIX32 ").", (uint32_t)lock);
                    }
                    pending = RestoreAction;
                    confirm_yes = false;
                    draw(browsing);
                    continue;
                }
                if (pending == DeleteAction) {
                    delete_backup();
                } else if (pending == BackupAction) {
                    show_message("Copiando y verificando la partida...");
                    backup_log_start(APP_ROOT, "respaldo normal", saves[save_index].id.title);
                    backup_log_event("RESPALDO confirmado por el usuario");
                    make_backup(&saves[save_index]);
                    finish_backup_log();
                } else if (pending == PrepareAction) {
                    show_message("Creando una copia NSV2 para el perfil elegido...");
                    backup_log_start(APP_ROOT, "copia para otro perfil", profiles[profile_index].save.id.title);
                    backup_log_event("RESPALDO confirmado por el usuario");
                    prepare_for_profile(&profiles[profile_index].save, &backups[backup_index]);
                    finish_backup_log();
                } else {
                    show_message(pending == StagedRestoreAction ?
                                 "Restaurando por etapas. Puede tardar varios minutos..." :
                                 "Verificando respaldo y guardando la partida actual...");
                    Result lock = appletLockExit();
                    if (R_SUCCEEDED(lock)) {
                        backup_log_start(APP_ROOT, "importar o restaurar y guardar copia previa",
                                         profiles[profile_index].save.id.title);
                        backup_log_event("RESTAURACION confirmada por el usuario modo=%s",
                                         pending == StagedRestoreAction ? "por etapas" : "normal");
                        restore_backup(&profiles[profile_index], &backups[backup_index],
                                       pending == StagedRestoreAction);
                        finish_backup_log();
                        appletUnlockExit();
                    } else {
                        report("La consola no permitio iniciar la operacion (0x%08" PRIX32 ").",
                               (uint32_t)lock);
                    }
                    browsing = false;
                }
                pending = NoAction;
            }
        } else if (browsing) {
            if (keys & HidNpadButton_B) {
                browsing = false;
            } else if (backup_count && (keys & (HidNpadButton_A | HidNpadButton_ZR))) {
                const BackupEntry *backup = &backups[backup_index];
                FILE *check = NULL;
                if (!backup->raw_folder) {
                    show_message("Comprobando el archivo...");
                    check = open_verified(backup);
                }
                if (check || backup->raw_folder) {
                    if (check)
                        fclose(check);
                    preparing_profile = (keys & HidNpadButton_ZR) != 0;
                    load_profiles(&saves[save_index]);
                    pending = ProfileAction;
                    report(preparing_profile ? "Elige el perfil para la nueva copia NSV2." :
                                              "Elige donde se escribira la partida.");
                }
            } else if (backup_count && !backups[backup_index].raw_folder &&
                       (keys & HidNpadButton_X)) {
                pending = DeleteAction;
            } else if (backup_count && !backups[backup_index].raw_folder &&
                       (keys & HidNpadButton_Y)) {
                show_message("Extrayendo los archivos del respaldo...");
                export_backup(&backups[backup_index]);
            } else {
                backup_index = select_index(backup_index, backup_count, movement);
            }
        } else if (keys & HidNpadButton_Y) {
            show_message("Leyendo partidas...");
            load_saves();
        } else if ((keys & HidNpadButton_X) && save_count) {
            load_backups(&saves[save_index]);
            browsing = true;
        } else if ((keys & HidNpadButton_A) && save_count) {
            pending = BackupAction;
            report("Confirma el juego y el perfil que quieres respaldar.");
        } else if ((keys & HidNpadButton_ZR) && save_count) {
            open_edits(&saves[save_index]);
            browsing = true;
        } else {
            save_index = select_index(save_index, save_count, movement);
        }
        if (pending != previous)
            confirm_yes = false;
        draw(browsing);
    }
    free(backups);
    free(saves);
    consoleExit(NULL);
    return 0;
}
