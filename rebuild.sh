make M=fs/f2fs -j64
sudo umount /mnt/f2fs
sudo rmmod f2fs
sudo insmod /home/menguozi/workspace/hisilicon/fs/f2fs/f2fs.ko
sudo mkfs.f2fs -f /dev/sdc
sudo mount /dev/sdc /mnt/f2fs
sudo cat /sys/kernel/debug/f2fs/status >> start

sudo cat /sys/kernel/debug/f2fs/status >> end
