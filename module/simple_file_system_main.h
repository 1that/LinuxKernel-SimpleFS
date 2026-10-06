#ifndef SIMPLEFS_HEADER
#define SIMPLEFS_HEADER

#include <linux/types.h>
#include <linux/fs.h>

#define SIMPLEFS_MAGIC 0xADADADA1 

#define SIMPLEFS_SECTOR_SIZE 512

#define SIMPLEFS_DEFAULT_SB0_SECTOR 0
#define SIMPLEFS_DEFAULT_SB1_SECTOR 0
#define SIMPLEFS_MAX_FILE_SECTORS 8
#define SIMPLEFS_DEFAULT_FILE_SECTORS 1

#define SIMPLEFS_ROOT_INO 1
#define SIMPLEFS_FIRST_FILE_INO 2

#define SIMPLEFS_NAME_PREFIX "file"
#define SIMPLEFS_NAME_PREFIX_LEN (sizeof(SIMPLEFS_NAME_PREFIX) - 1)
#define SIMPLEFS_NAME_MAX_DIGITS 10
#define SIMPLEFS_NAME_MAX_LEN (SIMPLEFS_NAME_PREFIX_LEN + SIMPLEFS_NAME_MAX_DIGITS)

struct simplefs_super {
    __le32 magic;
    __le32 sector_size;
    __le32 total_sectors;
    __le32 sb0_sector;
    __le32 sb1_sector;
    __le32 file_sectors;
    __le32 file_count;
    __le32 checksum;
};

struct simplefs_info {
    u32 total_sectors;
    u32 sb0_sector;
    u32 sb1_sector;
    u32 file_sectors;
    u32 file_count;
    bool erased;
};

long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
u32 simplefs_file_sector(const struct simplefs_info *fsi, u32 file, u32 block);

#endif
