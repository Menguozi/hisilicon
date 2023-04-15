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

static DEFINE_MUTEX(mutex_reduce_he);
struct kmem_cache *hotness_manage_slab;
struct hotness_info *hotness_info_ptr;
struct workqueue_struct *wq;

int insert_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, __u64 value, int type)
{
	// printk("%s: blkaddr = %u, value = %llu, type = %d\n", __func__, blkaddr, value, type);
	// printk("hotness_info_ptr->hotness_rt_array[type] in %p\n", &hotness_info_ptr->hotness_rt_array[type]);
	radix_tree_insert(&hotness_info_ptr->hotness_rt_array[type], blkaddr, (void *) value);
	hotness_info_ptr->count++;
	hotness_info_ptr->new_blk_cnt++;
	return 0;
}

int update_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr_old, block_t blkaddr_new, __u64 value, int type_old, int type_new)
{
	// printk("%s: blkaddr_old = %u, blkaddr_new = %u, type_old = %d, type_new = %d, value = 0x%llx\n", __func__, blkaddr_old, blkaddr_new, type_old, type_new, value);
	radix_tree_delete(&hotness_info_ptr->hotness_rt_array[type_old], blkaddr_old);
	radix_tree_insert(&hotness_info_ptr->hotness_rt_array[type_new], blkaddr_new, (void *) value);

	hotness_info_ptr->upd_blk_cnt++;
	if (blkaddr_old != blkaddr_new) {
		hotness_info_ptr->opu_blk_cnt++;
	} else {
		hotness_info_ptr->ipu_blk_cnt++;
	}
	return 0;
}

__u64 lookup_hotness_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int* type)
{
	// printk("In %s, type = %d\n", __func__, type);
	void *value;
	value = radix_tree_lookup(&hotness_info_ptr->hotness_rt_array[0], blkaddr);
	if (value) {
		*type = CURSEG_HOT_DATA;
		goto found;
	}
	value = radix_tree_lookup(&hotness_info_ptr->hotness_rt_array[1], blkaddr);
	if (value) {
		*type = CURSEG_WARM_DATA;
		goto found;
	}
	value = radix_tree_lookup(&hotness_info_ptr->hotness_rt_array[2], blkaddr);
	if (value) {
		*type = CURSEG_COLD_DATA;
		goto found;
	}
	// not_found
	*type = -1;
	return 0;
found:
	return (__u64) value;
}

void reduce_hotness_entry(struct f2fs_sb_info *sbi) {
	// printk("In %s", __func__);
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned int count = 0;
	radix_tree_for_each_slot(slot, &hotness_info_ptr->hotness_rt_array[0], &iter, 0) {
		if (count >= DEF_HC_HOTNESS_ENTRY_SHRINK_NUM) 
			break;
		radix_tree_delete(&hotness_info_ptr->hotness_rt_array[0], iter.index);
		hotness_info_ptr->count--;
		count++;
	}
	hotness_info_ptr->rmv_blk_cnt += count;
	mutex_unlock(&mutex_reduce_he);
}

void insert_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	insert_hotness_entry(hm->sbi, hm->blkaddr_new, hm->value, hm->type_new);
	kmem_cache_free(hotness_manage_slab, hm);
}

void update_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	update_hotness_entry(hm->sbi, hm->blkaddr_old, hm->blkaddr_new, hm->value, hm->type_old, hm->type_new);
	kmem_cache_free(hotness_manage_slab, hm);
}

void reduce_hotness_entry_work(struct work_struct *work)
{
	struct hotness_manage *hm = container_of(work, struct hotness_manage, work);
	reduce_hotness_entry(hm->sbi);
	kmem_cache_free(hotness_manage_slab, hm);
}

