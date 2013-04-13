/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify the
 * minimum oom_adj values in /sys/module/lowmemorykiller/parameters/adj and the
 * number of free pages in /sys/module/lowmemorykiller/parameters/minfree. Both
 * files take a comma separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill processes
 * with a oom_adj value of 8 or higher when the free memory drops below 4096 pages
 * and kill processes with a oom_adj value of 0 or higher when the free memory
 * drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/swap.h>

#define SEC_ADJUST_LMK

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>

static int auto_detect = 0; //Autodetect disabled by Default.

static int init_kobject(void);
#endif


static uint32_t lowmem_debug_level = 1;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;

static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int lowmem_shrink(int nr_to_scan, gfp_t gfp_mask)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int min_adj = OOM_ADJUST_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;
#ifdef SEC_ADJUST_LMK
	int other_file = global_page_state(NR_INACTIVE_FILE) +
						global_page_state(NR_ACTIVE_FILE);
#else
	int other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);
#endif

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
#ifdef SEC_ADJUST_LMK
		if ((other_free + other_file) < lowmem_minfree[i])
#else
		if (other_free < lowmem_minfree[i] &&
            other_file < lowmem_minfree[i])
#endif
        {
			min_adj = lowmem_adj[i];
			break;
		}
	}
#ifdef SEC_ADJUST_LMK
	if (min_adj == OOM_ADJUST_MAX + 1)
		return 0;
#endif
	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %d, %x, ofree %d %d, ma %d\n",
			     nr_to_scan, gfp_mask, other_free, other_file,
			     min_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
#ifdef SEC_ADJUST_LMK
	if (nr_to_scan <= 0)
#else
	if (nr_to_scan <= 0 || min_adj == OOM_ADJUST_MAX + 1)
#endif
	{
		lowmem_print(5, "lowmem_shrink %d, %x, return %d\n",
			     nr_to_scan, gfp_mask, rem);
		return rem;
	}
	selected_oom_adj = min_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		int oom_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		if (test_tsk_thread_flag(p, TIF_MEMDIE) &&
			time_before_eq(jiffies, lowmem_deathpending_timeout)) {
				task_unlock(p);
				rcu_read_unlock();
				return 0;
		}

		oom_adj = p->signal->oom_adj;
		if (oom_adj < min_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				continue;
			if (oom_adj == selected_oom_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_adj, tasksize);
	}
	if (selected) {
		
		if (fatal_signal_pending(selected)) {
			pr_warning("process %d is suffering a slow death\n",
				selected->pid);
			read_unlock(&tasklist_lock);
			return rem;
		}

		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
			     selected->pid, selected->comm,
			     selected_oom_adj, selected_tasksize);
		lowmem_deathpending_timeout = jiffies + HZ;
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
	}
#ifdef SEC_ADJUST_LMK
	else
		rem = -1;
#endif

	lowmem_print(4, "lowmem_shrink %d, %x, return %d\n",
		     nr_to_scan, gfp_mask, rem);
	rcu_read_unlock();
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES 
	init_kobject();
	#endif	
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES                                                                            
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)                                                                                  
{                                                                                                                                            
       if (oom_adj == OOM_ADJUST_MAX)                                                                                                        
               return OOM_SCORE_ADJ_MAX;                                                                                                     
       else                                                                                                                                  
               return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;                                                                          
}                                                                                                                                            
                                                                                                                                             
