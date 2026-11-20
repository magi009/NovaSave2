#define _POSIX_C_SOURCE 200809L
#include "zipio.h"

#include "backup_log.h"
#include "progress.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

enum {
    PathSize = 1024,
    ChunkSize = 65536,
    MaxEntries = 65535,
    MaxDepth = 64,
    LocalSig = 0x04034b50,
    CentralSig = 0x02014b50,
    EndSig = 0x06054b50,
    FlagEncrypted = 1,
    FlagUtf8 = 1 << 11,
};

static char error_text[160];

const char *zip_error(void)
{
    return error_text;
}

static bool fail(const char *what, const char *path)
{
    if (!error_text[0])
        snprintf(error_text, sizeof error_text, "%s", what);
    backup_log_event("ERROR ZIP paso=%s errno=%d ruta=%s", what, errno, path ? path : "-");
    return false;
}

static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(unsigned char *p, unsigned v) { p[0] = v & 0xFF; p[1] = v >> 8 & 0xFF; }
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = v >> 8 & 0xFF; p[2] = v >> 16 & 0xFF; p[3] = v >> 24 & 0xFF;
}

/* Rutas dentro del ZIP: relativas, sin "..", sin ':' ni '\\', componentes < 256. */
static bool clean_name(char *name)
{
    while (name[0] == '.' && name[1] == '/')
        memmove(name, name + 2, strlen(name + 2) + 1);
    size_t length = strlen(name);
    if (!length || name[0] == '/' || strpbrk(name, ":\\"))
        return false;
    const char *part = name;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t span = end ? (size_t)(end - part) : strlen(part);
        if (span == 0 && end && end[1] == '\0')
            break;
        if (span == 0 || span > 255 || (span == 1 && part[0] == '.') ||
            (span == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        if (!end)
            break;
        part = end + 1;
    }
    return true;
}

static bool make_parents(const char *root, const char *name)
{
    char path[PathSize];
    size_t base = strlen(root);
    if (snprintf(path, sizeof path, "%s%s", root, name) >= (int)sizeof path)
        return false;
    for (char *p = path + base; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return true;
}

typedef struct {
    char name[PathSize];
    uint16_t flags, method;
    uint32_t crc, csize, usize, offset;
    bool folder;
} Entry;

static bool read_entries(FILE *file, Entry **out, size_t *count, uint64_t *total)
{
    if (fseek(file, 0, SEEK_END) != 0)
        return fail("leer el ZIP", NULL);
    long size = ftell(file);
    if (size < 22)
        return fail("el archivo no es un ZIP", NULL);
    long tail = size < 65557 ? size : 65557;
    unsigned char *buffer = malloc((size_t)tail);
    if (!buffer)
        return fail("reservar memoria", NULL);
    bool ok = fseek(file, size - tail, SEEK_SET) == 0 && fread(buffer, 1, (size_t)tail, file) == (size_t)tail;
    long end = -1;
    for (long i = tail - 22; ok && i >= 0; i--)
        if (get32(buffer + i) == EndSig) {
            end = i;
            break;
        }
    if (!ok || end < 0) {
        free(buffer);
        return fail("el archivo no es un ZIP", NULL);
    }
    unsigned entries = get16(buffer + end + 10);
    uint32_t cd_size = get32(buffer + end + 12), cd_offset = get32(buffer + end + 16);
    free(buffer);
    if (entries == 0xFFFF || cd_offset == 0xFFFFFFFF || (uint64_t)cd_offset + cd_size > (uint64_t)size)
        return fail("ZIP64 o ZIP danado", NULL);

    Entry *list = calloc(entries ? entries : 1, sizeof *list);
    unsigned char *cd = malloc(cd_size ? cd_size : 1);
    if (!list || !cd || fseek(file, cd_offset, SEEK_SET) != 0 || fread(cd, 1, cd_size, file) != cd_size) {
        free(list);
        free(cd);
        return fail("leer el indice del ZIP", NULL);
    }
    size_t at = 0;
    *total = 0;
    for (unsigned i = 0; i < entries; i++) {
        if (at + 46 > cd_size || get32(cd + at) != CentralSig) {
            free(list);
            free(cd);
            return fail("indice del ZIP danado", NULL);
        }
        Entry *e = &list[i];
        e->flags = get16(cd + at + 8);
        e->method = get16(cd + at + 10);
        e->crc = get32(cd + at + 16);
        e->csize = get32(cd + at + 20);
        e->usize = get32(cd + at + 24);
        unsigned nlen = get16(cd + at + 28), xlen = get16(cd + at + 30), clen = get16(cd + at + 32);
        e->offset = get32(cd + at + 42);
        if (at + 46 + nlen > cd_size || nlen >= sizeof e->name) {
            free(list);
            free(cd);
            return fail("nombre en el ZIP demasiado largo", NULL);
        }
        memcpy(e->name, cd + at + 46, nlen);
        e->name[nlen] = '\0';
        for (char *p = e->name; *p; p++)
            if (*p == '\\')
                *p = '/';
        e->folder = nlen && e->name[nlen - 1] == '/';
        if (e->flags & FlagEncrypted) {
            fail("el ZIP tiene contrasena", e->name);
            free(list);
            free(cd);
            return false;
        }
        if (!clean_name(e->name)) {
            fail("ruta no valida dentro del ZIP", e->name);
            free(list);
            free(cd);
            return false;
        }
        if (!e->folder && e->method != 0 && e->method != 8) {
            fail("metodo de compresion no compatible", e->name);
            free(list);
            free(cd);
            return false;
        }
        if (!e->folder)
            *total += e->usize;
        at += 46 + nlen + xlen + clen;
    }
    free(cd);
    *out = list;
    *count = entries;
    return true;
}

/* Extrae o, si out es NULL, solo comprueba el CRC. */
static bool inflate_entry(FILE *file, const Entry *e, FILE *out, unsigned char *in_buf, unsigned char *out_buf)
{
    unsigned char local[30];
    if (fseek(file, e->offset, SEEK_SET) != 0 || fread(local, 1, 30, file) != 30 || get32(local) != LocalSig)
        return fail("cabecera de archivo del ZIP danada", e->name);
    if (fseek(file, (long)e->offset + 30 + get16(local + 26) + get16(local + 28), SEEK_SET) != 0)
        return fail("leer archivo del ZIP", e->name);

    progress_file(e->name, e->usize);
    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint64_t written = 0, remaining = e->csize;
    z_stream z = {0};
    if (e->method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return fail("iniciar descompresion", e->name);
    bool ok = true, finished = e->method == 0 && remaining == 0;
    while (ok && remaining > 0) {
        size_t chunk = remaining < ChunkSize ? (size_t)remaining : ChunkSize;
        if (fread(in_buf, 1, chunk, file) != chunk) {
            ok = fail("leer archivo del ZIP", e->name);
            break;
        }
        remaining -= chunk;
        if (e->method == 0) {
            crc = crc32(crc, in_buf, (uInt)chunk);
            written += chunk;
            progress_add(chunk);
            if (out && fwrite(in_buf, 1, chunk, out) != chunk)
                ok = fail("escribir archivo extraido", e->name);
            finished = remaining == 0;
            continue;
        }
        z.next_in = in_buf;
        z.avail_in = (uInt)chunk;
        do {
            z.next_out = out_buf;
            z.avail_out = ChunkSize;
            int rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                ok = fail("datos comprimidos danados", e->name);
                break;
            }
            size_t produced = ChunkSize - z.avail_out;
            crc = crc32(crc, out_buf, (uInt)produced);
            written += produced;
            progress_add(produced);
            if (out && produced && fwrite(out_buf, 1, produced, out) != produced)
                ok = fail("escribir archivo extraido", e->name);
            finished = rc == Z_STREAM_END;
        } while (ok && !finished && (z.avail_in > 0 || z.avail_out == 0));
    }
    if (e->method == 8)
        inflateEnd(&z);
    if (ok && (!finished || written != e->usize || crc != e->crc))
        ok = fail("el contenido del ZIP no coincide con su CRC", e->name);
    return ok;
}

static bool unzip(const char *zip_path, const char *dest_root)
{
    error_text[0] = '\0';
    FILE *file = fopen(zip_path, "rb");
    if (!file)
        return fail("abrir el ZIP", zip_path);
    Entry *entries = NULL;
    size_t count = 0;
    uint64_t total = 0;
    bool ok = read_entries(file, &entries, &count, &total);
    unsigned char *in_buf = malloc(ChunkSize), *out_buf = malloc(ChunkSize);
    if (ok && (!in_buf || !out_buf))
        ok = fail("reservar memoria", NULL);
    unsigned files = 0;
    for (size_t i = 0; ok && i < count; i++)
        files += !entries[i].folder;
    if (ok)
        progress_start(total, files);
    char path[PathSize];
    for (size_t i = 0; ok && i < count; i++) {
        const Entry *e = &entries[i];
        if (!dest_root) {
            if (!e->folder)
                ok = inflate_entry(file, e, NULL, in_buf, out_buf);
            continue;
        }
        if (snprintf(path, sizeof path, "%s%s", dest_root, e->name) >= (int)sizeof path) {
            ok = fail("ruta extraida demasiado larga", e->name);
            break;
        }
        if (!make_parents(dest_root, e->name)) {
            ok = fail("crear carpeta extraida", e->name);
            break;
        }
        if (e->folder) {
            path[strlen(path) - 1] = '\0';
            if (mkdir(path, 0700) != 0 && errno != EEXIST)
                ok = fail("crear carpeta extraida", e->name);
            continue;
        }
        FILE *out = fopen(path, "wb");
        if (!out) {
            ok = fail("crear archivo extraido", e->name);
            break;
        }
        ok = inflate_entry(file, e, out, in_buf, out_buf);
        if (fclose(out) != 0 && ok)
            ok = fail("cerrar archivo extraido", e->name);
        if (ok)
            backup_log_entry("EXTRAER", e->name, e->usize);
    }
    if (ok)
        progress_finish();
    free(in_buf);
    free(out_buf);
    free(entries);
    fclose(file);
    return ok;
}

bool zip_extract(const char *zip_path, const char *dest_root)
{
    backup_log_event("EXTRAER ZIP origen=%s destino=%s", zip_path, dest_root);
    return unzip(zip_path, dest_root);
}

/* ---- Crear ---------------------------------------------------------------- */

typedef struct {
    FILE *file;
    unsigned char *central;
    size_t central_size, central_capacity;
    unsigned entries;
    unsigned char *in_buf, *out_buf;
} Writer;

static void dos_time(time_t when, uint16_t *time_out, uint16_t *date_out)
{
    struct tm parts;
    if (!localtime_r(&when, &parts) || parts.tm_year < 80) {
        *time_out = 0;
        *date_out = (1 << 5) | 1;
        return;
    }
    *time_out = (uint16_t)(parts.tm_hour << 11 | parts.tm_min << 5 | parts.tm_sec / 2);
    *date_out = (uint16_t)((parts.tm_year - 80) << 9 | (parts.tm_mon + 1) << 5 | parts.tm_mday);
}

static bool add_central(Writer *w, const char *name, uint16_t method, uint16_t mtime, uint16_t mdate,
                        uint32_t crc, uint32_t csize, uint32_t usize, uint32_t offset, bool folder)
{
    size_t nlen = strlen(name);
    if (w->entries == MaxEntries)
        return fail("demasiados archivos para un ZIP", name);
    if (w->central_size + 46 + nlen > w->central_capacity) {
        size_t capacity = (w->central_capacity + 46 + nlen) * 2;
        unsigned char *grown = realloc(w->central, capacity);
        if (!grown)
            return fail("reservar memoria", name);
        w->central = grown;
        w->central_capacity = capacity;
    }
    unsigned char *c = w->central + w->central_size;
    memset(c, 0, 46);
    put32(c, CentralSig);
    put16(c + 4, 0x0314);        /* creado en Unix, version 2.0 */
    put16(c + 6, 20);
    put16(c + 8, FlagUtf8);
    put16(c + 10, method);
    put16(c + 12, mtime);
    put16(c + 14, mdate);
    put32(c + 16, crc);
    put32(c + 20, csize);
    put32(c + 24, usize);
    put16(c + 28, (unsigned)nlen);
    put32(c + 38, folder ? (040755u << 16) | 0x10 : 0100644u << 16);
    put32(c + 42, offset);
    memcpy(c + 46, name, nlen);
    w->central_size += 46 + nlen;
    w->entries++;
    return true;
}

static bool write_local(Writer *w, const char *name, uint16_t method, uint16_t mtime, uint16_t mdate,
                        long *offset)
{
    unsigned char local[30] = {0};
    size_t nlen = strlen(name);
    put32(local, LocalSig);
    put16(local + 4, 20);
    put16(local + 6, FlagUtf8);
    put16(local + 8, method);
    put16(local + 10, mtime);
    put16(local + 12, mdate);
    put16(local + 26, (unsigned)nlen);
    *offset = ftell(w->file);
    if (*offset < 0 || *offset > 0x7FFFFFFF)
        return fail("el ZIP pasa de 2 GB", name);
    return fwrite(local, 1, 30, w->file) == 30 && fwrite(name, 1, nlen, w->file) == nlen;
}

static bool add_file(Writer *w, const char *disk, const char *name, const struct stat *info)
{
    if (info->st_size > 0x7FFFFFFF)
        return fail("archivo demasiado grande para ZIP", name);
    uint16_t mtime, mdate;
    dos_time(info->st_mtime, &mtime, &mdate);
    long offset;
    if (!write_local(w, name, 8, mtime, mdate, &offset))
        return fail("escribir el ZIP", name);
    FILE *in = fopen(disk, "rb");
    if (!in)
        return fail("abrir archivo para el ZIP", name);
    progress_file(name, (uint64_t)info->st_size);
    z_stream z = {0};
    if (deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        fclose(in);
        return fail("iniciar compresion", name);
    }
    uint32_t crc = crc32(0L, Z_NULL, 0), csize = 0, usize = 0;
    bool ok = true;
    int flush = Z_NO_FLUSH;
    do {
        size_t got = fread(w->in_buf, 1, ChunkSize, in);
        if (ferror(in)) {
            ok = fail("leer archivo para el ZIP", name);
            break;
        }
        flush = feof(in) ? Z_FINISH : Z_NO_FLUSH;
        crc = crc32(crc, w->in_buf, (uInt)got);
        usize += (uint32_t)got;
        progress_add(got);
        z.next_in = w->in_buf;
        z.avail_in = (uInt)got;
        do {
            z.next_out = w->out_buf;
            z.avail_out = ChunkSize;
            deflate(&z, flush);
            size_t produced = ChunkSize - z.avail_out;
            if (produced && fwrite(w->out_buf, 1, produced, w->file) != produced) {
                ok = fail("escribir el ZIP", name);
                break;
            }
            csize += (uint32_t)produced;
        } while (z.avail_out == 0);
    } while (ok && flush != Z_FINISH);
    deflateEnd(&z);
    fclose(in);
    if (!ok)
        return false;
    long end = ftell(w->file);
    unsigned char sizes[12];
    put32(sizes, crc);
    put32(sizes + 4, csize);
    put32(sizes + 8, usize);
    if (end < 0 || fseek(w->file, offset + 14, SEEK_SET) != 0 || fwrite(sizes, 1, 12, w->file) != 12 ||
        fseek(w->file, end, SEEK_SET) != 0)
        return fail("escribir el ZIP", name);
    backup_log_entry("COMPRIMIR", name, usize);
    return add_central(w, name, 8, mtime, mdate, crc, csize, usize, (uint32_t)offset, false);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool add_tree(Writer *w, const char *root, const char *relative, unsigned depth)
{
    if (depth > MaxDepth)
        return fail("demasiadas carpetas anidadas", relative);
    char disk[PathSize], child[PathSize];
    snprintf(disk, sizeof disk, "%s%s", root, relative);
    DIR *dir = opendir(disk);
    if (!dir)
        return fail("abrir carpeta para el ZIP", relative[0] ? relative : root);
    char **names = NULL;
    size_t count = 0, capacity = 0;
    bool ok = true;
    struct dirent *entry;
    while (ok && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            char **grown = realloc(names, capacity * sizeof *names);
            if (!grown) {
                ok = fail("reservar memoria", relative);
                break;
            }
            names = grown;
        }
        if (!(names[count] = strdup(entry->d_name)))
            ok = fail("reservar memoria", relative);
        else
            count++;
    }
    closedir(dir);
    if (ok && count > 1)
        qsort(names, count, sizeof *names, compare_names);
    for (size_t i = 0; ok && i < count; i++) {
        int n = snprintf(child, sizeof child, "%s%s%s", relative, relative[0] ? "/" : "", names[i]);
        if (n < 0 || n >= (int)sizeof child - 1) {
            ok = fail("ruta demasiado larga para el ZIP", names[i]);
            break;
        }
        snprintf(disk, sizeof disk, "%s%s", root, child);
        struct stat info;
        if (lstat(disk, &info) != 0) {
            ok = fail("examinar archivo para el ZIP", child);
        } else if (S_ISDIR(info.st_mode)) {
            char folder[PathSize];
            snprintf(folder, sizeof folder, "%s/", child);
            uint16_t mtime, mdate;
            long offset;
            dos_time(info.st_mtime, &mtime, &mdate);
            ok = (write_local(w, folder, 0, mtime, mdate, &offset) ||
                  fail("escribir el ZIP", folder)) &&
                 add_central(w, folder, 0, mtime, mdate, 0, 0, 0, (uint32_t)offset, true) &&
                 add_tree(w, root, child, depth + 1);
        } else if (S_ISREG(info.st_mode)) {
            ok = add_file(w, disk, child, &info);
        }
    }
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return ok;
}

static bool sum_tree(const char *root, const char *relative, uint64_t *bytes, unsigned *files, unsigned depth)
{
    char disk[PathSize], child[PathSize];
    snprintf(disk, sizeof disk, "%s%s", root, relative);
    DIR *dir = opendir(disk);
    if (!dir || depth > MaxDepth)
        return dir ? (closedir(dir), false) : false;
    struct dirent *entry;
    bool ok = true;
    while (ok && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        snprintf(child, sizeof child, "%s%s%s", relative, relative[0] ? "/" : "", entry->d_name);
        snprintf(disk, sizeof disk, "%s%s", root, child);
        struct stat info;
        if (lstat(disk, &info) != 0)
            ok = false;
        else if (S_ISDIR(info.st_mode))
            ok = sum_tree(root, child, bytes, files, depth + 1);
        else if (S_ISREG(info.st_mode)) {
            *bytes += (uint64_t)info.st_size;
            (*files)++;
        }
    }
    closedir(dir);
    return ok;
}

bool zip_create(const char *src_root, const char *zip_path)
{
    error_text[0] = '\0';
    backup_log_event("CREAR ZIP origen=%s destino=%s", src_root, zip_path);
    uint64_t bytes = 0;
    unsigned files = 0;
    if (!sum_tree(src_root, "", &bytes, &files, 0))
        return fail("leer la carpeta de origen", src_root);
    if (!files)
        return fail("la carpeta esta vacia", src_root);
    struct stat existing;
    if (stat(zip_path, &existing) == 0)
        return fail("ya existe un ZIP con ese nombre", zip_path);
    char partial[PathSize];
    if (snprintf(partial, sizeof partial, "%s.partial", zip_path) >= (int)sizeof partial)
        return fail("ruta de salida demasiado larga", zip_path);
    Writer w = {0};
    w.file = fopen(partial, "wbx");
    if (!w.file)
        return fail("crear el ZIP en la microSD", partial);
    w.in_buf = malloc(ChunkSize);
    w.out_buf = malloc(ChunkSize);
    progress_start(bytes, files);
    bool ok = (w.in_buf && w.out_buf) || fail("reservar memoria", NULL);
    ok = ok && add_tree(&w, src_root, "", 0);
    if (ok) {
        long cd_offset = ftell(w.file);
        unsigned char end[22] = {0};
        put32(end, EndSig);
        put16(end + 8, w.entries);
        put16(end + 10, w.entries);
        put32(end + 12, (uint32_t)w.central_size);
        put32(end + 16, (uint32_t)cd_offset);
        ok = (cd_offset >= 0 && cd_offset <= 0x7FFFFFFF &&
              fwrite(w.central, 1, w.central_size, w.file) == w.central_size &&
              fwrite(end, 1, sizeof end, w.file) == sizeof end) ||
             fail("escribir el indice del ZIP", zip_path);
    }
    if (fflush(w.file) != 0 || fsync(fileno(w.file)) != 0)
        ok = ok && fail("guardar el ZIP en la microSD", zip_path);
    if (fclose(w.file) != 0)
        ok = ok && fail("cerrar el ZIP", zip_path);
    free(w.central);
    free(w.in_buf);
    free(w.out_buf);
    if (ok) {
        progress_title("Verificando el ZIP");
        ok = unzip(partial, NULL);
    }
    if (ok && rename(partial, zip_path) != 0)
        ok = fail("publicar el ZIP", zip_path);
    if (!ok)
        remove(partial);
    backup_log_event("FIN ZIP ok=%d archivos=%u bytes=%" PRIu64, ok, files, bytes);
    return ok;
}
