#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/crc32.h>

#include "simple_file_system_main.h"
#include "fs_ioctl.h"

static int zero_sector(struct super_block *sb, sector_t nr)
{
    struct buffer_head *bh = sb_getblk(sb, nr);

    if (!bh)
        return -ENOMEM;
    
    lock_buffer(bh);

    memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
    set_buffer_uptodate(bh);

    unlock_buffer(bh);
    mark_buffer_dirty(bh);

    brelse(bh);
    return 0;
}

static int ioctl_zero(struct super_block *sb)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    u32 file, block;
    int err;

    for (file = 0; file < fsi->file_count; file++) {
        for (block = 0; block < fsi->file_sectors; block++) {
            err = zero_sector(sb, simplefs_file_sector(fsi, file, block));
            if (err)
                return err;
        }
    }
    
    err = sync_blockdev(sb->s_bdev);
    if (err)
        return err;

    printk(KERN_INFO "simplefs: все файлы обнулены\n");
    return 0;
}

static int ioctl_erase(struct super_block *sb)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    u32 nr;
    int err;

    for (nr = 0; nr < fsi->total_sectors; nr++) {
        err = zero_sector(sb, nr);
        if (err)
            return err;
    }
    
    err = sync_blockdev(sb->s_bdev);
    if (err)
        return err;

    fsi->erased = true;
    
    printk(KERN_INFO "simplefs: ФС стёрта\n");
    return 0;
}

static int file_crc32(struct super_block *sb, u32 file, u32 *crc)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    struct buffer_head *bh;
    u32 block;

    *crc = ~0;
    for (block = 0; block < fsi->file_sectors; block++) {
        bh = sb_bread(sb, simplefs_file_sector(fsi, file, block));
        if (!bh)
            return -EIO;
        *crc = crc32_le(*crc, bh->b_data, SIMPLEFS_SECTOR_SIZE);
        brelse(bh);
    }
    return 0;
}

static int ioctl_meta(struct super_block *sb, struct simplefs_ioc_meta __user *uarg)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    struct simplefs_ioc_meta req;
    struct simplefs_file_meta meta;
    u32 file;
    int err;

    if (copy_from_user(&req, uarg, sizeof(req)))
        return -EFAULT;

    req.count = fsi->file_count;

    if (copy_to_user(uarg, &req, sizeof(req)))
        return -EFAULT;

    if (req.capacity < req.count)
        return -ENOSPC;

    for (file = 0; file < fsi->file_count; file++) {
        memset(&meta, 0, sizeof(meta));
        snprintf(meta.name, sizeof(meta.name), SIMPLEFS_NAME_PREFIX "%u", file);
        meta.first_sector = simplefs_file_sector(fsi, file, 0);
        meta.last_sector = simplefs_file_sector(fsi, file, fsi->file_sectors - 1);
        meta.size = fsi->file_sectors * SIMPLEFS_SECTOR_SIZE;

        err = file_crc32(sb, file, &meta.crc32);
        if (err)
            return err;

        if (copy_to_user(&uarg->files[file], &meta, sizeof(meta)))
            return -EFAULT;
    }

    printk(KERN_INFO "simplefs: метаинформация файлов получена\n");
    return 0;
}

static int ioctl_map(struct super_block *sb, struct simplefs_ioc_map __user *uarg)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    struct simplefs_ioc_map req;
    u32 block, sector;

    if (copy_from_user(&req, uarg, sizeof(req)))
        return -EFAULT;

    if (req.file >= fsi->file_count)
        return -ENOENT;

    req.count = fsi->file_sectors;

    if (copy_to_user(uarg, &req, sizeof(req)))
        return -EFAULT;

    if (req.capacity < req.count)
        return -ENOSPC;

    for (block = 0; block < fsi->file_sectors; block++) {
        sector = simplefs_file_sector(fsi, req.file, block);
        if (copy_to_user(&uarg->sectors[block], &sector, sizeof(sector)))
            return -EFAULT;
    }

    printk(KERN_INFO "simplefs: физические сектора файла %u получены\n", req.file);
    return 0;
}

long simplefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct super_block *sb = file_inode(file)->i_sb;

    switch (cmd) {
        case SIMPLEFS_IOC_ZERO:
            return ioctl_zero(sb);
        case SIMPLEFS_IOC_ERASE:
            return ioctl_erase(sb);
        case SIMPLEFS_IOC_META:
            return ioctl_meta(sb, (void __user *)arg);
        case SIMPLEFS_IOC_MAP:
            return ioctl_map(sb, (void __user *)arg);
        default:
            printk(KERN_INFO "simplefs: неизвестная команда IOCTL %u\n", cmd);
            return -ENOTTY;
    }
}
