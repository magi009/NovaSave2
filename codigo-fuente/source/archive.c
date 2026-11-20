#define _POSIX_C_SOURCE 200809L
#include "archive.h"
#include "backup_log.h"
#include "storage_error.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH2__
#include <switch/crypto/sha256.h>
#include <switch/runtime/devices/fs_dev.h>
typedef Sha256Context Hash;
static bool hash_init(Hash *hash) { sha256ContextCreate(hash); return true; }
static bool hash_add(Hash *hash, const void *data, size_t size)
{
    sha256ContextUpdate(hash, data, size);
    return true;
}
static bool hash_final(Hash *hash, unsigned char out[32])
{
    sha256ContextGetHash(hash, out);
    return true;
}
#else
#include <openssl/sha.h>
typedef SHA256_CTX Hash;
static bool hash_init(Hash *hash) { return SHA256_Init(hash) == 1; }
static bool hash_add(Hash *hash, const void *data, size_t size)
{
    return SHA256_Update(hash, data, size) == 1;
}
static bool hash_final(Hash *hash, unsigned char out[32])
{
    return SHA256_Final(out, hash) == 1;
}
#endif

/* Rutas: la consola abre como maximo "/" + 767 caracteres (FS_MAX_PATH 0x301
 * con el NUL). Una partida puede tener entradas mas profundas si el juego movio
 * una carpeta a un lugar hondo. Esas entradas no se pueden abrir por nombre,
 * pero si se puede mover una carpeta intermedia a la raiz. El metodo por tramos
 * mueve la carpeta, trabaja con la ruta corta y la devuelve a su lugar. El NSV2
 * guarda siempre la ruta logica completa (hasta PathCapacity - 1). */
enum { PathCapacity = 4096, MaxDepth = 1024, BufferSize = 65536, MaxHops = 32 };
#ifdef __SWITCH2__
enum { RelativeLimit = FS_MAX_PATH - 2, HopPathSize = FS_MAX_PATH };
#else
enum { RelativeLimit = 0x301 - 2, HopPathSize = 0x301 }; /* mismo limite que la consola */
#endif
enum { End = 0, Directory = 1, RegularFile = 2 };

typedef struct {
    FILE *file;
    Hash hash;
    bool has_file;
    unsigned char buffer[BufferSize];
} Archive;

static ArchiveOutputError output_error;

const ArchiveOutputError *archive_output_error(void)
{
    return &output_error;
}

static bool fail_output(const char *operation, const char *path, int system_error)
{
    uint32_t native = storage_native_error(system_error);
    backup_log_event("ERROR paso=%s errno=%d fs=0x%08" PRIX32 " ruta=%s",
                     operation, system_error, native, path);
    if (!output_error.operation) {
        output_error.operation = operation;
        output_error.system_error = system_error;
        output_error.native_error = native;
        snprintf(output_error.path, sizeof output_error.path, "%s", path);
    }
    return false;
}

static const unsigned char magic[8] = {'N', 'S', 'V', '2', 1, 0, 0, 0};

static void encode(unsigned char *out, uint64_t number, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        out[i] = (unsigned char)number;
        number >>= 8;
    }
}

static uint64_t decode(const unsigned char *data, unsigned size)
{
    uint64_t number = 0;
    while (size > 0)
        number = (number << 8) | data[--size];
    return number;
}

static void header(unsigned char out[32], SaveIdentity id)
{
    memcpy(out, magic, sizeof magic);
    encode(out + 8, id.title, 8);
    encode(out + 16, id.user_low, 8);
    encode(out + 24, id.user_high, 8);
}

static bool write_data(Archive *archive, const void *data, size_t size)
{
    return fwrite(data, 1, size, archive->file) == size &&
           hash_add(&archive->hash, data, size);
}

static bool read_data(Archive *archive, void *data, size_t size)
{
    return fread(data, 1, size, archive->file) == size &&
           hash_add(&archive->hash, data, size);
}

static bool make_path(char *out, const char *root, const char *relative)
{
    int n = snprintf(out, PathCapacity, "%s%s", root, relative);
    return n >= 0 && n < PathCapacity;
}

static bool safe_path(const char *path, size_t size)
{
    size_t start = 0;
    unsigned depth = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i == size || path[i] == '/') {
            size_t span = i - start;
            if (span == 0 || span > 255 || ++depth > MaxDepth ||
                (span == 1 && path[start] == '.') ||
                (span == 2 && path[start] == '.' && path[start + 1] == '.'))
                return false;
            start = i + 1;
        } else if ((unsigned char)path[i] < 32 || path[i] == '\\' || path[i] == ':') {
            return false;
        }
    }
    return true;
}

static bool directory_end(void)
{
#ifdef __SWITCH2__
    return errno == 0 || errno == ENOENT;
#else
    return errno == 0;
#endif
}


