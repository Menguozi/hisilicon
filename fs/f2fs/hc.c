#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/slab_def.h>
// #include <include/linux/xarray.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"
// #include <trace/events/f2fs.h>

static DEFINE_SPINLOCK(list_lock);
static DEFINE_MUTEX(list_mutex);
DEFINE_SPINLOCK(count_lock);
static DEFINE_SPINLOCK(shrink_lock);
static struct kmem_cache *hotness_entry_slab;
struct kmem_cache *hotness_manage_slab;
struct kmem_cache *hotness_entry_info_slab;
struct hc_list *hc_list_ptr;
struct workqueue_struct *wq;
nid_t last_ino;
char segment_valid[MAX_SEGNO];

/* 热度元数据操作 */
/* 1、添加 */
// create
int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he, block_t write_count)
{
	#ifdef F2FS_PTIME
	struct timespec64 ts_start, ts_end;
	struct timespec64 ts_delta;
	ktime_get_boottime_ts64(&ts_start);
	#endif

	he = f2fs_kmem_cache_alloc(hotness_entry_slab, GFP_NOFS);
	he->blk_addr = blkaddr;
	he->IRR = __UINT32_MAX__;
	he->LWS = write_count;
	hc_list_ptr->count++;
	f2fs_radix_tree_insert(&hc_list_ptr->iroot, blkaddr, he);
	// spin_lock(&list_lock);
	// mutex_lock(&list_mutex);
	list_add_tail_rcu(&he->list, &hc_list_ptr->ilist);
	// spin_unlock(&list_lock);
	// mutex_unlock(&list_mutex);

	#ifdef F2FS_PTIME
	ktime_get_boottime_ts64(&ts_end);
	ts_delta = timespec64_sub(ts_end, ts_start);
	printk("%s: time cost: %lld\n", __func__, timespec64_to_ns(&ts_delta));
	#endif

	// printk("%s, slab object_size is %u, slab size is %u, kmem_cache_size(hotness_entry_slab) = %u\n", __func__, hotness_entry_slab->object_size, hotness_entry_slab->size, kmem_cache_size(hotness_entry_slab));

	return 0;
}

/**
 * @brief 更新
 * 
 * @param sbi 
 * @param old_blkaddr 
 * @param new_blkaddr 
 * @param he 
 * @return int 
 */
int update_hotness_entry(struct f2fs_sb_info *sbi, block_t old_blkaddr, block_t new_blkaddr, struct hotness_entry *he)
{
	#ifdef F2FS_PTIME
	struct timespec64 ts_start, ts_end;
	struct timespec64 ts_delta;
	ktime_get_boottime_ts64(&ts_start);
	#endif

	f2fs_radix_tree_insert(&hc_list_ptr->iroot, new_blkaddr, he);
	radix_tree_delete(&hc_list_ptr->iroot, old_blkaddr);
	// spin_lock(&list_lock);
	// mutex_lock(&list_mutex);
	list_del_rcu(&he->list);
	list_add_tail_rcu(&he->list, &hc_list_ptr->ilist);
	// spin_unlock(&list_lock);
	// mutex_unlock(&list_mutex);

	#ifdef F2FS_PTIME
	ktime_get_boottime_ts64(&ts_end);
	ts_delta = timespec64_sub(ts_end, ts_start);
	printk("%s: time cost: %lld\n", __func__, timespec64_to_ns(&ts_delta));
	#endif

	return 0;
}

/* 2、查询 */
struct hotness_entry *lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	#ifdef F2FS_PTIME
	struct timespec64 ts_start, ts_end;
	struct timespec64 ts_delta;
	ktime_get_boottime_ts64(&ts_start);
	#endif

	struct hotness_entry *he;
	if (hc_list_ptr->iroot.xa_head == NULL) return NULL;
	he = radix_tree_lookup(&hc_list_ptr->iroot, blkaddr);

	#ifdef F2FS_PTIME
	ktime_get_boottime_ts64(&ts_end);
	ts_delta = timespec64_sub(ts_end, ts_start);
	printk("%s: time cost: %lld\n", __func__, timespec64_to_ns(&ts_delta));
	#endif

	return he;
}

