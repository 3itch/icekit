// #include "include/msr.h"
#include "include/iomap.h"
#include <asm/processor.h>
#include <asm/tlbflush.h>
#include <linux/cpu.h>
#include <linux/ctype.h>
#include <linux/fs.h>
#include <linux/hw_breakpoint.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/perf_event.h>
/*
  io_space_virt     : virtual address space of our carved out I/O space mapped as WC
  cache_way_locks   : array tracking locked cache ways
  DEFINE_SPINLOCK   : spinlock for cache ops
  cat_config        : CAT way configuration
  perf_event        : check for l3 cache misses
*/
void *io_space_virt;
int cache_way_locks[CACHE_WAYS] = {0};
DEFINE_SPINLOCK(cache_lock_spinlock);
struct cat_config cat_configs[CACHE_WAYS] = {0};
static struct perf_event *cache_miss_event;
// static int is_uc = 0;

// all intel BS
// struct perf_event_attr attr = {
//     .type = PERF_TYPE_RAW,
//     .config = 0x24,      // SDM
//     .size = sizeof(struct perf_event_attr),
//     .disabled = 1,
//     .exclude_kernel = 1,
//     .exclude_hv = 1,
// };

// TODO: Fix docs and find a way to read from L3 cache properly.
// I don't want to rely on prime+probe
/*
 * initialise l3 cache miss checking using AMD PMU
 * @return : 0 on success or whatever fucking error our ptr returns
 */
int init_cache_monitor(void) {
  struct perf_event_attr attr = {
      // .type = PERF_TYPE_HW_CACHE,
      // .config = (PERF_COUNT_HW_CACHE_LL) | (PERF_COUNT_HW_CACHE_OP_READ << 8)
      // |
      //           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
      .type = PERF_TYPE_RAW,
      .config = 0x0104E,        // AMD zen 2 l3 cache miss event ( see spec )
      .size = sizeof(struct perf_event_attr),
      .disabled = 1,
      .exclude_kernel = 1,      // kernelspace events
      .exclude_hv = 1,          // hypervisor events
  };

  cache_miss_event =
      perf_event_create_kernel_counter(&attr, -1, current, NULL, NULL);
  if (IS_ERR(cache_miss_event)) {
    printk(KERN_ERR "[ICEPICK] Failed to create L3 perf event: %ld\n",
           PTR_ERR(cache_miss_event));
    return PTR_ERR(cache_miss_event);
  }

  perf_event_enable(cache_miss_event);

  printk(KERN_INFO "[ICEPICK] L3 cache miss monitoring enabled\n");
  return 0;
}
/*  check l3 cache evictions by reading from PMU performance counter  */
void check_evictions(void) {
  u64 miss_count, enabled, running;
  static u64 last_miss_count = 0;

  miss_count = perf_event_read_value(cache_miss_event, &enabled, &running);
  if (miss_count > last_miss_count) {
    printk(KERN_WARNING "[ICEPICK] Detected %llu L3 cache misses\n",
           miss_count - last_miss_count);
  }

  last_miss_count = miss_count;
}
/*  rid of l3 cache miss monitor  */
void cleanup_cache_monitor(void) {
  if (cache_miss_event) {
    perf_event_disable(cache_miss_event);
    perf_event_release_kernel(cache_miss_event);
    cache_miss_event = NULL;
  }
}
/*
 * check physical memory range availability in `/proc/iomem`
 * @start  : physical address
 * @size   : size of range
 * @return : 0 if free or -EBUSY on overlap
 */
static int check_iomem_range(unsigned long start, unsigned long size) {
  struct file *file;
  char buf[256];
  char *line = buf;
  loff_t pos = 0;
  ssize_t len;
  unsigned long end = start + size - 1;

  file = filp_open("/proc/iomem", O_RDONLY, 0);
  if (IS_ERR(file)) {
    printk(KERN_ERR "[ICEPICK]      failed to open '/proc/iomem': %ld\n",
           PTR_ERR(file));
    return PTR_ERR(file);
  }

  //    this section i didn't do myself,  too much parsing trial & error
  printk(KERN_INFO "[ICEPICK] target: 0x%08lx-0x%08lx\n", start, end);
  while ((len = kernel_read(file, buf, sizeof(buf) - 1, &pos)) > 0) {
    buf[len] = '\0';

    while (line && *line) {
      unsigned long r_start, r_end;
      char *dash = strchr(line, '-');
      char *colon = strchr(line, ':');
      char *endptr;

      while (*line == ' ' || *line == '\t')
        line++;
      if (!dash || (colon && colon < dash) || !isxdigit(*line)) {
        line = strchr(line, '\n');
        if (line)
          line++;
        continue;
      }

      *dash = '\0';
      r_start = simple_strtoul(line, &endptr, 16);
      if (endptr != dash) {
        printk(KERN_DEBUG "[ICEPICK]      bad start: %s\n", line);
        goto next_line;
      }

      line = dash + 1;
      if (colon)
        *colon = '\0';
      r_end = simple_strtoul(line, &endptr, 16);

      if (*endptr && *endptr != ' ' && *endptr != '\n') {
        printk(KERN_DEBUG "[ICEPICK]      bad end: %s\n", line);
        goto next_line;
      }

      printk(KERN_INFO "[ICEPICK] parsed: 0x%08lx-0x%08lx\n", r_start, r_end);

      if (start <= r_end && end >= r_start) {
        printk(KERN_ERR "[ICEPICK]      overlap with 0x%08lx-0x%08lx\n", r_start,
               r_end);
        filp_close(file, NULL);
        return -EBUSY;
      }

    next_line:
      line = strchr(line, '\n');
      if (line)
        line++;
      else
        break;
    }
  }

  filp_close(file, NULL);
  printk(KERN_INFO "[ICEPICK] range 0x%08lx-0x%08lx is free\n", start, end);
  return 0;
}
/*
 * initialise I/O space with WC
 * @return : 0 on success, -EBUSY if I/O space is reserved, -ENOMEM if map was unsuccessfull
 */
