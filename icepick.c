#define ICEPICK_IOC_MAGIC 'k'
#define ICEPICK_FLUSH_CACHE _IOW(ICEPICK_IOC_MAGIC, 1, char *)

// #include "include/msr.h"
#include "include/iomap.h"
#include <asm/io.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
/*
  major_num   : major number for our character device
  cache_class : device class for `/sys/class`
  locked_data : buffer holding locked data for integrity checking ( Redundant )
  locked_len  : length of our locked data
*/
static int major_num;
static struct class *cache_class;
static char *locked_data = NULL;
static size_t locked_len = 0;
// static struct cdev cache_cdev;
/*
 * open `ICEPICK` charatcer device
 * @inodep : inode ptr
 * @filep  : file ptr
 * @return : 0 on sucess
 */
static int d_open(struct inode *inodep, struct file *filep) {
  printk(KERN_INFO "[ICEPICK] device opened\n");
  return 0;
}
/*
 * close `ICEPICK` charatcer device
 * @inodep : inode ptr
 * @filep  : file ptr
 * @return : 0 on sucess
 */
static int d_release(struct inode *inodep, struct file *filep) {
  printk(KERN_INFO "[ICEPICK] device closed\n");
  return 0;
}
/*
 * write data to character device and lock it in l3 cache way 0
 * @filep  : file ptr
 * @buffer : user-space buffer
 * @len    : length of data to write
 * @offset : offset in the cache
 * @return : bytes written or -1 on failure
 */
static ssize_t d_write(struct file *filep, const char __user *buffer,
                       size_t len, loff_t *offset) {
  int ret;

  if (len > CACHE_SIZE)
    return -EINVAL;

  if (locked_data) {
    kfree(locked_data);
    locked_data = NULL;
    locked_len = 0;
  }

  locked_data = kmalloc(len, GFP_KERNEL);
  if (!locked_data)
    return -ENOMEM;

  if (copy_from_user(locked_data, buffer, len)) {
    kfree(locked_data);
    locked_data = NULL;
    return -EFAULT;
  }

  ret = lock_cache_way(0, locked_data, len);
  if (ret == 0) {
    if (memcmp(io_space_virt, locked_data, len) != 0) {
      printk(KERN_WARNING "[ICEPICK]      data mismatch after lock!\n");
    }
    check_fill();
    check_evictions();
    locked_len = len;
    *offset += len;
    return len;
  }

  kfree(locked_data);
  locked_data = NULL;
  return ret;
}
/*
 * Read data from locked l3 cache
 * @filep  : file ptr
 * @buffer : user-space buffer holding data
 * @len    : length of the data to read
 * @offset : offset in the cache
 * @return : bytes written or -EFAULT
 */
static ssize_t d_read(struct file *filep, char __user *buffer, size_t len,
                      loff_t *offset) {
  int error_count;

  if (*offset >= CACHE_SIZE)
    return 0;
  if (*offset + len > CACHE_SIZE)
    len = CACHE_SIZE - *offset;

  if (locked_data && locked_len >= *offset + len) {
    if (memcmp(io_space_virt + *offset, locked_data + *offset, len) != 0) {
      printk(
          KERN_WARNING
          "[ICEPICK]      data in L3 no longer matches initial test data!\n");
    } else {
      printk(KERN_INFO "[ICEPICK] data in L3 matches initial data\n");
    }
  }

  check_fill();
  check_evictions();

  error_count = copy_to_user(buffer, io_space_virt + *offset, len);
  if (error_count == 0) {
    *offset += len;
    return len;
  }

  return -EFAULT;
}
/*
 * manage IOCTL cmds to flush L3 cache
 * @filep  : file ptr
 * @cmd    : IOCTL command
 * @arg    : user-space argument
 * @return : 0 on success or -EINVAL
 */
