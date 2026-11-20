#ifndef NOVASAVE_STORAGE_ERROR_H
#define NOVASAVE_STORAGE_ERROR_H

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#ifdef __SWITCH2__
#include <switch/runtime/devices/fs_dev.h>
#include <switch/result.h>
#endif

static inline uint32_t storage_native_error(int system_error)
{
#ifdef __SWITCH2__
    if (system_error == EIO)
        return fsdevGetLastResult();
#else
    (void)system_error;
#endif
    return 0;
}

static inline bool storage_path_missing(int system_error)
{
    if (system_error == ENOENT)
        return true;
#ifdef __SWITCH2__
    return R_VALUE(storage_native_error(system_error)) == MAKERESULT(2, 1);
#else
    return false;
#endif
}

#endif