/* 3、移除 */
int delete_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct hotness_entry *he;
	he = radix_tree_lookup(&hc_list_ptr->iroot, blkaddr);
	if (he) {
		hc_list_ptr->count--;
		radix_tree_delete(&hc_list_ptr->iroot, he->blk_addr);
		// spin_lock(&list_lock);
		// mutex_lock(&list_mutex);
		list_del_rcu(&he->list);
		// spin_unlock(&list_lock);
		// mutex_unlock(&list_mutex);
		kmem_cache_free(hotness_entry_slab, he);
		return 0;
	}
	return -1;
}

void shrink_hotness_entry(void) {
	struct hotness_entry *he, *he_next;
	unsigned int num;
	block_t blkaddr;
	printk("Calling function %s\n", __func__);
	// spin_lock(&list_lock);
	mutex_lock(&list_mutex);
	if (hc_list_ptr->count < DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		// spin_unlock(&list_lock);
		mutex_unlock(&list_mutex);
		return;
	}
	he_next = list_first_entry(&hc_list_ptr->ilist, struct hotness_entry, list);
	for (num = 0; num < DEF_HC_HOTNESS_ENTRY_SHRINK_NUM && !list_entry_is_head(he_next, &hc_list_ptr->ilist, list); num++) {
		he = he_next;
		blkaddr = he->blk_addr;
		he_next = list_next_entry(he, list);
		// printk("In function %s, remove hotness entry whose blkaddr = %u\n", __func__, blkaddr);
		radix_tree_delete(&hc_list_ptr->iroot, blkaddr);
		list_del_rcu(&he->list);
		// synchronize_rcu();
		kmem_cache_free(hotness_entry_slab, he);
		hc_list_ptr->count--;
	}
	hc_list_ptr->rmv_blk_cnt += num;
	// spin_unlock(&list_lock);
	mutex_unlock(&list_mutex);
}

void insert_hotness_entry_work(struct work_struct *work)
{
	// printk("In function: %s\n", __FUNCTION__);
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	insert_hotness_entry(NULL, hm->new_blkaddr, hm->he, hm->write_count);
	kmem_cache_free(hotness_manage_slab, hm);
}

void update_hotness_entry_work(struct work_struct *work)
{
	// printk("In function: %s\n", __FUNCTION__);
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	update_hotness_entry(NULL, hm->old_blkaddr, hm->new_blkaddr, hm->he);
	kmem_cache_free(hotness_manage_slab, hm);
}

void shrink_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	shrink_hotness_entry();
	kmem_cache_free(hotness_manage_slab, hm);
}

int f2fs_create_hotness_clustering_cache(void)
{
	hotness_entry_slab = f2fs_kmem_cache_create("f2fs_hotness_entry", sizeof(struct hotness_entry));
	if (!hotness_entry_slab)
		return -ENOMEM;
	hotness_manage_slab = f2fs_kmem_cache_create("f2fs_hotness_manage", sizeof(struct hotness_manage));
	if (!hotness_manage_slab)
		return -ENOMEM;
	hotness_entry_info_slab = f2fs_kmem_cache_create("f2fs_hotness_entry_info", sizeof(struct hotness_entry_info));
	if (!hotness_entry_info_slab)
		return -ENOMEM;
	return 0;
}

void f2fs_destroy_hotness_clustering_cache(void)
{
	printk("In function %s\n", __func__);
	kmem_cache_destroy(hotness_entry_slab);
	kmem_cache_destroy(hotness_manage_slab);
	kmem_cache_destroy(hotness_entry_info_slab);
}