/* Metodo por tramos: Route guarda las carpetas movidas a la raiz, en orden. */
typedef struct {
    char home[HopPathSize];
    char temp[HopPathSize];
} Hop;
typedef struct {
    Hop hop[MaxHops];
    unsigned count;
} Route;

static bool relocation_allowed, relocation_needed;
static unsigned relocations_done;

void archive_set_relocation(bool allowed)
{
    relocation_allowed = allowed;
    relocation_needed = false;
    relocations_done = 0;
}

bool archive_needs_relocation(void)
{
    return relocation_needed;
}

unsigned archive_relocations(void)
{
    return relocations_done;
}

/* Ultima barra en work[1..RelativeLimit]: la carpeta que se mueve. */
static size_t hop_cut(const char *work)
{
    size_t cut = 0;
    for (size_t i = 1; i <= RelativeLimit && work[i]; i++)
        if (work[i] == '/')
            cut = i;
    return cut;
}

static Route posix_route;

static bool posix_unshorten(const char *root)
{
    bool ok = true;
    char from[PathCapacity], to[PathCapacity];
    while (posix_route.count) {
        Hop *hop = &posix_route.hop[--posix_route.count];
        if (!make_path(from, root, hop->temp) || !make_path(to, root, hop->home) ||
            rename(from, to) != 0) {
            ok = fail_output("devolver carpeta movida por tramos", hop->home, errno);
            continue;
        }
        backup_log_event("TRAMO DEVOLVER desde=%s a=%s", hop->temp, hop->home);
    }
    return ok;
}

/* Deja en phys una ruta relativa de RelativeLimit caracteres o menos. */
static bool posix_shorten(const char *root, const char *logical, char *phys)
{
    posix_route.count = 0;
    char disk[PathCapacity], target[PathCapacity];
    int length = snprintf(phys, PathCapacity, "%s", logical);
    if (length < 0 || length >= PathCapacity)
        return fail_output("validar ruta logica", logical, ENAMETOOLONG);
    while (strlen(phys) > RelativeLimit) {
        if (!relocation_allowed) {
            relocation_needed = true;
            backup_log_event("RUTA LARGA caracteres=%zu limite=%d: se necesita el metodo por tramos ruta=%s",
                             strlen(phys) + 1, RelativeLimit + 1, logical);
            return fail_output("leer ruta larga (requiere tramos)", logical, ENAMETOOLONG);
        }
        size_t cut = hop_cut(phys);
        if (cut == 0 || posix_route.count == MaxHops) {
            posix_unshorten(root);
            return fail_output("dividir ruta larga en tramos", logical, ENAMETOOLONG);
        }
        Hop *hop = &posix_route.hop[posix_route.count];
        memcpy(hop->home, phys, cut);
        hop->home[cut] = '\0';
        bool found = false;
        for (unsigned n = 0; n < 1000 && !found; n++) {
            struct stat info;
            snprintf(hop->temp, sizeof hop->temp, ".nsv2-tramo-%u", n);
            if (!make_path(target, root, hop->temp))
                break;
            if (lstat(target, &info) == 0)
                continue;
            if (!storage_path_missing(errno)) {
                posix_unshorten(root);
                return fail_output("buscar nombre temporal para tramos", hop->temp, errno);
            }
            found = true;
        }
        if (!found || !make_path(disk, root, hop->home)) {
            posix_unshorten(root);
            return fail_output("buscar nombre temporal para tramos", logical, EEXIST);
        }
        if (rename(disk, target) != 0) {
            int error = errno;
            posix_unshorten(root);
            return fail_output("mover carpeta para leer por tramos", hop->home, error);
        }
        backup_log_event("TRAMO MOVER desde=%s a=%s", hop->home, hop->temp);
        posix_route.count++;
        relocations_done++;
        size_t rest = strlen(phys + cut);
        size_t temp = strlen(hop->temp);
        memmove(phys + temp, phys + cut, rest + 1);
        memcpy(phys, hop->temp, temp);
    }
    return true;
}

typedef struct {
    char **items;
    size_t count, capacity;
} NameList;

static void free_names(NameList *names)
{
    for (size_t i = 0; i < names->count; i++)
        free(names->items[i]);
    free(names->items);
}

static bool add_name(NameList *names, const char *name)
{
    if (names->count == names->capacity) {
        size_t capacity = names->capacity ? names->capacity * 2 : 16;
        char **items = realloc(names->items, capacity * sizeof *items);
        if (!items)
            return false;
        names->items = items;
        names->capacity = capacity;
    }
    char *copy = strdup(name);
    if (!copy)
        return false;
    names->items[names->count++] = copy;
    return true;
}

