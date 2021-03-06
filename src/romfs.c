#include <string.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <unistd.h>
#include "fio.h"
#include "filesystem.h"
#include "romfs.h"
#include "osdebug.h"
#include "hash-djb2.h"
#include "dir.h"
#include "string.h"

#define hash_init 5381

struct romfs_fds_t {
    const uint8_t * file;
    uint32_t cursor;
    uint32_t size;
};

struct romfs_dirent_t {
    uint32_t d_ino; /* unused so far*/
    char d_name[FILE_NAME_LEN]; /* dir name, assume the name of a file
                         would be shorter than 128 charaters*/
};

struct romfs_dirs_t {
    uint32_t start;
    uint32_t cur_off;
    uint32_t end;
};

static struct romfs_fds_t romfs_fds[MAX_FDS];

static struct romfs_dirent_t romfs_dirent[DIR_LEN];
static struct romfs_dirs_t romfs_dirs[MAX_DIRS];

static uint32_t get_unaligned(const uint8_t * d) {
    return ((uint32_t) d[0]) | ((uint32_t) (d[1] << 8)) | ((uint32_t) (d[2] << 16)) | ((uint32_t) (d[3] << 24));
}

static ssize_t romfs_read(void * opaque, void * buf, size_t count) {
    struct romfs_fds_t * f = (struct romfs_fds_t *) opaque;
    uint32_t size = f -> size;
    
    if ((f->cursor + count) > size)
        count = size - f->cursor;

    memcpy(buf, f->file + f->cursor, count);
    f->cursor += count;

    return count;
}

static off_t romfs_seek(void * opaque, off_t offset, int whence) {
    struct romfs_fds_t * f = (struct romfs_fds_t *) opaque;
    uint32_t size = f->size; 
    uint32_t origin;
    
    switch (whence) {
    case SEEK_SET:
        origin = 0;
        break;
    case SEEK_CUR:
        origin = f->cursor;
        break;
    case SEEK_END:
        origin = size;
        break;
    default:
        return -1;
    }

    offset = origin + offset;

    if (offset < 0)
        return -1;
    if (offset > size)
        offset = size;

    f->cursor = offset;

    return offset;
}

int romfs_dir_next(void * opaque, void * buf, size_t bufsize) {
    struct romfs_dirs_t * dirs = opaque;
    if ((dirs->cur_off + dirs->start) > dirs->end){
        return 0;
    }
    char * pBuf = buf;

    uint32_t eff_offset = dirs->start + dirs->cur_off;
    size_t i = bufsize;
    uint32_t c = 0;
    while ((*pBuf++ = romfs_dirent[eff_offset].d_name[c++]) && i > 1) i--;
    *(pBuf) = '\0';
    dirs->cur_off ++;
    return (bufsize - i); //or return 0 would be better?
    
}

int romfs_dir_close(void * opaque) {
    uint32_t i, j;
    struct romfs_dirs_t * dirs = opaque;

    for (i = dirs->start; i <= dirs->end; i++){
        j = 0;
        romfs_dirent[i].d_ino = 0;
        while(j++<FILE_NAME_LEN){
        romfs_dirent[i].d_name[j] = '\0';
        }
    }

    return 0;
}

int romfs_get_all_filename_by_hash(const uint8_t * romfs, uint32_t h, uint32_t start) {
    const uint8_t * meta;
    uint32_t i = start;
    for (meta = romfs; get_unaligned(meta) && get_unaligned(meta + 4); meta += get_unaligned(meta + 4) + 12) {
        if (get_unaligned(meta+8) == h){
            strcpy(romfs_dirent[i].d_name, (const char *)meta+12); //may overflow the d_name
            romfs_dirent[i].d_ino = (i - start + 1);
            i++;
        }
    }
    return (i - start);
}

int romfs_opendir(void * opaque, const char * path){
    uint32_t hash = hash_djb2((const uint8_t *) path, -1);
    const uint8_t * romfs = (const uint8_t *)opaque;
    uint32_t start = 0; // assume only one dir at a time,
                        // so dirent[] always start at 0;
    uint32_t matched = romfs_get_all_filename_by_hash(romfs, hash, start);
    uint32_t end = 0, dird = 0;
    if (matched > 0){
        end = start + matched;
        dird = dir_open(romfs_dir_next, romfs_dir_close, opaque);
        romfs_dirs[dird].start = start;
        romfs_dirs[dird].cur_off = 0;
        romfs_dirs[dird].end = end;
        dir_set_opaque(dird, romfs_dirs + dird);
    }

    return dird;
}

const uint8_t * romfs_get_file_by_hash(const uint8_t * romfs, uint32_t h, uint32_t * len) {
    const uint8_t * meta;

    for (meta = romfs; get_unaligned(meta) && get_unaligned(meta + 4); meta += get_unaligned(meta + 4) + 12) {
        if (get_unaligned(meta) == h) {
            if (len) {
                *len = get_unaligned(meta + 4);
            }
            return meta + 12;
        }
    }

    return NULL;
}

static int romfs_open(void * opaque, const char * path, int flags, int mode) {
    uint32_t h = hash_djb2((const uint8_t *) path, -1);
    const uint8_t * romfs = (const uint8_t *) opaque;
    const uint8_t * file;
    int r = -1;

    file = romfs_get_file_by_hash(romfs, h, NULL);

    if (file) {
        r = fio_open(romfs_read, NULL, romfs_seek, NULL, NULL);
        if (r > 0) {
            uint32_t size = get_unaligned(file - 8);
            const uint8_t *filestart = file;
            while(*filestart) ++filestart;
            ++filestart;
            size -= filestart - file;
            romfs_fds[r].file = filestart;
            romfs_fds[r].cursor = 0;
            romfs_fds[r].size = size;
            fio_set_opaque(r, romfs_fds + r);
        }
    }
    return r;
}

void register_romfs(const char * mountpoint, const uint8_t * romfs) {
//    DBGOUT("Registering romfs `%s' @ %p\r\n", mountpoint, romfs);
    register_fs(mountpoint, romfs_open, romfs_opendir, (void *) romfs);
}