static void init_hc_management(struct f2fs_sb_info *sbi)
{
	struct file *fp;
	loff_t pos = 0;
	// block_t blk_addr_tmp;
	// unsigned int IRR_tmp;
	// unsigned int LWS_tmp;
	// struct hotness_entry_info *new_hei = NULL;
	static struct hc_list hc_list_var = {
		.ilist = LIST_HEAD_INIT(hc_list_var.ilist),
		.iroot = RADIX_TREE_INIT(hc_list_var.iroot, GFP_NOFS),
		.IRR_min = {__UINT32_MAX__, __UINT32_MAX__, __UINT32_MAX__},
	};
	unsigned int n_clusters;
	unsigned int i;
	unsigned int *centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	unsigned int count;

	hc_list_ptr = &hc_list_var;

	const unsigned int onlinecpus = num_possible_cpus();
	// wq = alloc_workqueue("f2fs-hc-workqueue", WQ_HIGHPRI | WQ_CPU_INTENSIVE, 512);
	// wq = alloc_workqueue("f2fs-hc-workqueue", WQ_UNBOUND | WQ_HIGHPRI, onlinecpus + onlinecpus / 4);
	wq = alloc_workqueue("f2fs-hc-workqueue", WQ_UNBOUND | WQ_HIGHPRI, onlinecpus / 4);

	fp = filp_open("/tmp/f2fs_hotness_no", O_RDWR, 0644);
	if (IS_ERR(fp)) {
		printk("failed to open /tmp/f2fs_hotness.\n");
		sbi->total_writed_block_count = 0;
		sbi->n_clusters = N_CLUSTERS;
		sbi->centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
		sbi->centers_valid = 0;
		goto out;
	}

	printk(">>>>>>>>>>>\n");
	kernel_read(fp, &n_clusters, sizeof(n_clusters), &pos);
	printk("n_clusters = %u, pos = %llu\n", n_clusters, pos);
	sbi->n_clusters = n_clusters;

	// read centers
	for(i = 0; i < n_clusters; ++i) {
		kernel_read(fp, &centers[i], sizeof(centers[i]), &pos);
		printk("%u, 0x%x\n", centers[i], centers[i]);
	}
	sbi->centers = centers;
	sbi->centers_valid = 1;

	// read count
	kernel_read(fp, &count, sizeof(count), &pos);
	printk("%u, 0x%x\n", count, count);
	sbi->total_writed_block_count = count;

	// read blk_addr & IRR & LWS for each block to init hc_list
	for(i = 0; i < count; i++) {
		struct hotness_entry *he = f2fs_kmem_cache_alloc(hotness_entry_slab, GFP_NOFS);;
		kernel_read(fp, &he->blk_addr, sizeof(he->blk_addr), &pos);
		kernel_read(fp, &he->IRR, sizeof(he->IRR), &pos);
		kernel_read(fp, &he->LWS, sizeof(he->LWS), &pos);
		insert_hotness_entry(NULL, he->blk_addr, he, count);
		printk("%u, %u, %u\n", he->blk_addr, he->IRR, he->LWS);
	}

	filp_close(fp, NULL);
out:
	return;
}

void f2fs_build_hc_manager(struct f2fs_sb_info *sbi)
{
	printk("In f2fs_build_hc_manager\n");
	init_hc_management(sbi);
	last_ino = __UINT32_MAX__;
	memset(segment_valid, 0, MAX_SEGNO);
	printk("Finish f2fs_build_hc_manager\n");
}

static int kmeans_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	wait_queue_head_t *wq = &sbi->hc_thread->hc_wait_queue_head;
	unsigned int wait_ms;
	int err;

	printk("In kmeans_thread_func\n");
	
	wait_ms = hc_th->min_sleep_time;

	set_freezable();
	do {
		wait_event_interruptible_timeout(*wq, kthread_should_stop() || freezing(current), msecs_to_jiffies(wait_ms));
		err = f2fs_hc(hc_list_ptr, sbi);
		if (!err) sbi->centers_valid = 1;
	} while (!kthread_should_stop());
	return 0;
}

int f2fs_start_hc_thread(struct f2fs_sb_info *sbi)
{
    struct f2fs_hc_kthread *hc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	printk("In f2fs_start_hc_thread\n");

	hc_th = f2fs_kmalloc(sbi, sizeof(struct f2fs_hc_kthread), GFP_KERNEL);
	if (!hc_th) {
		err = -ENOMEM;
		goto out;
	}

	hc_th->min_sleep_time = DEF_HC_THREAD_MIN_SLEEP_TIME;
	hc_th->max_sleep_time = DEF_HC_THREAD_MAX_SLEEP_TIME;
	hc_th->no_hc_sleep_time = DEF_HC_THREAD_NOHC_SLEEP_TIME;

    sbi->hc_thread = hc_th;
	init_waitqueue_head(&sbi->hc_thread->hc_wait_queue_head);
    sbi->hc_thread->f2fs_hc_task = kthread_run(kmeans_thread_func, sbi,
			"f2fs_hc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(hc_th->f2fs_hc_task)) {
		err = PTR_ERR(hc_th->f2fs_hc_task);
		kfree(hc_th);
		sbi->hc_thread = NULL;
	}
out:
	return err;
}

