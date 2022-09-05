#include <linux/fs.h>
#include <linux/module.h>
#include <linux/f2fs_fs.h>
#include <linux/random.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "hc.h"
#include "kmeans.h"

#define diff(a, b) (a) < (b) ? ((b) - (a)) : ((a) - (b))
#define MIN_3(a, b, c) ((a) < (b)) ? (((a) < (c)) ? CURSEG_HOT_DATA : CURSEG_COLD_DATA) : (((c) > (b)) ? CURSEG_WARM_DATA : CURSEG_COLD_DATA)
#define MIN_2(a, b) ((a) < (b)) ? CURSEG_HOT_DATA : CURSEG_WARM_DATA
#define MAX_LOOP_NUM 1000
#define RANDOM_SEED 0  // 0为kmeans++播种，1为随机播种

static void add_to_nearest_set(unsigned int data, long long *mass_center, int center_num);
static void find_initial_cluster(unsigned int *data, int data_num, long long *mass_center, int center_num, int init_random);
static unsigned long long random(void);
static void bubble_sort(unsigned int *x, int num);

int f2fs_hc(struct hc_list *hc_list_ptr, struct f2fs_sb_info *sbi)
{
    /*
    1、对传入的热度数据进行k-means聚类
    2、聚类结果保存在f2fs_sb_info centers
    */
    // printk("Doing f2fs_hc...\n");
    struct hotness_entry *he;
    int center_num = sbi->n_clusters;
    unsigned int *data = kmalloc(sizeof(unsigned int) * hc_list_ptr->count, GFP_KERNEL);
    long long *mass_center = kmalloc(sizeof(long long) * center_num * 3, GFP_KERNEL); //存放质心，平均值，集合元素数
    int data_num = 0;
    int i, flag, loop_count, j;
    if (hc_list_ptr->count > 1000000)
        return -1;

    list_for_each_entry(he, &hc_list_ptr->ilist, list)
    {
        if (he->IRR != UINT_MAX)
            data[data_num++] = he->IRR;
        // printk("IRR = %u", he->IRR);
    }
    if (data_num == 0) return -1;
    // if (sbi->centers_valid) {
    //     for (i = 0; i < center_num; ++i) {
    //         mass_center[i] = (long long)sbi->centers[i];
    //     }
    // } else {
    //     find_initial_cluster(data, data_num, mass_center, center_num, RANDOM_SEED);
    // }
    find_initial_cluster(data, data_num, mass_center, center_num, RANDOM_SEED);
    flag = 1;
    loop_count = 0;
    // printk("IRRs: ");
    // for (i = 0; i < data_num; i++) {
    //     printk("%u ", data[i]);
    // }
    // printk("\n");
    while (flag == 1 && loop_count < MAX_LOOP_NUM)
    {
        flag = 0;
        ++loop_count;
        //每次循环都将上一次的平均值和集合元素数归零
        for (i = 0; i < center_num; ++i)
        {
            mass_center[i * 3 + 1] = 0;
            mass_center[i * 3 + 2] = 0;
        }
        for (j = 0; j < data_num; ++j)
            add_to_nearest_set(data[j], mass_center, center_num);
        for (i = 0; i < center_num; ++i)
        {
            if (mass_center[i * 3 + 2] == 0)
                continue;
            if (mass_center[i * 3] != mass_center[i * 3 + 1] / mass_center[i * 3 + 2])
            {
                flag = 1;
                mass_center[i * 3] = mass_center[i * 3 + 1] / mass_center[i * 3 + 2];
            }
            // if (mass_center[i * 3] != mass_center[i * 3 + 1])
            // {
            //     flag = 1;
            //     mass_center[i * 3] = mass_center[i * 3 + 1];
            // }
        }
    }
    for (i = 0; i < center_num; ++i)
        sbi->centers[i] = (unsigned int)mass_center[i * 3];
    bubble_sort(sbi->centers, center_num);
    // printk("centers: ");
    for (i = 0; i < center_num; i++) {
        // printk("%u ", sbi->centers[i]);
    }
    // printk("\n");
    kfree(data);
    kfree(mass_center);
    return 0;
}

int kmeans_get_type(struct f2fs_io_info *fio)
{
    unsigned int old_IRR, old_LWS;
    unsigned int type;
    int err;
    err = lookup_hotness_entry(fio->sbi, fio->old_blkaddr, &old_IRR, &old_LWS);

    printk("Doing kmeans_get_type...\n");
    
    if (err) {
        printk("fail to lookup hotness_entry\n");
        return err;
    }
    if(fio->sbi->n_clusters == 3) {
        type = MIN_3(diff(old_IRR, fio->sbi->centers[0]),
                     diff(old_IRR, fio->sbi->centers[1]),
                     diff(old_IRR, fio->sbi->centers[2]));
    } else {
        type = MIN_2(diff(old_IRR, fio->sbi->centers[0]),
                     diff(old_IRR, fio->sbi->centers[1]));
    }
    
    return type;
}

static void find_initial_cluster(unsigned int *data, int data_num, long long *mass_center, int center_num, int init_random)
{
    int i, j, k;
    unsigned int *distance;
    unsigned long long total_distance;
    unsigned long long threshold;
    unsigned long long distance_sum;
    //随机播种
    if (init_random == 1)
    {
random_seed:
        for (i = 0; i < center_num; ++i)
            mass_center[i * 3] = data[(int)(random() % data_num)];
        return;
    }
    // kmeans++播种
    mass_center[0] = data[(int)(random() % data_num)];
    distance = kmalloc(sizeof(unsigned int) * data_num, GFP_KERNEL);
    for (k = 1; k < center_num; ++k)
    {
        total_distance = 0;
        //求每一个元素到当前所有质心的距离
        for (j = 0; j < data_num; ++j)
        {
            distance[j] = 0;
            for (i = 0; i < k; i++)
                distance[j] += diff(mass_center[i * 3], data[j]);
            total_distance += distance[j];
        }
        //距离当前质心越远的元素更有可能被选为质心
        if (total_distance == 0) goto random_seed;
        threshold = random() % total_distance;
        distance_sum = 0;
        for (j = 0; j < data_num; ++j)
        {
            distance_sum += distance[j];
            if (distance_sum >= threshold)
                break;
        }
        //产生了新的质心
        mass_center[k * 3] = data[j];
    }
}

static unsigned long long random(void)
{
    unsigned long long x;
    get_random_bytes(&x, sizeof(x));
    return x;
}

static void add_to_nearest_set(unsigned int data, long long *mass_center, int center_num)
{
    /*
     * 将输入的参数点寻找最近的质心，并加入质心的函数中
     */
    unsigned int min = diff(mass_center[0], data);
    int position = 0, i;
    for (i = 1; i < center_num; i++)
    {
        unsigned int temp = diff(mass_center[i * 3], data);
        if (temp < min)
        {
            min = temp;
            position = i;
        }
    }
    mass_center[position * 3 + 1] += data;
    ++mass_center[position * 3 + 2];
    // mass_center[position * 3 + 1] = mass_center[position * 3 + 1] + (data - mass_center[position * 3 + 1]) / (++mass_center[position * 3 + 2]);
}

static void bubble_sort(unsigned int *x, int num)
{
    int temp, i, j;
    for (i = 0; i < num - 1; ++i)
        for (j = 0; j < num - 1 - i; ++j)
            if (x[j] > x[j + 1])
            {
                temp = x[j + 1];
                x[j + 1] = x[j];
                x[j] = temp;
            }
    return;
}