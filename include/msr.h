#ifndef MSR_H
#define MSR_H

#include <asm/msr.h>
#include <asm/tlbflush.h>
#include <linux/module.h>

#define MTRR_PHYS_BASE(n) (0x200 + 2 * (n))
#define MTRR_PHYS_MASK(n) (0x201 + 2 * (n))
#define MTRR_TYPE_WC 0x1
#define MTRR_ENABLE (1ULL << 11)
#define MSR_MCG_CTL 0x17B
#define MSR_MC0_CTL 0x400

void set_mtrr_wc(unsigned long base, unsigned long size, int index);
void disable_mce_writeback_errors(void);

#endif