static long d_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
  char user_input[32];
  static const char *flush_cmd = "please flush my cachelines";
  if (cmd != ICEPICK_FLUSH_CACHE)
    return -EINVAL;

  if (copy_from_user(user_input, (char *)arg, sizeof(user_input)))
    return -EFAULT;

  if (strncmp(user_input, flush_cmd, sizeof(user_input)) != 0)
    return -EINVAL;

  spin_lock(&cache_lock_spinlock);
  clflush_cache_range(io_space_virt, CACHE_SIZE);
  asm volatile("mfence" ::: "memory");
  printk(KERN_INFO "[ICEPICK] flushed L3 cache lines\n");
  spin_unlock(&cache_lock_spinlock);

  return 0;
}

/*  file ops structure for `ICEPICK` character device  */
static struct file_operations fops = {
    .open = d_open,
    .release = d_release,
    .read = d_read,
    .write = d_write,
    .unlocked_ioctl = d_ioctl,
};
// // Test ram buffer
// static int alloc_cm(void) {
//   cache_virt_addr = kmalloc(CACHE_SIZE, GFP_KERNEL | GFP_DMA);
//   if (!cache_virt_addr) {
//     printk(KERN_ERR "[ICEPICK]     cannot allocate cache mem\n");
//     return -ENOMEM;
//   }

//   cache_phys_addr = virt_to_phys(cache_virt_addr);
//   printk(KERN_INFO "[ICEPICK] allocated %zu bytes at phys addr 0x%llx\n",
//          CACHE_SIZE, (unsigned long long)cache_phys_addr);
//   return 0;
// }
/*
 * initialise `ICEPICK` kmod
 * @return : 0 on sucess or -1 on failure
 */
static int __init icepick_init(void) {
  printk(KERN_INFO "[ICEPICK] initialising module...\n");

  major_num = register_chrdev(0, DEVICE_NAME, &fops);
  if (major_num < 0) {
    printk(KERN_ALERT "[ICEPICK]      failed to register chrdev\n");
    return major_num;
  }

  printk(KERN_INFO "[ICEPICK] registered chrdev with major %d\n", major_num);

  // cache_class = class_create(THIS_MODULE, CLASS_NAME);     // May need to use
  // this intead if the blow `class_create()` expects an additional argument.
  cache_class = class_create(CLASS_NAME);
  if (IS_ERR(cache_class)) {
    unregister_chrdev(major_num, DEVICE_NAME);

    printk(KERN_ALERT "[ICEPICK]      failed to create class\n");
    return PTR_ERR(cache_class);
  }

  if (IS_ERR(device_create(cache_class, NULL, MKDEV(major_num, 0), NULL,
                           DEVICE_NAME))) {
    class_destroy(cache_class);
    unregister_chrdev(major_num, DEVICE_NAME);

    printk(KERN_ALERT "[ICEPICK]      failed to create device\n");
    return PTR_ERR(cache_class);
  }

  if (init_io() < 0) {
    device_destroy(cache_class, MKDEV(major_num, 0));
    class_destroy(cache_class);
    unregister_chrdev(major_num, DEVICE_NAME);

    printk(KERN_ALERT "[ICEPICK]      failed to init I/O\n");
    return -1;
  }

  if (init_cache_monitor() < 0) {
    cleanup_io();
    device_destroy(cache_class, MKDEV(major_num, 0));
    class_destroy(cache_class);
    unregister_chrdev(major_num, DEVICE_NAME);

    printk(KERN_ALERT "[ICEPICK]      failed to init cache monitor\n");
    return -1;
  }

  printk(KERN_INFO "[ICEPICK] module initialised\n");
  return 0;
}
/*  clean up and exit our `ICEPICK` module  */
static void __exit icepick_exit(void) {
  if (locked_data) {
    kfree(locked_data);
    locked_data = NULL;
    locked_len = 0;
  }

  cleanup_cache_monitor();
  cleanup_io();
  device_destroy(cache_class, MKDEV(major_num, 0));
  class_destroy(cache_class);
  unregister_chrdev(major_num, DEVICE_NAME);

  printk(KERN_INFO "[ICEPICK] module removed\n");
}

module_init(icepick_init);
module_exit(icepick_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MEOW");
MODULE_DESCRIPTION("cache allocation pseudo-locking");
