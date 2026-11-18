/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 LightSnow */
#ifndef CLONE_BREAKPOINT_H
#define CLONE_BREAKPOINT_H

#include <linux/uprobes.h>

struct task_struct;
struct clone_breakpoint;
typedef void (*clone_breakpoint_hit_t)(struct clone_breakpoint *breakpoint,
				       struct pt_regs *regs);

/* Zero-initialize before use. Keep it alive until unregister returns. */
struct clone_breakpoint {
	struct uprobe_consumer consumer;
	struct uprobe *uprobe;
	struct inode *inode;
	struct mm_struct *mm;
	struct task_struct *task;
	clone_breakpoint_hit_t on_hit;
	unsigned long address;
	loff_t offset;
	bool armed;
};

int clone_breakpoint_register(struct clone_breakpoint *breakpoint,
			      struct task_struct *task, unsigned long address,
			      clone_breakpoint_hit_t on_hit);
void clone_breakpoint_unregister(struct clone_breakpoint *breakpoint);

#endif
