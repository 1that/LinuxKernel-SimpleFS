#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../kernal/fs_ioctl.h"

static void test(const char *mnt)
{
    DIR *dir = opendir(mnt);
    struct dirent *de;
    char path[512];
    int ok = 0, bad = 0;

    if (!dir) {
        printf("не открывается %s\n", mnt);
        return;
    }
    srand(time(NULL));

    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG)
            continue;

        snprintf(path, sizeof(path), "%s/%s", mnt, de->d_name);
        int fd = open(path, O_RDWR);

        int wrote = rand(), got = 0;
        pwrite(fd, &wrote, sizeof(wrote), 0);
        pread(fd, &got, sizeof(got), 0);
        close(fd);

        if (got == wrote) {
            ok++;
        } else {
            printf("%s: записал %d, прочитал %d\n", de->d_name, wrote, got);
            bad++;
        }
    }
    closedir(dir);
    printf("успешно: %d, ошибок: %d\n", ok, bad);
}

static void meta(int fd)
{
    struct simplefs_ioc_meta probe = { .capacity = 0 };

    ioctl(fd, SIMPLEFS_IOC_META, &probe);

    struct simplefs_ioc_meta *req = malloc(sizeof(*req) + probe.count * sizeof(req->files[0]));

    req->capacity = probe.count;
    if (ioctl(fd, SIMPLEFS_IOC_META, req) < 0) {
        printf("ошибка meta\n");
        free(req);
        return;
    }

    for (unsigned int i = 0; i < req->count; i++)
        printf("%s сектора %u-%u %u байт crc32 %x\n",
               req->files[i].name, req->files[i].first_sector,
               req->files[i].last_sector, req->files[i].size, req->files[i].crc32);
    free(req);
}

static void map(int fd, unsigned int file)
{
    struct simplefs_ioc_map probe = { .file = file, .capacity = 0 };

    if (ioctl(fd, SIMPLEFS_IOC_MAP, &probe) < 0 && probe.count == 0) {
        printf("ошибка map\n");
        return;
    }

    struct simplefs_ioc_map *req = malloc(sizeof(*req) + probe.count * sizeof(req->sectors[0]));

    req->file = file;
    req->capacity = probe.count;
    if (ioctl(fd, SIMPLEFS_IOC_MAP, req) < 0) {
        printf("ошибка map: %s\n", strerror(errno));
        free(req);
        return;
    }

    printf("file%u:", file);
    for (unsigned int i = 0; i < req->count; i++)
        printf(" %u", req->sectors[i]);
    printf("\n");
    free(req);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("использование: %s <mnt_path> test | zero | erase | meta | map N\n", argv[0]);
        return 1;
    }

    const char *mnt = argv[1];
    const char *cmd = argv[2];

    if (!strcmp(cmd, "test")) {
        test(mnt);
        return 0;
    }

    int fd = open(mnt, O_RDONLY);
    if (fd < 0) {
        printf("ошибка: не открывается %s\n", mnt);
        return 1;
    }

    if (!strcmp(cmd, "zero")) {
        if (ioctl(fd, SIMPLEFS_IOC_ZERO) < 0)
            printf("ошибка zero");
    } else if (!strcmp(cmd, "erase")) {
        if (ioctl(fd, SIMPLEFS_IOC_ERASE) < 0)
            printf("ошибка erase");
    } else if (!strcmp(cmd, "meta")) {
        meta(fd);
    } else if (!strcmp(cmd, "map") && argc == 4) {
        map(fd, atoi(argv[3]));
    } else {
        printf("неизвестная команда: %s\n", cmd);
    }

    close(fd);
    return 0;
}
