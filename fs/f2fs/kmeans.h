#ifndef _LINUX_KMEANS_H
#define _LINUX_KMEANS_H 

int f2fs_hc(struct hotness_info *hotness_info_ptr, struct f2fs_sb_info *sbi);
int kmeans_get_type(struct f2fs_io_info *fio, __u32 IRR);

#endif
