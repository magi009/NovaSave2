#define _POSIX_C_SOURCE 200809L
#include "progress.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH2__
#include <switch.h>
#endif

enum { BarWidth = 56, Columns = 78, RedrawMs = 120 };

static struct {
    bool enabled;
    char title[64];
    char name[512];
    uint64_t total, done, file_size, file_done;
    unsigned files, file_index;
    uint64_t started_ms, drawn_ms;
} state;

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

void progress_enable(bool enabled)
{
    state.enabled = enabled;
}

void progress_title(const char *title)
{
    snprintf(state.title, sizeof state.title, "%s", title);
}

static void format_size(char *out, size_t capacity, uint64_t bytes)
{
    if (bytes < 1024)
        snprintf(out, capacity, "%" PRIu64 " B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, capacity, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        snprintf(out, capacity, "%.2f MB", bytes / (1024.0 * 1024));
    else
        snprintf(out, capacity, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
}

static void format_time(char *out, size_t capacity, uint64_t ms)
{
    uint64_t seconds = ms / 1000;
    if (seconds >= 3600)
        snprintf(out, capacity, "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                 seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        snprintf(out, capacity, "%02" PRIu64 ":%02" PRIu64, seconds / 60, seconds % 60);
}

#ifdef __SWITCH2__
static void draw(void)
{
    uint64_t elapsed = now_ms() - state.started_ms;
    unsigned percent = state.total ? (unsigned)(state.done * 100 / state.total) : 0;
    if (percent > 100)
        percent = 100;
    unsigned filled = percent * BarWidth / 100;

    char done[24], total[24], file_done[24], file_size[24], spent[16], left[16], speed[24];
    format_size(done, sizeof done, state.done);
    format_size(total, sizeof total, state.total);
    format_size(file_done, sizeof file_done, state.file_done);
    format_size(file_size, sizeof file_size, state.file_size);
    format_time(spent, sizeof spent, elapsed);
    if (state.done && elapsed > 1000 && state.done < state.total)
        format_time(left, sizeof left, (state.total - state.done) * elapsed / state.done);
    else
        snprintf(left, sizeof left, "--:--");
    format_size(speed, sizeof speed, elapsed ? state.done * 1000 / elapsed : 0);

    const char *name = state.name;
    size_t length = strlen(name);
    char shown[Columns + 1];
    if (length > Columns - 12)
        snprintf(shown, sizeof shown, "...%s", name + length - (Columns - 15));
    else
        snprintf(shown, sizeof shown, "%s", name);

    consoleClear();
    printf("\x1b[46;30m  NovaSave2 %-66s\x1b[0m\n\n", "");
    printf("  \x1b[36;1m%s\x1b[0m\n\n", state.title[0] ? state.title : "Trabajando...");

    printf("  \x1b[42m%*s\x1b[0m\x1b[37m", (int)filled, "");
    for (unsigned i = filled; i < BarWidth; i++)
        putchar('-');
    printf("\x1b[0m  \x1b[33;1m%3u%%\x1b[0m\n\n", percent);

    printf("  Total       %s de %s\n", done, total);
    printf("  Tiempo      %s transcurrido   ~%s restante\n", spent, left);
    printf("  Velocidad   %s/s\n\n", speed);

    printf("  \x1b[36mArchivo actual\x1b[0m");
    if (state.files)
        printf("  (%u de %u)", state.file_index, state.files);
    printf("\n  %s\n", shown[0] ? shown : "-");
    printf("  %s de %s\n\n", file_done, file_size);
    printf("  \x1b[37mNo apagues la consola ni saques la microSD.\x1b[0m\n");
    consoleUpdate(NULL);
    state.drawn_ms = now_ms();
}
#else
static void draw(void)
{
    state.drawn_ms = now_ms();
}
#endif

void progress_start(uint64_t total_bytes, unsigned total_files)
{
    state.total = total_bytes;
    state.files = total_files;
    state.done = state.file_done = state.file_size = 0;
    state.file_index = 0;
    state.name[0] = '\0';
    state.started_ms = now_ms();
    if (state.enabled)
        draw();
}

void progress_file(const char *name, uint64_t size)
{
    snprintf(state.name, sizeof state.name, "%s", name);
    state.file_size = size;
    state.file_done = 0;
    state.file_index++;
    if (state.enabled && now_ms() - state.drawn_ms >= RedrawMs)
        draw();
}

void progress_add(uint64_t bytes)
{
    state.done += bytes;
    state.file_done += bytes;
    if (state.file_done > state.file_size)
        state.file_done = state.file_size;
    if (state.enabled && now_ms() - state.drawn_ms >= RedrawMs)
        draw();
}

void progress_finish(void)
{
    state.done = state.total;
    state.file_done = state.file_size;
    if (state.enabled)
        draw();
}