void f2fs_stop_hc_thread(struct f2fs_sb_info *sbi) 
{
	struct f2fs_hc_kthread *hc_th = sbi->hc_thread;
	
	printk("In f2fs_stop_hc_thread");

	if (!hc_th)
		return;
	kthread_stop(hc_th->f2fs_hc_task);
	kfree(hc_th);
	sbi->hc_thread = NULL;
}

/**
 * @brief 热度元数据持久化
 * 
 * @param sbi 
 */
void save_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct file *fp;
	loff_t pos = 0;
	struct hotness_entry *he;
	unsigned int i;

	printk("In save_hotness_entry");

	// if (!sbi->centers_valid) goto out;
	fp = filp_open("/tmp/f2fs_hotness", O_RDWR|O_CREAT, 0644);
	if (IS_ERR(fp)) goto out;

	// save n_clusters
	kernel_write(fp, &sbi->n_clusters, sizeof(sbi->n_clusters), &pos);
	// printk("pos = 0x%llx\n", pos);
	// save centers
	for(i = 0; i < sbi->n_clusters; i++) {
		kernel_write(fp, &sbi->centers[i], sizeof(sbi->centers[i]), &pos);
		printk("%u, 0x%x\n", sbi->centers[i], sbi->centers[i]);
	}
	// printk("pos = 0x%llx\n", pos);
	// save total_writed_block_count
	kernel_write(fp, &sbi->total_writed_block_count, sizeof(sbi->total_writed_block_count), &pos);
	printk("%u, 0x%x\n", sbi->total_writed_block_count, sbi->total_writed_block_count);
	// printk("pos = 0x%llx\n", pos);
	
	rcu_read_lock();
	list_for_each_entry_rcu(he, &hc_list_ptr->ilist, list){
		kernel_write(fp, &he->blk_addr, sizeof(he->blk_addr), &pos);
		kernel_write(fp, &he->IRR, sizeof(he->IRR), &pos);
		kernel_write(fp, &he->LWS, sizeof(he->LWS), &pos);
	}
	rcu_read_unlock();
	// printk("pos = 0x%llx\n", pos);
	
	filp_close(fp, NULL);
out:
	return;
}

/**
 * @brief 释放内存占用
 */
void release_hotness_entry(struct f2fs_sb_info *sbi)
{
	struct hotness_entry *he, *tmp;
	struct radix_tree_iter iter;
	void __rcu **slot;

	printk("In release_hotness_entry\n");

    // flush_workqueue(wq);
    destroy_workqueue(wq);
    printk("Work queue exit: %s\n", __FUNCTION__);

	printk("----------- 1 -----------\n");
	if (sbi->centers) kfree(sbi->centers);

	printk("----------- 2 -----------\n");
	printk("hc_list_ptr in 0x%p\n", hc_list_ptr);
	printk("count = %u\n", hc_list_ptr->count);
	printk("----------- 2a -----------\n");
	if (hc_list_ptr->count == 0) return;
	printk("----------- 2b -----------\n");
	// spin_lock(&list_lock);
	// mutex_lock(&list_mutex);
	list_for_each_entry_safe(he, tmp, &hc_list_ptr->ilist, list) {
		printk("he in 0x%p\n", he);
		list_del_rcu(&he->list);
		// synchronize_rcu();
		kmem_cache_free(hotness_entry_slab, he);
		hc_list_ptr->count--;
	}
	// spin_unlock(&list_lock);
	// mutex_unlock(&list_mutex);

	printk("----------- 3 -----------\n");
	radix_tree_for_each_slot(slot, &hc_list_ptr->iroot, &iter, 0) {
		radix_tree_delete(&hc_list_ptr->iroot, iter.index);
	}
	printk("----------- 4 -----------\n");

}

unsigned int get_type_threshold(struct hotness_entry *he)
{
	unsigned int IRR = he->IRR;
	return (IRR < THRESHOLD_HOT_WARM) ? CURSEG_HOT_DATA : ((IRR < THRESHOLD_WARM_COLD) ? CURSEG_WARM_DATA : CURSEG_COLD_DATA);
}