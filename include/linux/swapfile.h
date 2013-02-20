#ifndef _LINUX_SWAPFILE_H
#define _LINUX_SWAPFILE_H
#include <linux/swap.h>

/*
 * these were static in swapfile.c but frontswap.c needs them and we don't
 * want to expose them to the dozens of source files that include swap.h
 */
extern spinlock_t swap_lock;
extern struct swap_list_t swap_list;
//extern struct swap_info_struct *swap_info[MAX_SWAPFILES];
extern int try_to_unuse(unsigned int, bool, unsigned long);

#endif /* _LINUX_SWAPFILE_H */
