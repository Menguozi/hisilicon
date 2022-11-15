#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */

// #define F2FS_DEBUG
// #define F2FS_CONCURRENT
// #define F2FS_PTIME
#define F2FS_PTIME_HC

#define DEF_HC_THREAD_MIN_SLEEP_TIME	30000	/* milliseconds */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	60000
#define DEF_HC_THREAD_NOHC_SLEEP_TIME	300000	/* wait 5 min */

#define DEF_HC_HOTNESS_ENTRY_MAX_NUM 16000
// #define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD 15000
#define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD __UINT32_MAX__
#define DEF_HC_HOTNESS_ENTRY_SHRINK_NUM 10000

#define THRESHOLD_HOT_WARM 29500
#define THRESHOLD_WARM_COLD 30000

enum {
	TYPE_STRATEGY_KMEANS = 0,
	TYPE_STRATEGY_THRESHOLD
};

#define TYPE_STRATEGY TYPE_STRATEGY_KMEANS
#define N_CLUSTERS 3
#define TEMP_TYPE_NUM 3

#define MIN(a, b) ((a) < (b)) ? a : b
#define MAX(a, b) ((a) < (b)) ? b : a

extern nid_t last_ino;
extern nid_t last2_ino;
#define MAX_SEGNO 4*1048576
extern char segment_valid[MAX_SEGNO];
extern struct workqueue_struct *wq;
extern struct kmem_cache *hotness_manage_slab;
extern struct kmem_cache *hotness_entry_info_slab;
extern spinlock_t count_lock;

/* 热度定义 */
struct hotness_entry
{
	block_t blk_addr;/* 块地址 */
	unsigned int IRR;/* 最近两次更新间隔时间 */
	unsigned int LWS;/* 最后一次更新时间 */
	struct list_head list;
	// struct hotness_entry_info *hei;
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

/* 热度元数据组织 */
struct hc_list {
	struct list_head ilist; // 16 bytes
	struct radix_tree_root iroot; // 16 bytes
	unsigned  count; // number of hotness entry
	unsigned int new_blk_cnt;
	unsigned int new_blk_compress_cnt;
	unsigned int upd_blk_cnt;
	unsigned int rmv_blk_cnt;

	// 记录3种温度类别的一些信息
	// unsigned int hot_count;
	// unsigned int hot_IRR_min;
	// unsigned int hot_IRR_max;
	// unsigned int warm_count;
	// unsigned int warm_IRR_min;
	// unsigned int warm_IRR_max;
	// unsigned int cold_count;
	// unsigned int cold_IRR_min;
	// unsigned int cold_IRR_max;
	unsigned int counts[TEMP_TYPE_NUM];
	unsigned int IRR_min[TEMP_TYPE_NUM];
	unsigned int IRR_max[TEMP_TYPE_NUM];
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
    struct hotness_entry *he;
	block_t new_blkaddr;
	block_t old_blkaddr;
	struct f2fs_sb_info *sbi;
	block_t write_count;
};

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he, block_t write_count);
int update_hotness_entry(struct f2fs_sb_info *sbi, block_t old_blkaddr, block_t new_blkaddr,  struct hotness_entry *he);
struct hotness_entry *lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr);
int delete_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr);
void shrink_hotness_entry(void);
void insert_hotness_entry_work(struct work_struct *work);
void update_hotness_entry_work(struct work_struct *work);
void shrink_hotness_entry_work(struct work_struct *work);
void save_hotness_entry(struct f2fs_sb_info *sbi);
void load_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);
unsigned int get_type_threshold(struct hotness_entry *he);

#endif