int hotness_decide(struct f2fs_io_info *fio, int *type_old_ptr, __u64 *value_ptr)
{
	// printk("%s: old_blkaddr = %u, new_blkaddr = %u\n", __func__, fio->old_blkaddr, fio->new_blkaddr);
	__u64 value, LWS;
	__u32 IRR, IRR1;
	int type_new, type_old;
	enum temp_type temp;
	__u64 LWS_old = 0;
	type_old = -1;
	LWS = fio->sbi->total_writed_block_count;
	if (fio->old_blkaddr != __UINT32_MAX__) {
		value = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, &type_old);
	}
	if (type_old == -1) { // 不存在
		IRR = __UINT32_MAX__ >> 2;
		IRR1 = IRR << 2;
		value = (LWS << 32) + IRR1;
		type_new = CURSEG_COLD_DATA;
		fio->temp = COLD;
		temp = fio->temp;
		hotness_info_ptr->counts[temp]++;
	} else {
		LWS_old = value >> 32;
		IRR = LWS - LWS_old;
		IRR1 = IRR << 2;
		value = (LWS << 32) + IRR1;
		if (fio->sbi->centers_valid) {
			type_new = kmeans_get_type(fio, IRR);
		} else {
			type_new = type_old;
		}
		if (IS_HOT(type_new))
			fio->temp = HOT;
		else if (IS_WARM(type_new))
			fio->temp = WARM;
		else
			fio->temp = COLD;
		temp = fio->temp;
		hotness_info_ptr->counts[temp]++;
		hotness_info_ptr->IRR_min[temp] = MIN(hotness_info_ptr->IRR_min[temp], IRR);
		hotness_info_ptr->IRR_max[temp] = MAX(hotness_info_ptr->IRR_max[temp], IRR);
	}
	// printk("%s: LWS = %llu[0x%llx], IRR = %u[0x%x], value = 0x%llx, LWS_old = %llu\n", __func__, LWS, LWS, IRR, IRR, value, LWS_old);
	fio->sbi->total_writed_block_count++;
	*type_old_ptr = type_old;
	*value_ptr = value;
	return type_new;
}

void hotness_maintain(struct f2fs_io_info *fio, int type_old, int type_new, __u64 value)
{
	// printk("%s: type_old = %d, type_new = %d, value = %llu\n", __func__, type_old, type_new, value);
	// struct hotness_manage *hm;
	// hm = f2fs_kmem_cache_alloc(hotness_manage_slab, GFP_KERNEL);
	// hm->sbi = fio->sbi;
	// hm->blkaddr_new = fio->new_blkaddr;
	// hm->type_new = type_new;
	// hm->value = value;
	if (type_old == -1) { /* 不存在 */
		// printk("%s: new\n", __func__);
		// INIT_WORK(&hm->work, insert_hotness_entry_work);
		// queue_work(wq, &hm->work);

		insert_hotness_entry(fio->sbi, fio->new_blkaddr, value, type_new);
	} else { // 存在
		// printk("%s: upd\n", __func__);
		// hm->blkaddr_old = fio->old_blkaddr;
		// hm->type_old = type_old;
		// INIT_WORK(&hm->work, update_hotness_entry_work);
		// queue_work(wq, &hm->work);

		update_hotness_entry(fio->sbi, fio->old_blkaddr, fio->new_blkaddr, value, type_old, type_new);
	}

	mutex_lock(&mutex_reduce_he);
	if (hotness_info_ptr->count < DEF_HC_HOTNESS_ENTRY_SHRINK_THRESHOLD) {
		mutex_unlock(&mutex_reduce_he);
		return;
	}
	reduce_hotness_entry(fio->sbi);
}

int f2fs_create_hotness_clustering_cache(void)
{
	hotness_manage_slab = f2fs_kmem_cache_create("f2fs_hotness_manage", sizeof(struct hotness_manage));
	if (!hotness_manage_slab)
		return -ENOMEM;
	return 0;
}

void f2fs_destroy_hotness_clustering_cache(void)
{
	printk("In function %s\n", __func__);
	kmem_cache_destroy(hotness_manage_slab);
}

static void init_hc_management(struct f2fs_sb_info *sbi)
{
	struct file *fp;
	loff_t pos = 0;
	unsigned int n_clusters;
	unsigned int i;
	unsigned int count;
	unsigned int *centers;
	__u32 IRR;
	__u64 LWS, value;
	block_t blk_addr;
	unsigned char type;
	static struct hotness_info hotness_info_var = {
		.hotness_rt_array[0] = RADIX_TREE_INIT(hotness_info_var.hotness_rt_array[0], GFP_NOFS),
		.hotness_rt_array[1] = RADIX_TREE_INIT(hotness_info_var.hotness_rt_array[1], GFP_NOFS),
		.hotness_rt_array[2] = RADIX_TREE_INIT(hotness_info_var.hotness_rt_array[2], GFP_NOFS),
		.IRR_min = {__UINT32_MAX__ >> 2, __UINT32_MAX__ >> 2, __UINT32_MAX__ >> 2},
	};
	// INIT_RADIX_TREE(&hotness_info_ptr->hotness_rt_array[0], GFP_NOFS);
	// INIT_RADIX_TREE(&hotness_info_ptr->hotness_rt_array[1], GFP_NOFS);
	// INIT_RADIX_TREE(&hotness_info_ptr->hotness_rt_array[2], GFP_NOFS);
	hotness_info_ptr = &hotness_info_var;
	printk("In %s, hotness_info_ptr->hotness_rt_array[0] in %p\n", __func__, &hotness_info_ptr->hotness_rt_array[0]);
	printk("In %s, hotness_info_ptr->hotness_rt_array[1] in %p\n", __func__, &hotness_info_ptr->hotness_rt_array[1]);
	printk("In %s, hotness_info_ptr->hotness_rt_array[2] in %p\n", __func__, &hotness_info_ptr->hotness_rt_array[2]);
	centers = kmalloc(sizeof(unsigned int) * sbi->n_clusters, GFP_KERNEL);

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

	// read blk_addr & IRR & LWS for each block to init 
	for(i = 0; i < count; i++) {
		kernel_read(fp, &blk_addr, sizeof(blk_addr), &pos);
		kernel_read(fp, &LWS, sizeof(LWS), &pos);
		kernel_read(fp, &IRR, sizeof(IRR), &pos);
		kernel_read(fp, &type, sizeof(type), &pos);
		value = (LWS << 32) + IRR;
		radix_tree_insert(&hotness_info_ptr->hotness_rt_array[type], blk_addr, (void *) value);
		// printk("%u, %u, %u\n", he->blk_addr, he->IRR, he->LWS);
	}

	filp_close(fp, NULL);
out:
	return;
}

