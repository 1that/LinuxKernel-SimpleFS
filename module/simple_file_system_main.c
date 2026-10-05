#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/statfs.h>

#include "simple_file_system_main.h"
#include "fs_ioctl.h"

static char *device_name;
static unsigned int name_max = SIMPLEFS_NAME_MAX_LEN;
static unsigned int max_file_sectors = SIMPLEFS_MAX_FILE_SECTORS;
static unsigned int sb0_sector = SIMPLEFS_DEFAULT_SB0_SECTOR;
static unsigned int sb1_sector = SIMPLEFS_DEFAULT_SB1_SECTOR;

module_param(device_name, charp, 0444);
module_param(name_max, uint, 0444);
module_param(max_file_sectors, uint, 0444);
module_param(sb0_sector, uint, 0444);
module_param(sb1_sector, uint, 0444);


static u32 simplefs_checksum(const struct simplefs_super *fssb)
{
    struct simplefs_super tmp = *fssb;
    tmp.checksum = 0;
    return crc32_le(~0, (const u8 *)&tmp, sizeof(tmp));
}

u32 simplefs_file_sector(const struct simplefs_info *fsi, u32 file, u32 block)
{
    u32 low = fsi->sb0_sector < fsi->sb1_sector ? fsi->sb0_sector : fsi->sb1_sector;
    u32 high = fsi->sb0_sector < fsi->sb1_sector ? fsi->sb1_sector : fsi->sb0_sector;
    u32 phys = file * fsi->file_sectors + block;

    if (phys >= low)
        phys++;

    if (phys >= high)
        phys++;

    return phys;
}

static bool simplefs_super_valid(const struct simplefs_super *fssb, u32 total)
{
    u32 k = le32_to_cpu(fssb->file_sectors);

    if (le32_to_cpu(fssb->magic) != SIMPLEFS_MAGIC)
        return false;
    if (le32_to_cpu(fssb->checksum) != simplefs_checksum(fssb))
        return false;
    if (le32_to_cpu(fssb->sector_size) != SIMPLEFS_SECTOR_SIZE ||
        le32_to_cpu(fssb->total_sectors) != total ||
        le32_to_cpu(fssb->sb0_sector) != sb0_sector ||
        le32_to_cpu(fssb->sb1_sector) != sb1_sector)
        return false;
    if (k < 1 || k > total - 2 ||
        le32_to_cpu(fssb->file_count) != (total - 2) / k)
        return false;
    return true;
}

static int simplefs_write_super(struct buffer_head *bh, const struct simplefs_super *fssb)
{
    lock_buffer(bh);
    memset(bh->b_data, 0, SIMPLEFS_SECTOR_SIZE);
    memcpy(bh->b_data, fssb, sizeof(*fssb));
    unlock_buffer(bh);
    mark_buffer_dirty(bh);
    return sync_dirty_buffer(bh);
}

static void simplefs_make_super(struct simplefs_super *fssb, u32 total, u32 k)
{
    memset(fssb, 0, sizeof(*fssb));
    fssb->magic = cpu_to_le32(SIMPLEFS_MAGIC);
    fssb->sector_size = cpu_to_le32(SIMPLEFS_SECTOR_SIZE);
    fssb->total_sectors = cpu_to_le32(total);
    fssb->sb0_sector = cpu_to_le32(sb0_sector);
    fssb->sb1_sector = cpu_to_le32(sb1_sector);
    fssb->file_sectors = cpu_to_le32(k);
    fssb->file_count = cpu_to_le32((total - 2) / k);
    fssb->checksum = cpu_to_le32(simplefs_checksum(fssb));
}

