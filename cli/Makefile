all: userspace

userspace: userspace.c ../kernal/fs_ioctl.h
	gcc -o userspace userspace.c

clean:
	rm -f userspace
