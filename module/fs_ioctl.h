#ifndef SIMPLEFS_IOCTL_H
#define SIMPLEFS_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SIMPLEFS_IOC_MAGIC 'A'

struct simplefs_file_meta {
    char name[16];
    __u32 first_sector;
    __u32 last_sector;
    __u32 size;
    __u32 crc32;
};

struct simplefs_ioc_meta {
    __u32 capacity;
    __u32 count;
    struct simplefs_file_meta files[];
};

struct simplefs_ioc_map {
    __u32 file;
    __u32 capacity;
    __u32 count;
    __u32 sectors[];
};


#define SIMPLEFS_IOC_ZERO _IO(SIMPLEFS_IOC_MAGIC, 1)
#define SIMPLEFS_IOC_ERASE _IO(SIMPLEFS_IOC_MAGIC, 2)
#define SIMPLEFS_IOC_META _IOWR(SIMPLEFS_IOC_MAGIC, 3, struct simplefs_ioc_meta)
#define SIMPLEFS_IOC_MAP _IOWR(SIMPLEFS_IOC_MAGIC, 4, struct simplefs_ioc_map)

#endif