int init_io(void) {
  int ret = check_iomem_range(IO_SPACE, IO_SIZE);
  unsigned long base = IO_SPACE & ~0xFFFUL;
  unsigned long size = roundup_pow_of_two(IO_SIZE);
  u32 mtrr_cap, mtrr_def;
  int index = 0;

  if (ret < 0)
    return ret;

  if (!request_mem_region(IO_SPACE, IO_SIZE, "icepick")) {
    printk(KERN_ERR "[ICEPICK]      failed to reserve I/O space 0x%lx-0x%lx\n",
           (unsigned long)IO_SPACE, (unsigned long)IO_SPACE + IO_SIZE - 1);
    return -EBUSY;
  }

  io_space_virt = ioremap_wc(IO_SPACE, IO_SIZE);
  if (!io_space_virt) {
    release_mem_region(IO_SPACE, IO_SIZE);
    printk(KERN_ERR "[ICEPICK]      failed to map WC\n");
    return -ENOMEM;
  }

  rdmsr_on_cpu(0, MSR_MTRRcap, &mtrr_cap, &mtrr_def);
  if (index < (mtrr_cap & 0xFF)) {
    u64 phys_base = base | 0x6;                           // write-combine type ( AMD uses `0x6` )
    u64 phys_mask = ~(size - 1) | (1ULL << 11);           // enable bit
    local_irq_disable();
    wbinvd();
    wrmsr_on_cpu(0, 0x200 + 2 * index, phys_base, 0);     // MTRR_PHYS_BASE
    wrmsr_on_cpu(0, 0x201 + 2 * index, phys_mask, 0);     // MTRR_PHYS_MASK
    wbinvd();
    local_irq_enable();
    __flush_tlb_all();
    printk(KERN_INFO "[ICEPICK] set MTRR%d: base=0x%lx, size=0x%lx to WC\n",
           index, base, size);
  }

  printk(KERN_INFO "[ICEPICK] mapped WC space 0x%lx-0x%lx (%zu bytes)\n",
         (unsigned long)IO_SPACE, (unsigned long)IO_SPACE + IO_SIZE - 1,
         IO_SIZE);
  return 0;
}
/*  cleanup I/O mapped space  */
void cleanup_io(void) {
  if (io_space_virt) {
    iounmap(io_space_virt);
    release_mem_region(IO_SPACE, IO_SIZE);
  }
}
// void cleanup_io(void) {
//   if (io_space_virt) {
//     clflush_cache_range(io_space_virt, IO_SIZE);
//     iounmap(io_space_virt);
//     release_mem_region(IO_SPACE, IO_SIZE);
//   }
// }
/*
 * cache way configuration using standard CAT resctrl for l3 allocation
 * @way    : cache way
 * @cbm    : cache bit mask
 * @return : 0 on success or whatever error our ptr returns
 */
static int configure_cat_way(int way, unsigned long cbm) {
  char path[64], buf[32];
  struct file *f;
  int len, ret = 0;
  struct path parent_path;
  struct inode *parent_inode;
  struct dentry *dentry;

  ret = kern_path("/sys/fs/resctrl", 0, &parent_path);
  if (ret) {
    printk(KERN_ERR "[ICEPICK]      failed to access /sys/fs/resctrl: %d\n", ret);
    return ret;
  }

  parent_inode = parent_path.dentry->d_inode;
  snprintf(path, sizeof(path), "icepick%d", way);

  inode_lock(parent_inode);
  dentry = lookup_one_len(path, parent_path.dentry, strlen(path));
  if (IS_ERR(dentry)) {
    ret = PTR_ERR(dentry);
    printk(KERN_ERR "[ICEPICK]      failed to lookup %s: %d\n", path, ret);
    goto unlock;
  }

  if (!dentry->d_inode) {
    ret = vfs_mkdir(&init_user_ns, parent_inode, dentry,
                    0700);
    if (ret && ret != -EEXIST) {
      printk(KERN_ERR "[ICEPICK]      failed to create resctrl group %s: %d\n", path,
             ret);
      dput(dentry);
      goto unlock;
    }
  }
  dput(dentry);

unlock:
  inode_unlock(parent_inode);
  path_put(&parent_path);
  if (ret)
    return ret;

  snprintf(path, sizeof(path), "/sys/fs/resctrl/icepick%d/tasks", way);
  f = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(f)) {
    printk(KERN_ERR "[ICEPICK]      failed to open tasks file: %ld\n", PTR_ERR(f));
    return PTR_ERR(f);
  }
  len = snprintf(buf, sizeof(buf), "%d\n", current->pid);
  if (kernel_write(f, buf, len, &f->f_pos) < 0)
    ret = -EIO;
  filp_close(f, NULL);

  snprintf(path, sizeof(path), "/sys/fs/resctrl/icepick%d/schemata", way);
  f = filp_open(path, O_WRONLY, 0);
  if (IS_ERR(f)) {
    printk(KERN_ERR "[ICEPICK]      failed to open schemata file: %ld\n",
           PTR_ERR(f));
    return PTR_ERR(f);
  }
  len = snprintf(buf, sizeof(buf), "L3:%d=%lx\n", way, cbm);
  if (kernel_write(f, buf, len, &f->f_pos) < 0)
    ret = -EIO;
  filp_close(f, NULL);

  return ret;
}
/*
 * lock our data into a specified cache way
 * @way    : cache way
 * @data   : data 2 lock
 * @size   : data size
 * @return : 0 on sucess
 */
