#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */

#define _F2FS_DEBUG

#define N_CLUSTERS 3

#define DEF_HC_THREAD_MIN_SLEEP_TIME	30000	/* milliseconds */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	60000
#define DEF_HC_THREAD_NOHC_SLEEP_TIME	300000	/* wait 5 min */

extern nid_t last_ino;
extern nid_t last2_ino;
#define MAX_SEGNO 4*1048576
extern char segment_valid[MAX_SEGNO];

/* 热度定义 */
struct hotness_entry
{
	block_t blk_addr;/* 块地址 */
	unsigned int IRR;/* 最近两次更新间隔时间 */
	unsigned int LWS;/* 最后一次更新时间 */
	struct list_head list;
	#ifdef _F2FS_DEBUG
		struct hotness_entry_info *hei;
	#endif
};

struct hotness_entry_info
{
	/* from struct f2fs_io_info */
	nid_t ino;		/* inode number */
	enum page_type type;	/* contains DATA/NODE/META/META_FLUSH */
	enum temp_type temp;	/* contains HOT/WARM/COLD */
	enum iostat_type io_type;	/* io type */

	/* from struct f2fs_summary */
	__le32 nid;		/* parent node id */
	__le16 ofs_in_node;	/* block index in parent node */

	unsigned int segno;
};
extern struct kmem_cache *hotness_entry_info_slab;

/* 热度元数据组织 */
struct hc_list {
	struct list_head ilist; // 16 bytes
	struct radix_tree_root iroot; // 16 bytes
	unsigned  count; // number of hotness entry
	unsigned int new_blk_cnt;
	unsigned int new_blk_compress_cnt;
	unsigned int upd_blk_cnt;
};
extern struct hc_list *hc_list_ptr;

/* 热度聚类 */
struct f2fs_hc_kthread {
	struct task_struct *f2fs_hc_task;
	wait_queue_head_t hc_wait_queue_head;

	/* for hc sleep time */
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
	unsigned int no_hc_sleep_time;
};

struct hotness_manage {
    struct work_struct work;
    struct hotness_entry he;
};

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he);
// int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he);
struct hotness_entry *lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr);
int delete_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr);
void save_hotness_entry(struct f2fs_sb_info *sbi);
void load_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);

#endif