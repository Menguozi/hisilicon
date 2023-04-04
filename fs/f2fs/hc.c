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
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"

DEFINE_SPINLOCK(count_lock);
static DEFINE_MUTEX(mutex_reduce_he);
static struct kmem_cache *hotness_entry_slab;
struct kmem_cache *hotness_manage_slab;
struct kmem_cache *hotness_entry_info_slab;
struct hc_list *hc_list_ptr;
struct workqueue_struct *wq;
unsigned int max_blkaddr;
nid_t last_ino;
char segment_valid[MAX_SEGNO];

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he, block_t write_count)
{
	he = f2fs_kmem_cache_alloc(hotness_entry_slab, GFP_NOFS);
	he->blk_addr = blkaddr;
	he->IRR = __UINT32_MAX__;
	he->LWS = write_count;
	// he->type = CURSEG_WARM_DATA;
	hc_list_ptr->count++;
	// printk("%s: blkaddr = %u, he = %p\n", __func__, blkaddr, he);
	radix_tree_insert(&hc_list_ptr->iroot, blkaddr, he);
	INIT_LIST_HEAD(&he->list);
	list_lru_add(&hc_list_ptr->lru, &he->list);
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
	// printk("In %s, old_blkaddr = %u, new_blkaddr = %u, IRR = %u\n", __func__, old_blkaddr, new_blkaddr, he->IRR);
	if (old_blkaddr != new_blkaddr) {
		radix_tree_delete(&hc_list_ptr->iroot, old_blkaddr);
		radix_tree_insert(&hc_list_ptr->iroot, new_blkaddr, he);
		// list_lru_del(&hc_list_ptr->lru, &he->list);
		// list_lru_add(&hc_list_ptr->lru, &he->list);
		hc_list_ptr->opu_blk_cnt++;
	} else {
		hc_list_ptr->ipu_blk_cnt++;
	}
	return 0;
}

struct hotness_entry *lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct hotness_entry *he;
	if (hc_list_ptr->iroot.xa_head == NULL) return NULL;
	he = radix_tree_lookup(&hc_list_ptr->iroot, blkaddr);
	return he;
}

void reduce_hotness_entry(struct f2fs_sb_info *sbi) {
	struct hotness_entry *he;
	unsigned int num = 0;
	block_t blkaddr;
	unsigned int x;
	printk("In %s", __func__);
	while (num < DEF_HC_HOTNESS_ENTRY_SHRINK_NUM) {
		get_random_bytes(&x, sizeof(x));
		blkaddr = x % max_blkaddr;
		he = lookup_hotness_entry(sbi, blkaddr);
		if (he) {
			radix_tree_delete(&hc_list_ptr->iroot, blkaddr);
			list_lru_del(&hc_list_ptr->lru, &he->list);
			kmem_cache_free(hotness_entry_slab, he);
			hc_list_ptr->count--;
			num++;
		}
	}
	hc_list_ptr->rmv_blk_cnt += num;
	mutex_unlock(&mutex_reduce_he);
}

void insert_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	insert_hotness_entry(NULL, hm->new_blkaddr, hm->he, hm->write_count);
	kmem_cache_free(hotness_manage_slab, hm);
}

void update_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	update_hotness_entry(NULL, hm->old_blkaddr, hm->new_blkaddr, hm->he);
	kmem_cache_free(hotness_manage_slab, hm);
}

void reduce_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	reduce_hotness_entry(hm->sbi);
	kmem_cache_free(hotness_manage_slab, hm);
}

int hotness_decide(struct f2fs_io_info *fio, struct hotness_entry **he_pp)
{
	int type;
	enum temp_type temp;
	struct hotness_entry *he = NULL;
	if (fio->old_blkaddr != __UINT32_MAX__) {
		he = lookup_hotness_entry(fio->sbi, fio->old_blkaddr);
	}
	if (he) { // 存在，还要加锁的
		// he->IRR = fio->sbi->total_writed_block_count - he->LWS;
		he->IRR = fio->sbi->total_writed_block_count - (he->LWS >> 1);
		he->LWS = fio->sbi->total_writed_block_count;
	}
	if (he && fio->sbi->centers_valid) {
		type = kmeans_get_type(fio, he);
		if (IS_HOT(type))
			fio->temp = HOT;
		else if (IS_WARM(type))
			fio->temp = WARM;
		else
			fio->temp = COLD;
		temp = fio->temp;
		hc_list_ptr->counts[temp]++;
		hc_list_ptr->IRR_min[temp] = MIN(hc_list_ptr->IRR_min[temp], he->IRR);
		hc_list_ptr->IRR_max[temp] = MAX(hc_list_ptr->IRR_max[temp], he->IRR);
		// he->type = type;
	} else {
		type = CURSEG_WARM_DATA;
		fio->temp = WARM;
		temp = fio->temp;
		hc_list_ptr->counts[temp]++;
	}
	*he_pp = he;
	return type;
}