static int simplefs_load_super(struct super_block *sb, u32 k_opt)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    sector_t nr = bdev_nr_sectors(sb->s_bdev);
    struct buffer_head *bh0 = NULL, *bh1 = NULL;
    struct simplefs_super good;
    bool ok0, ok1;
    u32 total;
    int err = 0;

    if (nr < 3 || nr > U32_MAX) {
        printk(KERN_ERR "simplefs: ошибка размера диска %llu секторов\n", (unsigned long long)nr);
        return -EINVAL;
    }
    
    total = nr;

    if (sb0_sector >= total || sb1_sector >= total) {
        printk(KERN_ERR "simplefs: нет места для суперблоков\n");
        return -EINVAL;
    }

    bh0 = sb_bread(sb, sb0_sector);
    bh1 = sb_bread(sb, sb1_sector);

    if (!bh0 || !bh1) {
        printk(KERN_ERR "simplefs: ошибка чтения суперблока\n");
        err = -EIO;
        goto out;
    }

    struct simplefs_super* sb0 = (void *)bh0->b_data;
    struct simplefs_super* sb1 = (void *)bh1->b_data;

    ok0 = simplefs_super_valid(sb0, total);
    ok1 = simplefs_super_valid(sb1, total);

    if (ok0 && ok1) {
        good = *sb0;
        printk(KERN_INFO "simplefs: оба суперблока (сектора %u и %u)\n", sb0_sector, sb1_sector);
    } else if (ok0 && !ok1) {
        good = *sb0;
        printk(KERN_INFO "simplefs: суперблок в секторе %u испорчен, восстанавливается из сектора %u\n", sb1_sector, sb0_sector);
        err = simplefs_write_super(bh1, &good);
    } else if (!ok0 && ok1) {
        good = *sb1;
        printk(KERN_INFO "simplefs: суперблок в секторе %u испорчен, восстанавливается из сектора %u\n", sb0_sector, sb1_sector);
        err = simplefs_write_super(bh0, &good);
    } else if (!memchr_inv(bh0->b_data, 0, SIMPLEFS_SECTOR_SIZE) && !memchr_inv(bh1->b_data, 0, SIMPLEFS_SECTOR_SIZE)) {
        u32 k = k_opt ? k_opt : SIMPLEFS_DEFAULT_FILE_SECTORS;

        if (k > max_file_sectors || k > total - 2) {
            printk(KERN_INFO "simplefs: K=%u недопустим\n", k);
            err = -EINVAL;
            goto out;
        }
        simplefs_make_super(&good, total, k);
        printk(KERN_INFO "simplefs: форматируем с K=%u\n", k);
        err = simplefs_write_super(bh0, &good);
        if (!err)
            err = simplefs_write_super(bh1, &good);
    } else {
        printk(KERN_ERR "simplefs: на устройстве нет simplefs\n");
        err = -EINVAL;
    }
    if (err)
        goto out;

    if (le32_to_cpu(good.file_sectors) > max_file_sectors) {
        printk(KERN_ERR "simplefs: на диске K=%u, а max_file_sectors=%u\n",
               le32_to_cpu(good.file_sectors), max_file_sectors);
        err = -EINVAL;
        goto out;
    }
    if (k_opt && k_opt != le32_to_cpu(good.file_sectors)) {
        printk(KERN_ERR "simplefs: диск уже отформатирован с K=%u, file_sectors=%u не применить\n",
               le32_to_cpu(good.file_sectors), k_opt);
        err = -EINVAL;
        goto out;
    }

    fsi->total_sectors = le32_to_cpu(good.total_sectors);
    fsi->sb0_sector = le32_to_cpu(good.sb0_sector);
    fsi->sb1_sector = le32_to_cpu(good.sb1_sector);
    fsi->file_sectors = le32_to_cpu(good.file_sectors);
    fsi->file_count = le32_to_cpu(good.file_count);

    printk(KERN_INFO "simplefs: секторов %u, суперблоки в %u и %u, файлов %u по %u секторов\n",
        fsi->total_sectors, fsi->sb0_sector, fsi->sb1_sector, fsi->file_count, fsi->file_sectors);
           
out:
    brelse(bh0);
    brelse(bh1);
    return err;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct simplefs_info *fsi = dentry->d_sb->s_fs_info;

    buf->f_type = SIMPLEFS_MAGIC;
    buf->f_bsize = SIMPLEFS_SECTOR_SIZE;
    buf->f_blocks = fsi->total_sectors;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = fsi->file_count;
    buf->f_ffree = 0;
    buf->f_namelen = name_max;

    return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct super_operations simplefs_super_ops = {
    .statfs = simplefs_statfs,
    .put_super = simplefs_put_super,
};

static int file_inode_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr)
{
    if (attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID))
        return -EPERM;
    attr->ia_valid &= ~ATTR_SIZE;
    return simple_setattr(idmap, dentry, attr);
}

static const struct inode_operations simplefs_file_iops = {
    .setattr = file_inode_setattr,
};

static ssize_t read_file(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct simplefs_info *fsi = sb->s_fs_info;
    u32 nfile = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
    loff_t pos = *ppos;
    size_t done = 0;

    if (pos >= inode->i_size)
        return 0;
    if (len > inode->i_size - pos)
        len = inode->i_size - pos;

    while (done < len) {
        u32 block = pos / SIMPLEFS_SECTOR_SIZE;
        u32 offset = pos % SIMPLEFS_SECTOR_SIZE;
        size_t n = min_t(size_t, len - done, SIMPLEFS_SECTOR_SIZE - offset);
        struct buffer_head *bh;

        bh = sb_bread(sb, simplefs_file_sector(fsi, nfile, block));
        if (!bh)
            return done ? done : -EIO;
        
        if (copy_to_user(buf + done, bh->b_data + offset, n)) {
            brelse(bh);
            return done ? done : -EFAULT;
        }

        brelse(bh);
        done += n;
        pos += n;
        *ppos = pos;
    }
    return done;
}