/* Lee los nombres de una carpeta y la cierra antes de mover nada. */
static bool list_folder(const char *root, const char *relative, NameList *names, char *phys, char *disk)
{
    if (!posix_shorten(root, relative, phys))
        return false;
    bool ok = make_path(disk, root, phys) ||
              fail_output("validar ruta de carpeta de origen", relative, ENAMETOOLONG);
    DIR *dir = ok ? opendir(disk) : NULL;
    if (ok && !dir)
        ok = fail_output("abrir carpeta de origen", relative, errno);
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            ok = directory_end() || fail_output("listar archivos de origen", relative, errno);
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (!add_name(names, entry->d_name))
            ok = fail_output("reservar memoria", relative, ENOMEM);
    }
    if (dir && closedir(dir) != 0)
        ok = fail_output("cerrar carpeta de origen", relative, errno);
    return posix_unshorten(root) && ok;
}

static bool copy_file_into(Archive *archive, const char *disk, const char *child, uint64_t remaining)
{
    FILE *input = fopen(disk, "rb");
    if (input == NULL)
        return fail_output("abrir archivo de origen", child, errno);
    bool ok = true;
    uint64_t offset = 0;
    while (ok && remaining > 0) {
        size_t chunk = remaining < BufferSize ? (size_t)remaining : BufferSize;
        ok = (fread(archive->buffer, 1, chunk, input) == chunk ||
              fail_output("leer archivo de origen", child, ferror(input) ? errno : 0)) &&
             (write_data(archive, archive->buffer, chunk) ||
              fail_output("escribir respaldo en la microSD", child, errno));
        if (ok) {
            remaining -= chunk;
            offset += chunk;
        }
    }
    if (ok && (fgetc(input) != EOF || ferror(input)))
        ok = fail_output("confirmar fin del archivo de origen", child, ferror(input) ? errno : 0);
    if (fclose(input) != 0)
        ok = fail_output("cerrar archivo de origen", child, errno);
    if (!ok)
        backup_log_event("AVANCE ARCHIVO bytes_completados=%" PRIu64 " ruta=%s", offset, child);
    return ok;
}

static bool write_tree(Archive *archive, const char *root, const char *relative)
{
    char *buffers = malloc(3 * PathCapacity);
    if (!buffers)
        return fail_output("reservar memoria", relative, ENOMEM);
    char *child = buffers, *phys = buffers + PathCapacity, *disk = buffers + 2 * PathCapacity;
    NameList names = {0};
    bool ok = list_folder(root, relative, &names, phys, disk);
    for (size_t i = 0; ok && i < names.count; i++) {
        const char *name = names.items[i];
        int length = snprintf(child, PathCapacity, "%s%s%s", relative, relative[0] ? "/" : "", name);
        if (length <= 0 || length >= PathCapacity) {
            ok = fail_output("ruta mas larga que el maximo de NSV2 (4095)", relative, ENAMETOOLONG);
            break;
        }
        if (!safe_path(child, (size_t)length)) {
            ok = fail_output("validar nombre de entrada de origen", child, EINVAL);
            break;
        }
        if (!posix_shorten(root, child, phys)) {
            ok = false;
            break;
        }
        struct stat info;
        if (!make_path(disk, root, phys)) {
            ok = fail_output("validar ruta de entrada de origen", child, ENAMETOOLONG);
        } else if (lstat(disk, &info) != 0) {
            ok = fail_output("examinar entrada de origen", child, errno);
        } else if (info.st_size < 0 || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))) {
            ok = fail_output("examinar tipo de entrada de origen", child, EINVAL);
        } else {
            bool folder = S_ISDIR(info.st_mode);
            uint64_t size = folder ? 0 : (uint64_t)info.st_size;
            backup_log_entry(folder ? "CARPETA" : "COPIAR ARCHIVO", child, size);
            unsigned char record[11] = {folder ? Directory : RegularFile};
            encode(record + 1, (unsigned)length, 2);
            encode(record + 3, size, 8);
            ok = (write_data(archive, record, sizeof record) &&
                  write_data(archive, child, (size_t)length)) ||
                 fail_output("escribir indice en la microSD", child, errno);
            if (ok && !folder) {
                archive->has_file = true;
                ok = copy_file_into(archive, disk, child, size);
            }
            if (!posix_unshorten(root))
                ok = false;
            if (ok && folder)
                ok = write_tree(archive, root, child);
            continue;
        }
        posix_unshorten(root);
    }
    free_names(&names);
    free(buffers);
    return ok;
}

static Archive *open_output(const char *destination, char partial[PathCapacity],
                            SaveIdentity id, bool modified)
{
    output_error = (ArchiveOutputError){0};
    if (!make_path(partial, destination, ".partial")) {
        fail_output("validar la ruta de salida", destination, 0);
        return NULL;
    }
    Archive *archive = calloc(1, sizeof *archive);
    if (!archive) {
        fail_output("reservar memoria", destination, ENOMEM);
        return NULL;
    }
    backup_log_event("CREAR TEMPORAL modificado=%d ruta=%s", modified, partial);
    int fd = open(partial, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fail_output("crear archivo temporal en la microSD", partial, errno);
        free(archive);
        return NULL;
    }
    archive->file = fdopen(fd, "wb");
    unsigned char meta[32];
    backup_log_event("ESCRIBIR ENCABEZADO NSV2 ruta=%s", partial);
    header(meta, id);
    meta[5] = modified;
    if (!archive->file || !hash_init(&archive->hash) || !write_data(archive, meta, sizeof meta)) {
        fail_output("escribir encabezado", partial, errno);
        if (archive->file)
            fclose(archive->file);
        else
            close(fd);
        unlink(partial);
        free(archive);
        return NULL;
    }
    return archive;
}