void hotness_maintain(struct f2fs_io_info *fio, struct hotness_entry *he)
{
	unsigned int segno;
	struct hotness_manage *hm;
	// printk("%s: he = %p, temp = %d\n", __func__, he, fio->temp);
	fio->sbi->total_writed_block_count++;
	segno = GET_SEGNO(fio->sbi, fio->new_blkaddr);
	hm = f2fs_kmem_cache_alloc(hotness_manage_slab, GFP_KERNEL);
	hm->he = he;
	hm->new_blkaddr = fio->new_blkaddr;
	if (he) { // 存在
		// printk("%s: he existed: %p\n", __func__, he);
		he->blk_addr = fio->new_blkaddr;
		hm->old_blkaddr = fio->old_blkaddr;
		INIT_WORK(&hm->work, update_hotness_entry_work);
		queue_work(wq, &hm->work);
		hc_list_ptr->upd_blk_cnt++;
		segment_valid[segno] = 1;
	}
	else {/* 不存在 */
		// printk("%s: he is not existed\n", __func__);
		hm->write_count = fio->sbi->total_writed_block_count;
		INIT_WORK(&hm->work, insert_hotness_entry_work);
		queue_work(wq, &hm->work);
		hc_list_ptr->new_blk_cnt++;
	}

	mutex_lock(&mutex_reduce_he);
	if (hc_list_ptr->count < DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		mutex_unlock(&mutex_reduce_he);
		return;
	}
	reduce_hotness_entry(fio->sbi);
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
	static struct hc_list hc_list_var = {
		.iroot = RADIX_TREE_INIT(hc_list_var.iroot, GFP_NOFS),
		.IRR_min = {__UINT32_MAX__, __UINT32_MAX__, __UINT32_MAX__},
	};
	unsigned int n_clusters;
	unsigned int i;
	unsigned int *centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	unsigned int count;
	const unsigned int onlinecpus = num_possible_cpus();
	max_blkaddr = MAX_BLKADDR(sbi);

	list_lru_init(&hc_list_var.lru);
	hc_list_ptr = &hc_list_var;
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
	struct radix_tree_iter iter;
	void __rcu **slot;

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

	radix_tree_for_each_slot(slot, &hc_list_ptr->iroot, &iter, 0) {
		he = radix_tree_lookup(&hc_list_ptr->iroot, iter.index);

		kernel_write(fp, &he->blk_addr, sizeof(he->blk_addr), &pos);
		kernel_write(fp, &he->IRR, sizeof(he->IRR), &pos);
		kernel_write(fp, &he->LWS, sizeof(he->LWS), &pos);
		// kernel_write(fp, &he->type, sizeof(he->type), &pos);
	}
	
	filp_close(fp, NULL);
out:
	return;
}

/**
 * @brief 释放内存占用
 */
void release_hotness_entry(struct f2fs_sb_info *sbi)
{
	// struct hotness_entry *he, *tmp;
	struct radix_tree_iter iter;
	void __rcu **slot;

	printk("In release_hotness_entry\n");

    flush_workqueue(wq);
    destroy_workqueue(wq);
    printk("Work queue exit: %s\n", __FUNCTION__);

	if (sbi->centers) kfree(sbi->centers);

	printk("hc_list_ptr in 0x%p\n", hc_list_ptr);
	printk("count = %u\n", hc_list_ptr->count);
	if (hc_list_ptr->count == 0) return;

	list_lru_destroy(&hc_list_ptr->lru);

	radix_tree_for_each_slot(slot, &hc_list_ptr->iroot, &iter, 0) {
		radix_tree_delete(&hc_list_ptr->iroot, iter.index);
	}
}

unsigned int get_type_threshold(struct hotness_entry *he)
{
	unsigned int IRR = he->IRR;
	return (IRR < THRESHOLD_HOT_WARM) ? CURSEG_HOT_DATA : ((IRR < THRESHOLD_WARM_COLD) ? CURSEG_WARM_DATA : CURSEG_COLD_DATA);
}

unsigned int get_segment_hotness_avg(struct f2fs_sb_info *sbi, unsigned int segno)
{
	int off;
	block_t blk_addr;
	unsigned int valid = 0;
	block_t start_addr = START_BLOCK(sbi, segno);
	unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	struct hotness_entry *he = NULL;
	u64 IRR_sum = 0;
	for (off = 0; off < usable_blks_in_seg; off++) {
		// if (check_valid_map(sbi, segno, off) == 0) // gc.c
		// 	continue;
		blk_addr = start_addr + off;
		he = lookup_hotness_entry(sbi, blk_addr);
		if (he)	IRR_sum += he->IRR;
		valid++;
	}
	if (valid == 0) return __UINT32_MAX__; // 全部无效的情况怎么办
	else return IRR_sum / valid;
}

bool hc_can_inplace_update(struct f2fs_io_info *fio)
{
	struct hotness_entry *he = NULL;
	unsigned int segno;
	int type_blk, type_seg;
	unsigned int IRR_blk, IRR_seg;
	if (fio->type == DATA && fio->old_blkaddr != __UINT32_MAX__) {
		he = lookup_hotness_entry(fio->sbi, fio->old_blkaddr);
	}
	if (he && fio->sbi->centers_valid) {
		IRR_blk = he->IRR;
		type_blk = kmeans_get_type(fio, he);

		segno = GET_SEGNO(fio->sbi, fio->old_blkaddr);
		IRR_seg = get_segment_hotness_avg(fio->sbi, segno);
		he->IRR = IRR_seg;
		type_seg = kmeans_get_type(fio, he);

		he->IRR = IRR_blk;
		if (type_blk == type_seg)	return true;
		else	return false;
	} else {
		return true;
	}
}
