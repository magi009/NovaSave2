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

/* PathCapacity holds a mount prefix plus the longest relative path. The
 * console's own limit is FS_MAX_PATH (0x301): "/" + relative + NUL. A path
 * longer than that exists only if the game renamed a folder into a deep spot,
 * and the file service cannot open it by name. */
enum { PathCapacity = 1024, MaxDepth = 384, BufferSize = 65536 };
#ifdef __SWITCH2__
enum { RelativeLimit = FS_MAX_PATH - 2 };
#else
enum { RelativeLimit = 0x301 - 2 }; /* same limit, so a PC copy restores on the console */
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

static bool too_long(const char *path, size_t length)
{
    backup_log_event("RUTA DEMASIADO LARGA caracteres=%zu limite=%d ruta=%s",
                     length + 1, RelativeLimit + 1, path);
    return fail_output("leer ruta mas larga que el limite de la consola (768)", path, ENAMETOOLONG);
}

static bool write_tree(Archive *archive, const char *root, const char *relative)
{
    char *disk = malloc(2 * PathCapacity);
    if (!disk)
        return fail_output("reservar memoria", relative, ENOMEM);
    char *child = disk + PathCapacity;
    bool ok = true;
    if (!make_path(disk, root, relative)) {
        free(disk);
        return fail_output("validar ruta de carpeta de origen", relative, ENAMETOOLONG);
    }
    DIR *dir = opendir(disk);
    if (dir == NULL) {
        ok = fail_output("abrir carpeta de origen", disk, errno);
        free(disk);
        return ok;
    }
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            ok = directory_end() || fail_output("listar archivos de origen", disk, errno);
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        int length = snprintf(child, PathCapacity, "%s%s%s", relative,
                              relative[0] ? "/" : "", entry->d_name);
        struct stat info;
        if (length <= 0 || length >= PathCapacity) {
            ok = too_long(relative, strlen(relative) + 1 + strlen(entry->d_name));
            break;
        }
        if (length > RelativeLimit) {
            ok = too_long(child, (size_t)length);
            break;
        }
        if (!safe_path(child, (size_t)length)) {
            ok = fail_output("validar nombre de entrada de origen", child, EINVAL);
            break;
        }
        if (!make_path(disk, root, child)) {
            ok = too_long(child, (size_t)length);
            break;
        }
        if (lstat(disk, &info) != 0) {
            ok = fail_output("examinar entrada de origen", child, errno);
            break;
        }
        if (info.st_size < 0 || (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))) {
            ok = fail_output("examinar tipo de entrada de origen", child, EINVAL);
            break;
        }
        bool folder = S_ISDIR(info.st_mode);
        uint64_t remaining = folder ? 0 : (uint64_t)info.st_size;
        backup_log_entry(folder ? "CARPETA" : "COPIAR ARCHIVO", disk, remaining);
        unsigned char record[11] = {folder ? Directory : RegularFile};
        encode(record + 1, (unsigned)length, 2);
        encode(record + 3, remaining, 8);
        ok = write_data(archive, record, sizeof record) &&
             write_data(archive, child, (size_t)length);
        if (!ok) {
            fail_output("escribir indice en la microSD", child, errno);
            break;
        }
        if (folder) {
            ok = write_tree(archive, root, child);
            continue;
        }
        archive->has_file = true;
        FILE *input = fopen(disk, "rb");
        if (input == NULL) {
            ok = fail_output("abrir archivo de origen", disk, errno);
            break;
        }
        uint64_t offset = 0;
        while (ok && remaining > 0) {
            size_t chunk = remaining < BufferSize ? (size_t)remaining : BufferSize;
            ok = (fread(archive->buffer, 1, chunk, input) == chunk ||
                  fail_output("leer archivo de origen", disk, ferror(input) ? errno : 0)) &&
                 (write_data(archive, archive->buffer, chunk) ||
                  fail_output("escribir respaldo en la microSD", child, errno));
            if (ok) {
                remaining -= chunk;
                offset += chunk;
            }
        }
        if (ok && (fgetc(input) != EOF || ferror(input)))
            ok = fail_output("confirmar fin del archivo de origen", disk, ferror(input) ? errno : 0);
        if (fclose(input) != 0)
            ok = fail_output("cerrar archivo de origen", disk, errno);
        if (!ok)
            backup_log_event("AVANCE ARCHIVO bytes_completados=%" PRIu64 " ruta=%s", offset, disk);
    }
    if (closedir(dir) != 0)
        ok = fail_output("cerrar carpeta de origen", relative, errno);
    free(disk);
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
    ArchiveRestoreError *error;
#else
    int unused;
#endif
} NativeRestore;

#ifdef __SWITCH2__
static bool native_error(NativeRestore *restore, const char *operation, const char *path, Result result)
{
    restore->error->operation = operation;
    restore->error->result = result;
    snprintf(restore->error->path, sizeof restore->error->path, "%s", path);
    return false;
}

static bool confirm_stage(NativeRestore *restore, const char *path)
{
    if (!restore->staged)
        return true;
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

static bool restore_native_record(Archive *archive, NativeRestore *restore, unsigned char type,
                                  const char *relative, uint64_t size)
{
    char path[FS_MAX_PATH] = {0};
    int length = snprintf(path, sizeof path, "/%s", relative);
    if (length < 0 || (size_t)length >= sizeof path || size > INT64_MAX)
        return native_error(restore, "ruta o tamano invalido", relative, 0);

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
        char path[PathCapacity], disk[PathCapacity];
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
                if (length > RelativeLimit)
                    return native_error(native, "ruta mas larga que el limite de la consola (768)", path, 0);
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
        if (root) {
            if (!make_path(disk, root, path))
                return false;
            if (compare) {
                struct stat info;
                if (lstat(disk, &info) != 0)
                    return false;
                if (type == Directory) {
                    if (!S_ISDIR(info.st_mode))
                        return false;
                    continue;
                }
                if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size != remaining)
                    return false;
                output = fopen(disk, "rb");
                if (!output)
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
        char path[FS_MAX_PATH] = {0};
        int length = snprintf(path, sizeof path, "/%s", before->path);
        if (length < 0 || (size_t)length >= sizeof path)
            return native_error(native, "validar ruta anterior", before->path, 0);
        Result rc = before->type == Directory ? fsFsDeleteDirectory(native->fs, path)
                                              : fsFsDeleteFile(native->fs, path);
        if (R_FAILED(rc))
            return native_error(native, "retirar archivo o carpeta anterior", path, rc);
        if (!confirm_stage(native, path))
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