static bool finish_output(Archive *archive, const char *partial, const char *destination,
                          SaveIdentity id, bool ok)
{
    unsigned char digest[32];
    if (!ok && !output_error.operation)
        fail_output("leer el origen o terminar el respaldo", destination, 0);
    if (ok)
        backup_log_event("FINALIZAR SHA-256 Y VACIAR BUFFER ruta=%s", partial);
    if (ok && (!hash_final(&archive->hash, digest) ||
               fwrite(digest, 1, sizeof digest, archive->file) != sizeof digest ||
               fflush(archive->file) != 0))
        ok = fail_output("terminar de escribir en la microSD", partial, errno);
    if (ok) {
        backup_log_event("SINCRONIZAR TEMPORAL ruta=%s", partial);
        if (fsync(fileno(archive->file)) != 0)
            ok = fail_output("sincronizar la microSD", partial, errno);
    }
    if (fclose(archive->file) != 0)
        ok = fail_output("cerrar el archivo temporal", partial, errno);
    free(archive);
    if (ok) {
        backup_log_event("REABRIR TEMPORAL PARA VERIFICAR ruta=%s", partial);
        FILE *check = fopen(partial, "rb");
        if (!check) {
            ok = fail_output("abrir el respaldo para verificarlo", partial, errno);
        } else {
            if (!archive_read(check, id, NULL))
                ok = fail_output("verificar el respaldo escrito", partial, ferror(check) ? errno : 0);
            fclose(check);
        }
    }
    struct stat existing;
    if (ok) {
        backup_log_event("VERIFICADO; PUBLICAR RESPALDO destino=%s", destination);
        if (lstat(destination, &existing) == 0)
            fail_output("el destino ya existe", destination, EEXIST);
        else if (!storage_path_missing(errno))
            fail_output("consultar el nombre final", destination, errno);
        else if (rename(partial, destination) == 0) {
            backup_log_event("RESPALDO PUBLICADO ruta=%s", destination);
            return true;
        }
        else
            fail_output("publicar el respaldo en la microSD", destination, errno);
    }
    int removed = unlink(partial);
    int remove_error = removed != 0 ? errno : 0;
    backup_log_event("LIMPIAR TEMPORAL rc=%d errno=%d ruta=%s", removed, remove_error, partial);
    return false;
}

static bool backup_tree(const char *root, const char *destination, SaveIdentity id, bool modified)
{
    backup_log_event("LEER ORIGEN modificado=%d origen=%s destino=%s", modified, root, destination);
    char partial[PathCapacity];
    Archive *archive = open_output(destination, partial, id, modified);
    if (!archive)
        return false;
    unsigned char end = End;
    bool ok = write_tree(archive, root, "") &&
              (!modified || archive->has_file || fail_output("la carpeta editada esta vacia", root, 0)) &&
              write_data(archive, &end, sizeof end);
    return finish_output(archive, partial, destination, id, ok);
}

bool archive_backup(const char *root, const char *destination, SaveIdentity id)
{
    return backup_tree(root, destination, id, false);
}

bool archive_backup_modified(const char *root, const char *destination, SaveIdentity id)
{
    return backup_tree(root, destination, id, true);
}

static bool match_bytes(FILE *file, const unsigned char *expected, size_t size)
{
    unsigned char bytes[4096];
    while (size > 0) {
        size_t count = size < sizeof bytes ? size : sizeof bytes;
        if (fread(bytes, 1, count, file) != count || memcmp(bytes, expected, count) != 0)
            return false;
        expected += count;
        size -= count;
    }
    return true;
}

#ifdef __SWITCH2__
typedef struct { char *path; unsigned char type; } SavePath;
typedef struct {
    SavePath *items;
    size_t count, capacity;
} SaveManifest;
#endif

typedef struct {
#ifdef __SWITCH2__
    FsFileSystem *fs;
    SaveManifest *manifest;
    bool staged;
    bool pending_commit;
    ArchiveRestoreError *error;
#else
    int unused;
#endif
} NativeRestore;

#ifdef __SWITCH2__
static Route native_route;

static bool native_error(NativeRestore *restore, const char *operation, const char *path, Result result)
{
    backup_log_event("ERROR RESTAURAR paso=%s fs=0x%08" PRIX32 " ruta=%s", operation, (uint32_t)result, path);
    if (restore->error->operation)
        return false;
    restore->error->operation = operation;
    restore->error->result = result;
    snprintf(restore->error->path, sizeof restore->error->path, "%s", path);
    return false;
}

