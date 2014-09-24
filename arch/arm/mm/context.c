/*
 *  linux/arch/arm/mm/context.c
 *
 *  Copyright (C) 2012 ARM Limited
 *
 *  Author: Will Deacon <will.deacon@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/percpu.h>

#include <asm/mmu_context.h>
#include <asm/smp_plat.h>
#include <asm/thread_notify.h>
#include <asm/tlbflush.h>

#include <mach/msm_rtb.h>

/*
 * On ARMv6, we have the following structure in the Context ID:
 *
 * 31                         7          0
 * +-------------------------+-----------+
 * |      process ID         |   ASID    |
 * +-------------------------+-----------+
 * |              context ID             |
 * +-------------------------------------+
 *
 * The ASID is used to tag entries in the CPU caches and TLBs.
 * The context ID is used by debuggers and trace logic, and
 * should be unique within all running processes.
 */
#define ASID_FIRST_VERSION	(1ULL << ASID_BITS)

static DEFINE_RAW_SPINLOCK(cpu_asid_lock);
static u64 cpu_last_asid = ASID_FIRST_VERSION;

static DEFINE_PER_CPU(u64, active_asids);
static DEFINE_PER_CPU(u64, reserved_asids);
static cpumask_t tlb_flush_pending;

#ifdef CONFIG_SMP
DEFINE_PER_CPU(struct mm_struct *, current_mm);
#endif

#ifdef CONFIG_ARM_LPAE
#define cpu_set_asid(asid) {						\
	unsigned long ttbl, ttbh;					\
	asm volatile(							\
	"	mrrc	p15, 0, %0, %1, c2		@ read TTBR0\n"	\
	"	mov	%1, %2, lsl #(48 - 32)		@ set ASID\n"	\
	"	mcrr	p15, 0, %0, %1, c2		@ set TTBR0\n"	\
	: "=&r" (ttbl), "=&r" (ttbh)					\
	: "r" (asid & ~ASID_MASK));					\
}
#else
#define cpu_set_asid(asid) \
	asm("	mcr	p15, 0, %0, c13, c0, 1\n" : : "r" (asid))
#endif

static void write_contextidr(u32 contextidr)
{
	uncached_logk(LOGK_CTXID, (void *)contextidr);
	asm("mcr	p15, 0, %0, c13, c0, 1" : : "r" (contextidr));
	isb();
}

static u32 read_contextidr(void)
{
	u32 contextidr;
	asm("mrc	p15, 0, %0, c13, c0, 1" : "=r" (contextidr));
	return contextidr;
}

static int contextidr_notifier(struct notifier_block *unused, unsigned long cmd,
			       void *t)
{
	unsigned long flags;
	u32 contextidr;
	pid_t pid;
	struct thread_info *thread = t;

	if (cmd != THREAD_NOTIFY_SWITCH)
		return NOTIFY_DONE;

	pid = task_pid_nr(thread->task);
	local_irq_save(flags);
	contextidr = read_contextidr();
	contextidr &= ~ASID_MASK;
	contextidr |= pid << ASID_BITS;
	write_contextidr(contextidr);
	local_irq_restore(flags);

	return NOTIFY_OK;
}

static struct notifier_block contextidr_notifier_block = {
	.notifier_call = contextidr_notifier,
};

static int __init contextidr_notifier_init(void)
{
	return thread_register_notifier(&contextidr_notifier_block);
}
arch_initcall(contextidr_notifier_init);

static void flush_context(unsigned int cpu)
{
	int i;

	/* Update the list of reserved ASIDs. */
	per_cpu(active_asids, cpu) = 0;
	for_each_possible_cpu(i)
		per_cpu(reserved_asids, i) = per_cpu(active_asids, i);

	/* Queue a TLB invalidate and flush the I-cache if necessary. */
	if (!tlb_ops_need_broadcast())
		cpumask_set_cpu(cpu, &tlb_flush_pending);
	else
		cpumask_setall(&tlb_flush_pending);

	if (icache_is_vivt_asid_tagged())
		__flush_icache_all();
}

static int is_reserved_asid(u64 asid, u64 mask)
{
	int cpu;
	for_each_possible_cpu(cpu)
		if ((per_cpu(reserved_asids, cpu) & mask) == (asid & mask))
			return 1;
	return 0;
}

static void new_context(struct mm_struct *mm, unsigned int cpu)
{
	u64 asid = mm->context.id;

	if (asid != 0 && is_reserved_asid(asid, ULLONG_MAX)) {
		/*
		 * Our current ASID was active during a rollover, we can
		 * continue to use it and this was just a false alarm.
		 */
		asid = (cpu_last_asid & ASID_MASK) | (asid & ~ASID_MASK);
	} else {
		/*
		 * Allocate a free ASID. If we can't find one, take a
		 * note of the currently active ASIDs and mark the TLBs
		 * as requiring flushes.
		 */
		do {
			asid = ++cpu_last_asid;
			if ((asid & ~ASID_MASK) == 0)
				flush_context(cpu);
		} while (is_reserved_asid(asid, ~ASID_MASK));
		cpumask_clear(mm_cpumask(mm));
	}

	mm->context.id = asid;
}

void check_and_switch_context(struct mm_struct *mm, struct task_struct *tsk)
{
	unsigned long flags;
	unsigned int cpu = smp_processor_id();

	if (unlikely(mm->context.kvm_seq != init_mm.context.kvm_seq))
		__check_kvm_seq(mm);

	cpu_set_asid(0);
	isb();

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	/* Check that our ASID belongs to the current generation. */
	if ((mm->context.id ^ cpu_last_asid) >> ASID_BITS)
		new_context(mm, cpu);

	*this_cpu_ptr(&active_asids) = mm->context.id;
	cpumask_set_cpu(cpu, mm_cpumask(mm));

	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
		local_flush_tlb_all();
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

	cpu_switch_mm(mm->pgd, mm);
}
