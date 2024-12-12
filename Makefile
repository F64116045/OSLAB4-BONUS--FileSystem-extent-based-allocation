KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += osfs.o

osfs-objs := super.o inode.o file.o dir.o osfs_init.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod osfs.ko
	sudo mount -t osfs none mnt/

unload:
	sudo umount mnt/
	sudo rmmod osfs

test:
	cd mnt
	sudo touch test1.txt
	ls
	sudo bash -c "echo 'I LOVE OSLAB' > test1.txt"