static bool confirm_stage(NativeRestore *restore, const char *path)
{
    if (!restore->staged)
        return true;
    if (native_route.count) {
        /* Nunca confirmar con una carpeta fuera de su lugar. */
        restore->pending_commit = true;
        return true;
    }
    restore->pending_commit = false;
    Result rc = fsFsCommit(restore->fs);
    if (R_FAILED(rc))
        return native_error(restore, "confirmar etapa", path, rc);
    restore->error->stages_committed++;
    return true;
}

static bool record_path(SaveManifest *manifest, const char *path, unsigned char type)
{
    if (manifest->count == manifest->capacity) {
        size_t capacity = manifest->capacity ? manifest->capacity * 2 : 32;
        SavePath *items = realloc(manifest->items, capacity * sizeof *items);
        if (!items)
            return false;
        manifest->items = items;
        manifest->capacity = capacity;
    }
    char *name = strdup(path);
    if (!name)
        return false;
    manifest->items[manifest->count++] = (SavePath){name, type};
    return true;
}

static bool native_unshorten(NativeRestore *restore)
{
    bool ok = true;
    while (native_route.count) {
        Hop *hop = &native_route.hop[--native_route.count];
        Result rc = fsFsRenameDirectory(restore->fs, hop->temp, hop->home);
        if (R_FAILED(rc)) {
            ok = native_error(restore, "devolver carpeta movida por tramos", hop->home, rc);
            continue;
        }
        backup_log_event("TRAMO DEVOLVER desde=%s a=%s", hop->temp, hop->home);
    }
    if (ok && restore->pending_commit)
        ok = confirm_stage(restore, "tramo");
    return ok;
}

/* Deja en path ("/" + ruta) una ruta que la consola puede abrir. */
static bool native_shorten(NativeRestore *restore, const char *logical, char path[FS_MAX_PATH])
{
    native_route.count = 0;
    char work[PathCapacity];
    int length = snprintf(work, sizeof work, "%s", logical);
    if (length < 0 || length >= PathCapacity)
        return native_error(restore, "validar ruta logica", logical, 0);
    while (strlen(work) > RelativeLimit) {
        size_t cut = hop_cut(work);
        if (cut == 0 || native_route.count == MaxHops) {
            native_unshorten(restore);
            return native_error(restore, "dividir ruta larga en tramos", logical, 0);
        }
        Hop *hop = &native_route.hop[native_route.count];
        memset(hop, 0, sizeof *hop);
        hop->home[0] = '/';
        memcpy(hop->home + 1, work, cut);
        bool found = false;
        Result rc = 0;
        for (unsigned n = 0; n < 1000 && !found; n++) {
            FsDirEntryType kind;
            snprintf(hop->temp, sizeof hop->temp, "/.nsv2-tramo-%u", n);
            rc = fsFsGetEntryType(restore->fs, hop->temp, &kind);
            if (R_SUCCEEDED(rc))
                continue;
            if (R_VALUE(rc) != MAKERESULT(2, 1))
                break;
            found = true;
        }
        if (!found) {
            native_unshorten(restore);
            return native_error(restore, "buscar nombre temporal para tramos", logical, rc);
        }
        rc = fsFsRenameDirectory(restore->fs, hop->home, hop->temp);
        if (R_FAILED(rc)) {
            native_unshorten(restore);
            return native_error(restore, "mover carpeta para escribir por tramos", hop->home, rc);
        }
        backup_log_event("TRAMO MOVER desde=%s a=%s", hop->home, hop->temp);
        native_route.count++;
        size_t rest = strlen(work + cut);
        size_t temp = strlen(hop->temp + 1);
        memmove(work + temp, work + cut, rest + 1);
        memcpy(work, hop->temp + 1, temp);
    }
    /* El bucle deja work con RelativeLimit caracteres o menos. */
    memset(path, 0, FS_MAX_PATH);
    path[0] = '/';
    memcpy(path + 1, work, strlen(work));
    return true;
}

static bool restore_native_at(Archive *archive, NativeRestore *restore, unsigned char type,
                              const char *path, uint64_t size);

static bool restore_native_record(Archive *archive, NativeRestore *restore, unsigned char type,
                                  const char *relative, uint64_t size)
{
    char path[FS_MAX_PATH];
    if (size > INT64_MAX)
        return native_error(restore, "ruta o tamano invalido", relative, 0);
    if (!native_shorten(restore, relative, path))
        return false;
    bool ok = restore_native_at(archive, restore, type, path, size);
    return native_unshorten(restore) && ok;
}