void f2fs_build_hc_manager(struct f2fs_sb_info *sbi)
{
	printk("In f2fs_build_hc_manager\n");
	init_hc_management(sbi);
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
		err = f2fs_hc(hotness_info_ptr, sbi);
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
	unsigned int i;
	struct radix_tree_iter iter;
	void __rcu **slot;
	__u64 value, LWS;
	__u32 IRR;
	block_t blk_addr;
	int type;

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

	for (type = 0; type < 3; type++) {
		radix_tree_for_each_slot(slot, &hotness_info_ptr->hotness_rt_array[type], &iter, 0) {
			blk_addr = iter.index;
			value = (__u64) radix_tree_lookup(&hotness_info_ptr->hotness_rt_array[type], blk_addr);
    		IRR = value & 0xffffffff;
			LWS = value >> 32;
			kernel_write(fp, &blk_addr, sizeof(blk_addr), &pos);
			kernel_write(fp, &LWS, sizeof(LWS), &pos);
			kernel_write(fp, &IRR, sizeof(IRR), &pos);
			kernel_write(fp, &type, sizeof(type), &pos);
		}
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
	int type;

	printk("In release_hotness_entry\n");

    flush_workqueue(wq);
    destroy_workqueue(wq);
    printk("Work queue exit: %s\n", __FUNCTION__);

	if (sbi->centers) kfree(sbi->centers);

	printk("count = %u\n", hotness_info_ptr->count);
	if (hotness_info_ptr->count == 0) return;

	for (type = 0; type < 3; type++) {
		radix_tree_for_each_slot(slot, &hotness_info_ptr->hotness_rt_array[type], &iter, 0) {
			radix_tree_delete(&hotness_info_ptr->hotness_rt_array[type], iter.index);
		}
	}
}

unsigned int get_segment_hotness_avg(struct f2fs_sb_info *sbi, unsigned int segno)
{
	int off;
	block_t blk_addr;
	__u64 value;
	__u32 IRR;
	int type;
	unsigned int valid = 0;
	block_t start_addr = START_BLOCK(sbi, segno);
	// unsigned int usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
	unsigned int usable_blks_in_seg = sbi->blocks_per_seg;
	__u64 IRR_sum = 0;
	for (off = 0; off < usable_blks_in_seg; off++) {
		// if (check_valid_map(sbi, segno, off) == 0) // gc.c
		// 	continue;
		blk_addr = start_addr + off;
		value = lookup_hotness_entry(sbi, blk_addr, &type);
		if (type != -1)	{
    		IRR = (value & 0xffffffff) >> 2;
			IRR_sum += IRR;
			valid++;
		}
	}
	if (valid == 0) return __UINT32_MAX__ >> 2; // 全部无效的情况怎么办
	else return IRR_sum / valid;
}

bool hc_can_inplace_update(struct f2fs_io_info *fio)
{
	unsigned int segno;
	int type_blk, type_seg;
	unsigned int IRR_blk, IRR_seg;
	__u64 value;
	if (fio->type == DATA && fio->old_blkaddr != __UINT32_MAX__) {
		value = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, &type_blk);
	}
	if (type_blk != -1 && fio->sbi->centers_valid) {
		IRR_blk = (value & 0xffffffff) >> 2;
		// type_blk = kmeans_get_type(fio, IRR_blk);

		segno = GET_SEGNO(fio->sbi, fio->old_blkaddr);
		IRR_seg = get_segment_hotness_avg(fio->sbi, segno);
		type_seg = kmeans_get_type(fio, IRR_seg);

		if (type_blk == type_seg)	return true;
		else	return false;
	} else {
		return true;
	}
}
