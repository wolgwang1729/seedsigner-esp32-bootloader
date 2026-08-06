#ifndef BL_SYSCALLS_FS_H
#define BL_SYSCALLS_FS_H

#include <stdint.h>

typedef uint32_t bl_fsize_t;
typedef void* bl_file_obj_t;
typedef void* bl_file_t;
typedef int32_t bl_foffset_t;

typedef struct bl_ffind_ctx_struct {
  void* dj;
  void* fno;
} bl_ffind_ctx_t;

#endif // BL_SYSCALLS_FS_H