static bool restore_native_at(Archive *archive, NativeRestore *restore, unsigned char type,
                              const char *path, uint64_t size)
{

    FsDirEntryType existing;
    Result rc = fsFsGetEntryType(restore->fs, path, &existing);
    bool exists = R_SUCCEEDED(rc);
    if (!exists && R_VALUE(rc) != MAKERESULT(2, 1))
        return native_error(restore, "consultar destino", path, rc);
    if (type == Directory) {
        if (exists)
            return existing == FsDirEntryType_Dir ||
                   native_error(restore, "comprobar tipo de carpeta", path, 0);
        rc = fsFsCreateDirectory(restore->fs, path);
        if (R_FAILED(rc))
            return native_error(restore, "crear carpeta", path, rc);
        return confirm_stage(restore, path);
    }

    if (!exists) {
        rc = fsFsCreateFile(restore->fs, path, restore->staged ? 0 : (s64)size, 0);
        if (R_FAILED(rc))
            return native_error(restore, "crear archivo", path, rc);
        if (!confirm_stage(restore, path))
            return false;
    }
    FsFile file;
    u32 mode = FsOpenMode_Read | FsOpenMode_Write;
    if (restore->staged)
        mode |= FsOpenMode_Append;
    rc = fsFsOpenFile(restore->fs, path, mode, &file);
    if (R_FAILED(rc))
        return native_error(restore, "abrir archivo", path, rc);

    bool changed = !exists && !restore->staged;
    const char *failed = NULL;
    s64 current_size = restore->staged ? 0 : (s64)size;
    if (exists) {
        rc = fsFileGetSize(&file, &current_size);
        if (R_FAILED(rc) || current_size < 0) {
            failed = "consultar tamano";
        } else if (!restore->staged && current_size != (s64)size) {
            rc = fsFileSetSize(&file, (s64)size);
            changed = true;
            if (R_FAILED(rc))
                failed = "cambiar tamano";
            else
                current_size = (s64)size;
        }
    }
    unsigned char block[4096], current[4096];
    for (uint64_t offset = 0; !failed && offset < size;) {
        size_t count = size - offset < sizeof block ? (size_t)(size - offset) : sizeof block;
        if (!read_data(archive, block, count)) {
            failed = "leer respaldo";
            rc = 0;
            break;
        }
        if (exists && offset + count <= (uint64_t)current_size) {
            u64 read = 0;
            rc = fsFileRead(&file, (s64)offset, current, count, FsReadOption_None, &read);
            if (R_FAILED(rc) || read != count) {
                failed = "leer archivo en destino";
                break;
            }
            if (memcmp(block, current, count) == 0) {
                offset += count;
                continue;
            }
        }
        rc = fsFileWrite(&file, (s64)offset, block, count, FsWriteOption_None);
        if (R_FAILED(rc)) {
            failed = "escribir archivo";
            break;
        }
        changed = true;
        offset += count;
        if (restore->staged) {
            if ((s64)offset > current_size)
                current_size = (s64)offset;
            rc = fsFileFlush(&file);
            fsFileClose(&file);
            if (R_FAILED(rc))
                return native_error(restore, "sincronizar bloque", path, rc);
            if (!confirm_stage(restore, path))
                return false;
            if (offset == size && current_size == (s64)size)
                return true;
            rc = fsFsOpenFile(restore->fs, path, mode, &file);
            if (R_FAILED(rc))
                return native_error(restore, "reabrir archivo", path, rc);
            changed = false;
        }
    }
    if (!failed && restore->staged && current_size > (s64)size) {
        rc = fsFileSetSize(&file, (s64)size);
        changed = true;
        if (R_FAILED(rc))
            failed = "recortar archivo";
    }
    if (!failed && changed) {
        rc = fsFileFlush(&file);
        if (R_FAILED(rc))
            failed = "sincronizar archivo";
    }
    fsFileClose(&file);
    if (failed)
        return native_error(restore, failed, path, rc);
    return !changed || confirm_stage(restore, path);
}

#endif

