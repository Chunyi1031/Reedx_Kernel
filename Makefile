.SILENT:

all:
	./BuildK

clean:
	rm -rf ./build ./kernel.elf

tools:
	rm ./BuildK
	g++ tools/build.cpp -o ./BuildK
	mkdir mnt

clean_all:
	rm -rf build BuildK

MNT_DIR = ./mnt
SYSTEM_DISK = ./System.img #虚拟磁盘
SYSTEM_DISK_SIZE = 256     #虚拟磁盘容量（MB）

disk:
	dd if=/dev/zero of=$(SYSTEM_DISK) bs=1M count=$(SYSTEM_DISK_SIZE)
	# 使用parted创建ESP分区
	sudo parted $(SYSTEM_DISK) mklabel gpt
	sudo parted $(SYSTEM_DISK) mkpart primary fat32 1MiB 201MiB
	sudo parted $(SYSTEM_DISK) set 1 esp on
	sudo parted $(SYSTEM_DISK) print
	echo "Disk:$(SYSTEM_DISK) Size:$(SYSTEM_DISK_SIZE)MB"
	echo "Formatting..."
	sudo losetup -Pf $(SYSTEM_DISK)
	LOOP_DEV=$$(sudo losetup -j $(SYSTEM_DISK) | cut -d: -f1); \
	sudo mkfs.fat -F 32 $${LOOP_DEV}p1; \
	sudo losetup -d $$LOOP_DEV
	echo "Done!"
	sudo mount -o loop,offset=1048576 $(SYSTEM_DISK) $(MNT_DIR)
	sudo mkdir -p $(MNT_DIR)/EFI/BOOT
	sudo mkdir $(MNT_DIR)/SYS
	sudo cp data/BOOTX64.EFI $(MNT_DIR)/EFI/BOOT/BOOTX64.EFI
	sudo cp data/font.bin $(MNT_DIR)/SYS/KNLFNT.BIN
	sudo umount $(MNT_DIR)

system:
	sudo mount -o loop,offset=1048576 $(SYSTEM_DISK) $(MNT_DIR)
	sudo cp kernel.elf $(MNT_DIR)/SYS/KERNEL.ELF
	sudo umount $(MNT_DIR)

run:
	qemu-system-x86_64 -m 1G -bios OVMF.fd -hda $(SYSTEM_DISK)

run-debug:
	qemu-system-x86_64 -m 1G -bios OVMF.fd -hda $(SYSTEM_DISK) -s -S

.PHONY:all clean tools clean_all system run-debug