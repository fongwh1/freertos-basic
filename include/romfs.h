#ifndef __ROMFS_H__
#define __ROMFS_H__

#include <stdint.h>

#define DIR_LEN 128
#define FILE_NAME_LEN 128

void register_romfs(const char * mountpoint, const uint8_t * romfs);
const uint8_t * romfs_get_file_by_hash(const uint8_t * romfs, uint32_t h, uint32_t * len);

#endif