static bool read_records(Archive *archive, const char *root, Archive *copy, bool compare, NativeRestore *native)
{
#ifndef __SWITCH2__
    (void)native;
#endif
    for (;;) {
        unsigned char type;
        if (!read_data(archive, &type, 1) || (copy && !write_data(copy, &type, 1)))
            return false;
        if (type == End)
            return true;
        unsigned char fields[10];
        char path[PathCapacity], disk[PathCapacity], phys[PathCapacity];
        if ((type != Directory && type != RegularFile) ||
            !read_data(archive, fields, sizeof fields))
            return false;
        size_t length = (size_t)decode(fields, 2);
        uint64_t remaining = decode(fields + 2, 8);
        if (length >= sizeof path || !read_data(archive, path, length) ||
            !safe_path(path, length) || (type == Directory && remaining != 0))
            return false;
        if (copy && (!write_data(copy, fields, sizeof fields) || !write_data(copy, path, length)))
            return false;
        path[length] = '\0';
        if (copy)
            backup_log_entry(type == Directory ? "COPIAR CARPETA NSV2" : "COPIAR ARCHIVO NSV2", path, remaining);
#ifdef __SWITCH2__
        if (native) {
            if (native->manifest) {
                if (!record_path(native->manifest, path, type))
                    return native_error(native, "indexar el respaldo", path, 0);
            } else {
                if (!restore_native_record(archive, native, type, path, remaining))
                    return false;
                continue;
            }
        }
#endif
        FILE *output = NULL;
        bool shortened = false;
        if (root) {
            if (compare) {
                /* Releer la partida: las rutas largas se comparan por tramos. */
                if (!posix_shorten(root, path, phys))
                    return false;
                shortened = true;
                struct stat info;
                bool found = make_path(disk, root, phys) && lstat(disk, &info) == 0;
                bool fits = found && (type == Directory ? S_ISDIR(info.st_mode) :
                                      S_ISREG(info.st_mode) && info.st_size >= 0 &&
                                      (uint64_t)info.st_size == remaining);
                if (fits && type == Directory) {
                    if (!posix_unshorten(root))
                        return false;
                    continue;
                }
                output = fits ? fopen(disk, "rb") : NULL;
                if (!output) {
                    posix_unshorten(root);
                    return false;
                }
            } else if (!make_path(disk, root, path)) {
                return false;
            } else {
                if (type == Directory) {
                    if (mkdir(disk, 0700) != 0)
                        return false;
                    continue;
                }
                int fd = open(disk, O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd < 0)
                    return false;
                output = fdopen(fd, "wb");
                if (!output) {
                    close(fd);
                    return false;
                }
            }
        }
        bool ok = true;
        while (ok && remaining > 0) {
            size_t chunk = remaining < BufferSize ? (size_t)remaining : BufferSize;
            ok = read_data(archive, archive->buffer, chunk) &&
                 (!copy || write_data(copy, archive->buffer, chunk)) &&
                 (!output || (compare ? match_bytes(output, archive->buffer, chunk) :
                                        fwrite(archive->buffer, 1, chunk, output) == chunk));
            remaining -= chunk;
        }
        if (output) {
            if (ok && !compare && (fflush(output) != 0 || fsync(fileno(output)) != 0))
                ok = false;
            if (fclose(output) != 0)
                ok = false;
        }
        if (shortened && !posix_unshorten(root))
            ok = false;
        if (!ok) {
            if (copy)
                fail_output("copiar datos del NSV2 de origen", path,
                            ferror(archive->file) || ferror(copy->file) ? errno : 0);
            return false;
        }
    }
}

static bool valid_header(unsigned char actual[32], SaveIdentity id, bool *modified)
{
    unsigned char expected[32];
    header(expected, id);
    if (actual[5] > 1)
        return false;
    expected[5] = actual[5];
    if (memcmp(actual, expected, sizeof expected) != 0)
        return false;
    if (modified)
        *modified = actual[5] == 1;
    return true;
}

bool archive_identity(FILE *file, SaveIdentity *id, bool *modified)
{
    unsigned char actual[32];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(actual, 1, sizeof actual, file) != sizeof actual)
        return false;
    SaveIdentity source = {decode(actual + 8, 8), decode(actual + 16, 8), decode(actual + 24, 8)};
    if (!source.title || !(source.user_low || source.user_high) ||
        !valid_header(actual, source, modified))
        return false;
    *id = source;
    return true;
}

bool archive_matches(FILE *file, SaveIdentity id, bool *modified)
{
    SaveIdentity source;
    return archive_identity(file, &source, modified) && source.title == id.title &&
           source.user_low == id.user_low && source.user_high == id.user_high;
}

static bool read_archive(FILE *file, SaveIdentity id, const char *restore_root,
                         Archive *copy, bool expected_modified, bool compare, NativeRestore *native)
{
    if (fseek(file, 0, SEEK_SET) != 0)
        return false;
    Archive *archive = calloc(1, sizeof *archive);
    if (!archive)
        return false;
    archive->file = file;
    unsigned char actual[32], expected[32];
    bool modified;
    backup_log_event("COMPROBAR ENCABEZADO NSV2");
    bool ok = hash_init(&archive->hash) &&
              read_data(archive, actual, sizeof actual) &&
              valid_header(actual, id, &modified) &&
              (!copy || modified == expected_modified);
    if (ok) {
        backup_log_event("COMPROBAR ENTRADAS NSV2");
        ok = read_records(archive, restore_root, copy, compare, native);
    }
    if (ok) {
        backup_log_event("COMPARAR SHA-256 Y CONFIRMAR FIN NSV2");
        ok = hash_final(&archive->hash, expected) &&
             fread(actual, 1, sizeof actual, file) == sizeof actual &&
             memcmp(actual, expected, sizeof actual) == 0 &&
             fgetc(file) == EOF && !ferror(file);
    }
    free(archive);
    return ok;
}

bool archive_read(FILE *file, SaveIdentity id, const char *restore_root)
{
    return read_archive(file, id, restore_root, NULL, false, false, NULL);
}

#ifdef __SWITCH2__
static int compare_path(const void *a, const void *b)
{
    return strcmp(((const SavePath *)a)->path, ((const SavePath *)b)->path);
}

