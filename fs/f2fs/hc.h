#ifndef _LINUX_HC_H
#define _LINUX_HC_H

#include <linux/timex.h>
#include <linux/workqueue.h>    /* for work queue */
#include <linux/slab.h>         /* for kmalloc() */
#include <linux/list_lru.h>

// #define F2FS_DEBUG
// #define F2FS_CONCURRENT
// #define F2FS_PTIME
// #define F2FS_PTIME_HC

#define DEF_HC_THREAD_MIN_SLEEP_TIME	3000	/* milliseconds */
#define DEF_HC_THREAD_MAX_SLEEP_TIME	60000
#define DEF_HC_THREAD_NOHC_SLEEP_TIME	300000	/* wait 5 min */

// #define DEF_HC_HOTNESS_ENTRY_MAX_NUM 16000
#define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD 200000
// #define DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD __UINT32_MAX__
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

extern struct workqueue_struct *wq;
extern struct kmem_cache *hotness_manage_slab;
extern spinlock_t count_lock;

struct hotness_manage {
    struct work_struct work;
	struct f2fs_sb_info *sbi;
	block_t blkaddr_new;
	block_t blkaddr_old;
	__u64 value;
	int type_old;
	int type_new;
};

/* 热度元数据组织 */
struct hotness_info {
	struct radix_tree_root hotness_rt_array[3]; 
	// struct radix_tree_root hotness_rt_array_hot;
	// struct radix_tree_root hotness_rt_array_warm;
	// struct radix_tree_root hotness_rt_array_cold;

	unsigned int count; // number of hotness entry
	unsigned int new_blk_cnt;
	unsigned int upd_blk_cnt;
	unsigned int rmv_blk_cnt;
	unsigned int ipu_blk_cnt;
	unsigned int opu_blk_cnt;
	// 记录3种温度类别的一些信息
	unsigned int counts[TEMP_TYPE_NUM];
	unsigned int IRR_min[TEMP_TYPE_NUM];
	unsigned int IRR_max[TEMP_TYPE_NUM];
};
extern struct hotness_info *hotness_info_ptr;

/* 热度聚类 */
struct f2fs_hc_kthread {
	struct task_struct *f2fs_hc_task;
	wait_queue_head_t hc_wait_queue_head;

	/* for hc sleep time */
	unsigned int min_sleep_time;
	unsigned int max_sleep_time;
	unsigned int no_hc_sleep_time;
};

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type);
int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new, __u64 value, int type_old, int type_new);
__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int* type);
void reduce_hotness_entry(struct f2fs_sb_info *sbi);
void save_hotness_entry(struct f2fs_sb_info *sbi);
void release_hotness_entry(struct f2fs_sb_info *sbi);

void insert_hotness_entry_work(struct work_struct *work);
void update_hotness_entry_work(struct work_struct *work);
void reduce_hotness_entry_work(struct work_struct *work);

int hotness_decide(struct f2fs_io_info *fio, int *type_old_ptr, __u64 *value_ptr);
void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value);

#endif
