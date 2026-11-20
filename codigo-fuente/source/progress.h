#ifndef NOVASAVE_PROGRESS_H
#define NOVASAVE_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>

/* Barra de progreso en pantalla: porcentaje, tiempo, archivo actual y tamanos.
 * main.c pone el titulo de la fase; archive.c informa los bytes. */
void progress_title(const char *title);
void progress_start(uint64_t total_bytes, unsigned total_files);
void progress_file(const char *name, uint64_t size);
void progress_add(uint64_t bytes);
void progress_finish(void);
void progress_enable(bool enabled);

#endif
