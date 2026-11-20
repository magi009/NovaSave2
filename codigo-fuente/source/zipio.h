#ifndef NOVASAVE_ZIPIO_H
#define NOVASAVE_ZIPIO_H

#include <stdbool.h>

/* ZIP como los de JKSV: los archivos de la partida en la raiz del ZIP.
 * Admite entradas guardadas (0) y comprimidas con deflate (8). */
bool zip_extract(const char *zip_path, const char *dest_root);   /* dest_root termina en '/' */
bool zip_create(const char *src_root, const char *zip_path);     /* src_root termina en '/' */
const char *zip_error(void);

#endif
