#ifndef IOMAP_H
#define IOMAP_H

#define RESCTRL_PATH "/sys/fs/resctrl"
#define CACHE_WAYS 16           // Zen 2 L3 typical (CBM = fffff, L3/cbm_mask)
#define IO_SPACE 0xff000000     // this took way too long to find...
#define IO_SIZE (256 * 1024UL)
#define DEVICE_NAME "cachemem"
#define CLASS_NAME "cacheclass"
#define CACHE_SIZE IO_SIZE

#include <asm/cacheflush.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/spinlock.h>

struct cat_config {
  unsigned int closid;
  unsigned long cbm;
  int way;
};

extern void *io_space_virt;
extern int cache_way_locks[CACHE_WAYS];
extern spinlock_t cache_lock_spinlock;
extern struct cat_config cat_configs[CACHE_WAYS];

int init_io(void);
void cleanup_io(void);
// void switch_uc(void);
int lock_cache_way(int way, const void *data, size_t size);
// void flood_l3(void);
int init_cache_monitor(void);
void check_fill(void);
void check_evictions(void);
void cleanup_cache_monitor(void);
// void set_mtrr_wc(unsigned long base, unsigned long size, int index);

#endif