static ssize_t write_file(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct simplefs_info *fsi = sb->s_fs_info;
    u32 nfile = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
    loff_t pos = *ppos;
    size_t done = 0;

    if (len == 0)
        return 0;
    if (pos >= inode->i_size)
        return -EFBIG;
    if (len > inode->i_size - pos)
        len = inode->i_size - pos;

    inode_lock(inode);
    while (done < len) {
        u32 block = pos / SIMPLEFS_SECTOR_SIZE;
        u32 offset = pos % SIMPLEFS_SECTOR_SIZE;
        size_t n = min_t(size_t, len - done, SIMPLEFS_SECTOR_SIZE - offset);
        struct buffer_head *bh;

        bh = sb_bread(sb, simplefs_file_sector(fsi, nfile, block));

        if (!bh) {
            if (!done)
                done = -EIO;
            break;
        }

        if (copy_from_user(bh->b_data + offset, buf + done, n)) {
            brelse(bh);
            if (!done)
                done = -EFAULT;
            break;
        }

        mark_buffer_dirty(bh);
        brelse(bh);
        done += n;
        pos += n;
        *ppos = pos;
    }
    inode_unlock(inode);
    return done;
}

static const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .llseek = generic_file_llseek,
    .read = read_file,
    .write = write_file,
};

static struct inode *simplefs_get_file_inode(struct super_block *sb, u32 nfile)
{
    struct simplefs_info *fsi = sb->s_fs_info;
    struct inode *inode;

    if (nfile >= fsi->file_count)
        return ERR_PTR(-ENOENT);

    inode = iget_locked(sb, SIMPLEFS_FIRST_FILE_INO + nfile);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    if (!(inode->i_state & I_NEW))
        return inode;

    inode->i_uid = GLOBAL_ROOT_UID;
    inode->i_gid = GLOBAL_ROOT_GID;
    simple_inode_init_ts(inode);

    inode->i_mode = S_IFREG | 0666;
    inode->i_size = (loff_t)fsi->file_sectors * SIMPLEFS_SECTOR_SIZE;
    inode->i_blocks = fsi->file_sectors;
    inode->i_op = &simplefs_file_iops;
    inode->i_fop = &simplefs_file_ops;
    set_nlink(inode, 1);

    unlock_new_inode(inode);
    return inode;
}

static struct dentry *dir_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct simplefs_info *fsi = dir->i_sb->s_fs_info;
    const char *name = dentry->d_name.name;
    unsigned int len = dentry->d_name.len;
    struct inode *inode = NULL;
    unsigned int i;
    u64 n = 0;

    if (len > name_max)
        return ERR_PTR(-ENAMETOOLONG);
    if (len <= SIMPLEFS_NAME_PREFIX_LEN || len > SIMPLEFS_NAME_MAX_LEN ||
        memcmp(name, SIMPLEFS_NAME_PREFIX, SIMPLEFS_NAME_PREFIX_LEN))
        goto out;
    if (name[SIMPLEFS_NAME_PREFIX_LEN] == '0' && len > SIMPLEFS_NAME_PREFIX_LEN + 1)
        goto out;
    for (i = SIMPLEFS_NAME_PREFIX_LEN; i < len; i++) {
        if (name[i] < '0' || name[i] > '9')
            goto out;
        n = n * 10 + (name[i] - '0');
    }
    if (n >= fsi->file_count)
        goto out;

    inode = simplefs_get_file_inode(dir->i_sb, n);
    if (IS_ERR(inode))
        return ERR_CAST(inode);
out:
    return d_splice_alias(inode, dentry);
}

static const struct inode_operations simplefs_dir_iops = {
    .lookup = dir_lookup,
};

static int ls_dir(struct file *file, struct dir_context *ctx)
{
    struct simplefs_info *fsi = (file_inode(file)->i_sb)->s_fs_info;
    char name[SIMPLEFS_NAME_MAX_LEN + 1];
    int len;

    if (!dir_emit_dots(file, ctx))
        return 0;

    while (ctx->pos < 2 + (loff_t)fsi->file_count) {
        u32 i = ctx->pos - 2;

        len = snprintf(name, sizeof(name), SIMPLEFS_NAME_PREFIX "%u", i);
        if (!dir_emit(ctx, name, len, SIMPLEFS_FIRST_FILE_INO + i, DT_REG))
            return 0;
        ctx->pos++;
    }
    return 0;
}

static const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .read = generic_read_dir,
    .iterate_shared = ls_dir,
    .unlocked_ioctl = simplefs_ioctl,
};

static struct inode *simplefs_get_root_inode(struct super_block *sb)
{
    struct inode *inode = new_inode(sb);