int lock_cache_way(int way, const void *data, size_t size) {
  unsigned long cbm = (1UL << way) & 0xffff;
  int ret = configure_cat_way(way, cbm);
  volatile char *ptr = io_space_virt;
  size_t i;
  size_t j;

  if (way >= CACHE_WAYS || cache_way_locks[way])
    return -EBUSY;
  if (size > IO_SIZE)
    size = IO_SIZE;

  spin_lock(&cache_lock_spinlock);
  memcpy(io_space_virt, data, size);
  asm volatile("mfence" ::: "memory");

  for (i = 0; i < size; i += 64) {
    asm volatile("prefetcht2 (%0)" : : "r"(ptr + i));
  }
  asm volatile("mfence" ::: "memory");
  (void)ptr[0];

  for (j = 0; j < size; j += 64) {
    (void)ptr[j];
  }
  asm volatile("mfence" ::: "memory");

  if (ret < 0) {
    spin_unlock(&cache_lock_spinlock);
    return ret;
  }

  cache_way_locks[way] = 1;
  cat_configs[way].closid = way;
  cat_configs[way].cbm = cbm;
  cat_configs[way].way = way;

  spin_unlock(&cache_lock_spinlock);

  printk(KERN_INFO "[ICEPICK] locked %zu bytes in L3 way %d\n", size, way);
  return 0;
}

// void flood_l3(void) {
//   volatile char *flood_ptr = io_space_virt + IO_SIZE;
//   for (size_t i = 0; i < L3_FLOOD_SIZE; i += 64) {
//     flood_ptr[i] = (char)(i & 0xFF);
//   }
//   asm volatile("mfence" ::: "memory");

//   for (size_t i = 0; i < L3_FLOOD_SIZE; i += 64) {
//     asm volatile("prefetcht2 (%0)" : : "r"(flood_ptr + i));
//   }
//   asm volatile("mfence" ::: "memory");

//   for (size_t i = 0; i < L3_FLOOD_SIZE; i += 64) {
//     (void)flood_ptr[i];
//   }
//   asm volatile("mfence" ::: "memory");

//   printk(KERN_INFO "[ICEPICK] flooded l3 with %zu bytes from WC\n",
//          (unsigned long)L3_FLOOD_SIZE);
// }
/*
 * RDTSC usage to measure access latency to memory addr
 * @addr   : address 2 measure
 * @return : latency in cycles
 */
static u64 measure_access(void *addr) {
  u32 start_lo, start_hi, end_lo, end_hi;
  /*
  %0 = start_lo
  %1 = start_hi
  %2 = end_lo
  %3 = end_hi
  %4 = addr
  */
  asm volatile(
      "mfence\n\t"
      "rdtsc\n\t"
      "mov %%eax, %0\n\t"     // low 32 bits ( start_lo )
      "mov %%edx, %1\n\t"     // high 32 bits ( start_hi )
      "mfence\n\t"
      "mov (%4), %%eax\n\t"   // addr memory access ( dereference the ptr )
      "mfence\n\t"
      "rdtsc\n\t"
      "mov %%eax, %2\n\t"     // low 32 bits ( end_lo )
      "mov %%edx, %3\n\t"     // high 32 bits ( end_hi )
      : "=r"(start_lo), "=r"(start_hi), "=r"(end_lo), "=r"(end_hi)
      : "r"(addr)
      : "eax", "edx", "memory");
  return ((u64)end_hi << 32 | end_lo) - ((u64)start_hi << 32 | start_lo);
}
/*  check data residency in L3 cache based on the access latency   */
void check_fill(void) {
  u64 latency = measure_access(io_space_virt);
  if (latency < 50) {
    printk(KERN_INFO "[ICEPICK] data in L3 (latency: %llu cycles)\n", latency);
  } else {
    printk(KERN_WARNING "[ICEPICK]      data not in L3 (latency: %llu cycles)\n",
           latency);
  }
}
