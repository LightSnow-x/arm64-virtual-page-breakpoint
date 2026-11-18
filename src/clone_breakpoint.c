// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 LightSnow
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/string.h>
#include <asm/compat.h>
#include <asm/ptrace.h>

#include "clone_breakpoint.h"

#if !defined(CONFIG_ARM64) || !defined(CONFIG_UPROBES)
#error "LightSnow clone breakpoints require ARM64 and CONFIG_UPROBES"
#endif

#ifndef CLONE_BREAKPOINT_UPROBE_API
#if defined(UPROBE_HANDLER_IGNORE)
#define CLONE_BREAKPOINT_UPROBE_API 3
#elif defined(UPROBE_NO_TRAMPOLINE_VADDR)
#define CLONE_BREAKPOINT_UPROBE_API 2
#else
#define CLONE_BREAKPOINT_UPROBE_API 1
#endif
#endif

#if CLONE_BREAKPOINT_UPROBE_API == 1
static bool filter_mm(struct uprobe_consumer *uc, enum uprobe_filter_ctx ctx,
		      struct mm_struct *mm)
#else
static bool filter_mm(struct uprobe_consumer *uc, struct mm_struct *mm)
#endif
{
	struct clone_breakpoint *breakpoint =
		container_of(uc, struct clone_breakpoint, consumer);

	return mm == breakpoint->mm;
}

#if CLONE_BREAKPOINT_UPROBE_API == 3
static int handle_hit(struct uprobe_consumer *uc, struct pt_regs *regs, u64 *data)
#else
static int handle_hit(struct uprobe_consumer *uc, struct pt_regs *regs)
#endif
{
	struct clone_breakpoint *breakpoint =
		container_of(uc, struct clone_breakpoint, consumer);

	/* A fork can inherit the BRK. */
	if (current->mm != breakpoint->mm)
		return UPROBE_HANDLER_REMOVE;
	if (READ_ONCE(breakpoint->armed) && current == breakpoint->task && user_mode(regs) &&
	    instruction_pointer(regs) == breakpoint->address &&
	    !READ_ONCE(current->ptrace) && !test_thread_flag(TIF_SINGLESTEP))
		breakpoint->on_hit(breakpoint, regs);
	return 0;
}

static int find_probe_location(struct mm_struct *mm, unsigned long address,
			       struct inode **inode, loff_t *offset)
{
	const unsigned long mask = VM_EXEC | VM_MAYEXEC | VM_WRITE | VM_MAYSHARE | VM_HUGETLB;
	struct vm_area_struct *vma = find_vma(mm, address);
	unsigned long delta;

	if (!vma || vma->vm_start > address || !vma->vm_file ||
	    (vma->vm_flags & mask) != (VM_EXEC | VM_MAYEXEC))
		return -EOPNOTSUPP;
	delta = address - vma->vm_start;
	if (vma->vm_pgoff > ((u64)S64_MAX - delta) >> PAGE_SHIFT)
		return -EOVERFLOW;
	*inode = file_inode(vma->vm_file);
	*offset = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + delta;
	return 0;
}

static int register_uprobe(struct clone_breakpoint *breakpoint)
{
#if CLONE_BREAKPOINT_UPROBE_API != 1
	breakpoint->uprobe = uprobe_register(breakpoint->inode, breakpoint->offset,
					   0, &breakpoint->consumer);
	return PTR_ERR_OR_ZERO(breakpoint->uprobe);
#else
	return uprobe_register(breakpoint->inode, breakpoint->offset, &breakpoint->consumer);
#endif
}

static void unregister_uprobe(struct clone_breakpoint *breakpoint)
{
#if CLONE_BREAKPOINT_UPROBE_API != 1
	uprobe_unregister_nosync(breakpoint->uprobe, &breakpoint->consumer);
	uprobe_unregister_sync();
#else
	uprobe_unregister(breakpoint->inode, breakpoint->offset, &breakpoint->consumer);
#endif
}

static void release_breakpoint(struct clone_breakpoint *breakpoint)
{
	iput(breakpoint->inode);
	mmdrop(breakpoint->mm);
	put_task_struct(breakpoint->task);
	memset(breakpoint, 0, sizeof(*breakpoint));
}

int clone_breakpoint_register(struct clone_breakpoint *breakpoint,
			      struct task_struct *task, unsigned long address,
			      clone_breakpoint_hit_t on_hit)
{
	struct mm_struct *mm;
	struct inode *inode;
	loff_t offset;
	int ret;

	if (!on_hit || !IS_ALIGNED(address, sizeof(u32)) || address >= TASK_SIZE_64)
		return -EINVAL;
	if (breakpoint->inode)
		return -EBUSY;
	if (is_compat_thread(task_thread_info(task)))
		return -EOPNOTSUPP;
	if (READ_ONCE(task->ptrace) || test_tsk_thread_flag(task, TIF_SINGLESTEP))
		return -EBUSY;
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;

	mmap_read_lock(mm);
	ret = find_probe_location(mm, address, &inode, &offset);
	if (!ret) {
		breakpoint->inode = igrab(inode);
		if (!breakpoint->inode)
			ret = -ESTALE;
	}
	mmap_read_unlock(mm);
	if (ret)
		goto out;

	mmgrab(mm);
	get_task_struct(task);
	breakpoint->mm = mm;
	breakpoint->task = task;
	breakpoint->address = address;
	breakpoint->offset = offset;
	breakpoint->on_hit = on_hit;
	breakpoint->consumer.handler = handle_hit;
	breakpoint->consumer.filter = filter_mm;

	ret = register_uprobe(breakpoint);
	if (ret)
		goto put;

	/* The mapping can change while uprobe_register() runs. */
	mmap_read_lock(mm);
	ret = find_probe_location(mm, address, &inode, &offset);
	if (ret || inode != breakpoint->inode || offset != breakpoint->offset)
		ret = -ESTALE;
	mmap_read_unlock(mm);
	if (!ret && (READ_ONCE(task->mm) != mm || (READ_ONCE(task->flags) & PF_EXITING)))
		ret = -ESRCH;
	if (ret) {
		unregister_uprobe(breakpoint);
		goto put;
	}

	WRITE_ONCE(breakpoint->armed, true);
	goto out;
put:
	release_breakpoint(breakpoint);
out:
	mmput(mm);
	return ret;
}

void clone_breakpoint_unregister(struct clone_breakpoint *breakpoint)
{
	if (!breakpoint->inode)
		return;

	WRITE_ONCE(breakpoint->armed, false);
	unregister_uprobe(breakpoint);
	release_breakpoint(breakpoint);
}