    if (!inode)
        return NULL;

    inode->i_ino = SIMPLEFS_ROOT_INO;
    inode->i_uid = GLOBAL_ROOT_UID;
    inode->i_gid = GLOBAL_ROOT_GID;
    simple_inode_init_ts(inode);

    inode->i_mode = S_IFDIR | 0755;
    inode->i_op = &simplefs_dir_iops;
    inode->i_fop = &simplefs_dir_ops;
    set_nlink(inode, 2);
    return inode;
}

static int simplefs_parse_options(char *data, u32 *k)
{
    char *opt;

    *k = 0;
    while ((opt = strsep(&data, ",")) != NULL) {
        if (!*opt)
            continue;
        if (!strncmp(opt, "file_sectors=", 13)) {
            if (kstrtou32(opt + 13, 10, k) || *k == 0)
                return -EINVAL;
        } else {
            printk(KERN_INFO "simplefs: неизвестная опция \"%s\"\n", opt);
            return -EINVAL;
        }
    }
    return 0;
}

static int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct simplefs_info *fsi;
    struct inode *inode;
    char name[SIMPLEFS_NAME_MAX_LEN + 1];
    u32 k_opt;
    int err;

    if (!device_name) {
        printk(KERN_INFO "simplefs: не задан параметр модуля device_name\n");
        return -EINVAL;
    }
    if (strcmp(sb->s_bdev->bd_disk->disk_name, kbasename(device_name))) {
        printk(KERN_INFO "simplefs: диск %s, а нужен %s\n", sb->s_bdev->bd_disk->disk_name, device_name);
        return -EINVAL;
    }

    err = simplefs_parse_options(data, &k_opt);
    if (err)
        return err;

    if (!sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE)) {
        printk(KERN_INFO "simplefs: не удалось установить размер блока %d\n", SIMPLEFS_SECTOR_SIZE);
        return -EINVAL;
    }

    sb->s_magic = SIMPLEFS_MAGIC;
    sb->s_op = &simplefs_super_ops;

    fsi = kzalloc(sizeof(struct simplefs_info), GFP_KERNEL);
    if (!fsi)
        return -ENOMEM;
        
    sb->s_fs_info = fsi;

    err = simplefs_load_super(sb, k_opt);
    if (err)
        return err;

    if ((unsigned int)snprintf(name, sizeof(name), SIMPLEFS_NAME_PREFIX "%u", fsi->file_count - 1) > name_max) {
        printk(KERN_INFO "simplefs: имя %s длиннее name_max=%u\n", name, name_max);
        return -EINVAL;
    }

    sb->s_maxbytes = (loff_t)fsi->file_sectors * SIMPLEFS_SECTOR_SIZE;

    inode = simplefs_get_root_inode(sb);
    if (!inode)
        return -ENOMEM;

    sb->s_root = d_make_root(inode);
    if (!sb->s_root)
        return -ENOMEM;

    printk(KERN_INFO "simplefs: смонтирована\n");
    return 0;
}

static struct dentry *simplefs_mount(struct file_system_type *type, int flags, const char *dev_name, void *data)
{
    return mount_bdev(type, flags, dev_name, data, simplefs_fill_super);
}

static void simplefs_kill_sb(struct super_block *sb)
{
    kill_block_super(sb);
    kfree(sb->s_fs_info);
    printk(KERN_INFO "simplefs: размонтирована\n");
}

static struct file_system_type simplefs_type = {
    .owner = THIS_MODULE,
    .name = "simplefs",
    .mount = simplefs_mount,
    .kill_sb = simplefs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init simplefs_init(void)
{
    int err;

    if (name_max < SIMPLEFS_NAME_PREFIX_LEN + 1 || name_max > NAME_MAX) {
        printk(KERN_INFO "simplefs: name_max=%u вне возможного диапазона\n", name_max);
        return -EINVAL;
    }
    if (sb0_sector == sb1_sector) {
        printk(KERN_INFO "simplefs: sb0_sector и sb1_sector совпадают в %u\n", sb0_sector);
        return -EINVAL;
    }
    if (max_file_sectors < 1) {
        printk(KERN_INFO "simplefs: max_file_sectors должен быть >= 1\n");
        return -EINVAL;
    }

    err = register_filesystem(&simplefs_type);

    if (err)
        printk(KERN_INFO "simplefs: ошибка регистрации файловой системы %d\n", err);
    else
        printk(KERN_INFO "simplefs: модуль загружен\n");
    return err;
}

static void __exit simplefs_exit(void)
{
    unregister_filesystem(&simplefs_type);

    printk(KERN_INFO "simplefs: модуль выгружен\n");
}

module_init(simplefs_init);
module_exit(simplefs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("home work");