static bool sort_manifest(SaveManifest *manifest)
{
    if (manifest->count > 1)
        qsort(manifest->items, manifest->count, sizeof *manifest->items, compare_path);
    for (size_t i = 1; i < manifest->count; i++)
        if (compare_path(&manifest->items[i - 1], &manifest->items[i]) == 0)
            return false;
    return true;
}

static void free_manifest(SaveManifest *manifest)
{
    for (size_t i = 0; i < manifest->count; i++)
        free(manifest->items[i].path);
    free(manifest->items);
}

static bool remove_obsolete(NativeRestore *native, const SaveManifest *old, const SaveManifest *incoming)
{
    for (size_t i = old->count; i > 0; i--) {
        const SavePath *before = &old->items[i - 1];
        const SavePath *after = incoming->count ?
            bsearch(before, incoming->items, incoming->count, sizeof *incoming->items, compare_path) : NULL;
        if (after && before->type == after->type)
            continue;
        char path[FS_MAX_PATH];
        if (!native_shorten(native, before->path, path))
            return false;
        Result rc = before->type == Directory ? fsFsDeleteDirectory(native->fs, path)
                                              : fsFsDeleteFile(native->fs, path);
        if (R_FAILED(rc)) {
            native_error(native, "retirar archivo o carpeta anterior", before->path, rc);
            native_unshorten(native);
            return false;
        }
        if (!native_unshorten(native) || !confirm_stage(native, before->path))
            return false;
    }
    return true;
}

bool archive_restore_switch(FILE *file, SaveIdentity id, FILE *previous, SaveIdentity previous_id,
                            const char *device, bool staged, ArchiveRestoreError *error)
{
    *error = (ArchiveRestoreError){0};
    NativeRestore native = {.fs = fsdevGetDeviceFileSystem(device), .staged = staged, .error = error};
    if (!native.fs)
        return native_error(&native, "abrir destino", device, 0);
    SaveManifest incoming = {0}, old = {0};
    native.manifest = &incoming;
    bool ok = read_archive(file, id, NULL, NULL, false, false, &native) && sort_manifest(&incoming);
    if (ok) {
        native.manifest = &old;
        ok = read_archive(previous, previous_id, NULL, NULL, false, false, &native) &&
             sort_manifest(&old) && remove_obsolete(&native, &old, &incoming);
    }
    free_manifest(&old);
    free_manifest(&incoming);
    native.manifest = NULL;
    return ok && read_archive(file, id, NULL, NULL, false, false, &native);
}

#endif

bool archive_verify_restored(FILE *file, SaveIdentity id, const char *root)
{
    return read_archive(file, id, root, NULL, false, true, NULL);
}

bool archive_copy_for_profile(FILE *file, SaveIdentity source, SaveIdentity target,
                              const char *destination)
{
    backup_log_event("PREPARAR NSV2 PARA OTRO PERFIL destino=%s", destination);
    output_error = (ArchiveOutputError){0};
    bool modified;
    if (source.title != target.title || !(target.user_low || target.user_high) ||
        !archive_matches(file, source, &modified))
        return fail_output("verificar la identidad del respaldo", destination, 0);
    char partial[PathCapacity];
    Archive *copy = open_output(destination, partial, target, modified);
    if (!copy)
        return false;
    bool ok = read_archive(file, source, NULL, copy, modified, false, NULL);
    return finish_output(copy, partial, destination, target, ok);
}

static bool clear_level(const char *root, const char *relative, char *disk, char *child,
                        char *target);

static bool clear_tree(const char *root, const char *relative)
{
    char *buffers = malloc(3 * PathCapacity);
    if (!buffers)
        return false;
    bool ok = clear_level(root, relative, buffers, buffers + PathCapacity,
                          buffers + 2 * PathCapacity);
    free(buffers);
    return ok;
}

static bool clear_level(const char *root, const char *relative, char *disk, char *child,
                        char *target)
{
    if (!make_path(disk, root, relative))
        return false;
    for (;;) {
        DIR *dir = opendir(disk);
        if (!dir)
            return false;
        struct dirent *entry;
        errno = 0;
        do {
            entry = readdir(dir);
        } while (entry && (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")));
        bool end = entry == NULL;
        bool ok = !end || directory_end();
        int n = end ? 0 : snprintf(child, PathCapacity, "%s%s%s", relative,
                                  relative[0] ? "/" : "", entry->d_name);
        if (closedir(dir) != 0 || !ok)
            return false;
        if (end)
            return true;

        struct stat info;
        if (n <= 0 || n >= PathCapacity || !safe_path(child, (size_t)n) ||
            !make_path(target, root, child) || lstat(target, &info) != 0)
            return false;
        if (S_ISDIR(info.st_mode)) {
            if (!clear_tree(root, child) || rmdir(target) != 0)
                return false;
        } else if (!S_ISREG(info.st_mode) || unlink(target) != 0) {
            return false;
        }
    }
}

bool archive_clear(const char *root)
{
    return clear_tree(root, "");
}
