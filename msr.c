#include "include/msr.h"

void set_mtrr_wc(unsigned long base, unsigned long size, int index) {
  u64 phys_base, phys_mask, mtrr_cap, mtrr_def;

  rdmsr(MSR_MTRRcap, mtrr_cap, mtrr_def);
  if (index >= (mtrr_cap & 0xFF)) {
    printk(KERN_ERR "[ICEPICK]      MTRR index %d exceeds capability\n", index);
    return;
  }

  base &= ~0xFFFUL;
  size = roundup_pow_of_two(size);

  phys_base = base | MTRR_TYPE_WC;
  phys_mask = ~(size - 1) | MTRR_ENABLE;

  local_irq_disable();
  wbinvd();
  wrmsr(MTRR_PHYS_BASE(index), phys_base, 0);
  wrmsr(MTRR_PHYS_MASK(index), phys_mask, 0);
  wbinvd();
  local_irq_enable();
  __flush_tlb_all();

  printk(KERN_INFO "[ICEPICK] set MTRR%d: base=0x%lx, size=0x%lx to write-combined ( WC )\n",
         index, base, size);
}

void disable_mce_writeback_errors(void) {
  u64 mcg_ctl;
  rdmsr(MSR_MCG_CTL, mcg_ctl, mcg_ctl);
  mcg_ctl = 0xFFFFFFFFFFFFFFFFULL;
  wrmsr(MSR_MCG_CTL, mcg_ctl, 0);
  wrmsr(MSR_MC0_CTL, 0x0, 0x0);
  printk(KERN_INFO "[ICEPICK] disabled MCE for write-back ( WB ) errors\n");
}
