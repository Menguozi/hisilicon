#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
// #include <include/linux/xarray.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"
// #include <trace/events/f2fs.h>

static DEFINE_SPINLOCK(list_lock);
static struct kmem_cache *hotness_entry_slab;
struct kmem_cache *hotness_entry_info_slab;
struct hc_list *hc_list_ptr;
struct workqueue_struct *wq;
nid_t last_ino;
char segment_valid[MAX_SEGNO];

/* 热度元数据操作 */
/* 1、添加 */
// create
int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he)
{
	
	he = f2fs_kmem_cache_alloc(hotness_entry_slab, GFP_NOFS);
	he->blk_addr = blkaddr,
	he->IRR = __UINT32_MAX__,
	he->LWS = sbi->total_writed_block_count;
	f2fs_radix_tree_insert(&hc_list_ptr->iroot, blkaddr, he);
	spin_lock(&list_lock);
	list_add_tail_rcu(&he->list, &hc_list_ptr->ilist);
	spin_unlock(&list_lock);
	return 0;
}

// int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, struct hotness_entry *he)
// {
// 	radix_tree_delete(&hc_list_ptr->iroot, blkaddr);
// 	f2fs_radix_tree_insert(&hc_list_ptr->iroot, blkaddr, he);
// 	spin_lock(&list_lock);
// 	list_add_tail_rcu(&he->list, &hc_list_ptr->ilist);
// 	spin_unlock(&list_lock);
// }

/* 2、查询 */
struct hotness_entry *lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct hotness_entry *he;
	if (hc_list_ptr->iroot.xa_head == NULL) goto no_xa_head;
	he = radix_tree_lookup(&hc_list_ptr->iroot, blkaddr);
	if (he) {
        return he;
    } else {
no_xa_head:
        return NULL;
    }
}

/* 3、移除 */
int delete_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct hotness_entry *he;
	he = radix_tree_lookup(&hc_list_ptr->iroot, blkaddr);
	if (he) {
		hc_list_ptr->count--;
		radix_tree_delete(&hc_list_ptr->iroot, he->blk_addr);
		spin_lock(&list_lock);
		list_del_rcu(&he->list);
		spin_unlock(&list_lock);
		kmem_cache_free(hotness_entry_slab, he);
		return 0;
	}
	return -1;
}

int f2fs_create_hotness_clustering_cache(void)
{
	hotness_entry_slab = f2fs_kmem_cache_create("f2fs_hotness_entry", sizeof(struct hotness_entry));
	if (!hotness_entry_slab)
		return -ENOMEM;
	hotness_entry_info_slab = f2fs_kmem_cache_create("f2fs_hotness_entry_info", sizeof(struct hotness_entry_info));
	if (!hotness_entry_info_slab)
		return -ENOMEM;
	return 0;
}

void f2fs_destroy_hotness_clustering_cache(void)
{
	kmem_cache_destroy(hotness_entry_slab);
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
	};
	unsigned int n_clusters;
	unsigned int i;
	unsigned int *centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);
	unsigned int count;

	hc_list_ptr = &hc_list_var;

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
		insert_hotness_entry(sbi, he->blk_addr, he);
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

	fp = filp_open("/tmp/f2fs_hotness", O_RDWR|O_CREAT, 0644);
	if (IS_ERR(fp)) goto out;

	// save n_clusters
	kernel_write(fp, &sbi->n_clusters, sizeof(sbi->n_clusters), &pos);
	printk("pos = 0x%llx\n", pos);
	// save centers
	for(i = 0; i < sbi->n_clusters; i++) {
		kernel_write(fp, &sbi->centers[i], sizeof(sbi->centers[i]), &pos);
		printk("%u, 0x%x\n", sbi->centers[i], sbi->centers[i]);
	}
	printk("pos = 0x%llx\n", pos);
	// save total_writed_block_count
	kernel_write(fp, &sbi->total_writed_block_count, sizeof(sbi->total_writed_block_count), &pos);
	printk("%u, 0x%x\n", sbi->total_writed_block_count, sbi->total_writed_block_count);
	printk("pos = 0x%llx\n", pos);
	
	rcu_read_lock();
	list_for_each_entry_rcu(he, &hc_list_ptr->ilist, list){
		kernel_write(fp, &he->blk_addr, sizeof(he->blk_addr), &pos);
		kernel_write(fp, &he->IRR, sizeof(he->IRR), &pos);
		kernel_write(fp, &he->LWS, sizeof(he->LWS), &pos);
	}
	rcu_read_unlock();
	printk("pos = 0x%llx\n", pos);
	
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

	printk("In release_hotness_entry");

	// rcu_read_lock();
	list_for_each_entry_safe(he, tmp, &hc_list_ptr->ilist, list) {
		list_del(&he->list);
		kmem_cache_free(hotness_entry_slab, he);
		hc_list_ptr->count--;
	}

	radix_tree_for_each_slot(slot, &hc_list_ptr->iroot, &iter, 0) {
		radix_tree_delete(&hc_list_ptr->iroot, iter.index);
	}

	kfree(sbi->centers);
}