static void lowmem_autodetect_oom_adj_values(void)                                                                                           
{                                                                                                                                            
       int i;                                                                                                                                
       short oom_adj;                                                                                                                        
       short oom_score_adj;                                                                                                                  
       int array_size = ARRAY_SIZE(lowmem_adj);                                                                                              
       if (lowmem_adj_size < array_size)                                                                                                     
               array_size = lowmem_adj_size;                                                                                                 
                                                                                                                                             
       if (array_size <= 0)                                                                                                                  
               return;                                                                                                                       
                                                                                                                                             
       oom_adj = lowmem_adj[array_size - 1];                                                                                                 
       if (oom_adj > OOM_ADJUST_MAX)                                                                                                         
               return;                                                                                                                       
                                                                                                                                             
       oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);                                                                             
       if (oom_score_adj <= OOM_ADJUST_MAX)                                                                                                  
               return;                                                                                                                       
                                                                                                                                             
       lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");                                                                
       for (i = 0; i < array_size; i++) {                                                                                                    
               oom_adj = lowmem_adj[i];                                                                                                      
               oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);                                                                     
               lowmem_adj[i] = oom_score_adj;                                                                                                
               lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",                                                                           
                            oom_adj, oom_score_adj);                                                                                         
       }                                                                                                                                     
}                                                                                                                                            
                                                                                                                                             
static int lowmem_adj_array_set(const char *val, struct kernel_param *kp)                                                              
{                                                                                                                                            
       int ret; 
       ret = param_array_set(val, kp);                                                                                                                                      
       /* HACK: Autodetect oom_adj values in lowmem_adj array */
       /* But only if not disabled via sysfs interface /sys/kernel/lowmemorykiller/auto_detect */
       /* echo 0 > /sys/kernel/lowmemorykiller/auto_detect to disable auto_detect at runtime */
       /* Default is enabled (1) when config option is enabled too */

       if (auto_detect)
       		lowmem_autodetect_oom_adj_values(); 
       return ret;
}                                                                                                                           
static int lowmem_adj_array_get(char *buffer, struct kernel_param *kp)        
{
       return param_array_get(buffer, kp);                                                                                               
}
                                                                                                                                             
static void lowmem_adj_array_free(void *arg)                                                                                                 
{       
	unsigned int i;
	const struct kparam_array *arr = arg;
 
	for (i = 0; i < (arr->num ? *arr->num : arr->max); i++)
		kfree(arr->elem + arr->elemsize * i);
}
                                                                                                                                             
                                                                                                                                             
static const struct kparam_array __param_arr_adj = {
       .max = ARRAY_SIZE(lowmem_adj),
       .num = &lowmem_adj_size,
       .set = param_set_int,
       .get = param_get_int,
       .elemsize = sizeof(lowmem_adj[0]),                                                                                                    
       .elem = lowmem_adj,                                                                                                                   
};                                                                                                                                           

//For the auto_detect on/off sysfs attribute in /sys/kernel/lowmemory killer - Inspired by an0nym0us' posts on Mesa Kernel

static ssize_t ad_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", auto_detect);
}

static ssize_t ad_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	sscanf(buf, "%du", &auto_detect);
	if (auto_detect)
		printk("LMK - Auto Detect is On\n");
	else
		printk("LMK - Auto Detect is Off\n");

	return count;
}


static int init_kobject(void)
{

	int retval;
	static struct kobj_attribute ad_attribute = __ATTR(auto_detect, 0666, ad_show, ad_store); 
	static struct attribute *attrs[] = { &ad_attribute.attr, NULL, };                                                                                                                                                    
	static struct attribute_group attr_group = {
        	.attrs = attrs,                                                                                                                       
	};                                                                              
                                                                             
	static struct kobject *ad_kobj;                                                                                                      

	ad_kobj = kobject_create_and_add("lowmemorykiller", kernel_kobj);
	if (!ad_kobj) 
		return -ENOMEM;

	retval = sysfs_create_group(ad_kobj, &attr_group);
	if (retval)
		kobject_put(ad_kobj);
	return retval;
}

#endif                                                                                                                                       


module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
__module_param_call(MODULE_PARAM_PREFIX, adj,
	lowmem_adj_array_set, lowmem_adj_array_get,
	.arr = &__param_arr_adj,
	S_IRUGO | S_IWUSR, 1);
	__MODULE_PARM_TYPE(adj, "array of int");
#else
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
	S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

