# ARM64 虚拟页断点

传入目标线程和运行时地址，由 Linux `uprobes` 在目标进程的私有代码页上放置 `BRK` 执行断点。地址要四字节对齐，落在私有、不可写的可执行文件映射里。接口接收一个清零的 `struct clone_breakpoint`，命中后把目标线程的寄存器现场交给 `on_hit`。[接口定义](include/clone_breakpoint.h#L10)

## 从虚拟地址找到文件偏移

同一份代码在不同进程里可以映射到不同地址。原生 `uprobes` 按 inode 和文件偏移找指令，因此 `find_probe_location()` 先查目标 `mm` 的 VMA，再把地址换算成文件里的位置。

```c
delta = address - vma->vm_start;
if (vma->vm_pgoff > ((u64)S64_MAX - delta) >> PAGE_SHIFT)
    return -EOVERFLOW;
*inode = file_inode(vma->vm_file);
*offset = ((loff_t)vma->vm_pgoff << PAGE_SHIFT) + delta;
```

第一次查 VMA 时在读锁内用 `igrab()` 留住 inode。原生注册会自己拿 `mmap_write_lock()`，所以先释放读锁，再调用 `uprobe_register()`。注册回来重新查一次 VMA。文件或偏移对不上就注销刚注册的探针，返回 `-ESTALE`。确认选中的线程还在原地址空间，才设 `armed` 开始接收命中。[文件偏移换算](src/clone_breakpoint.c#L60) · [注册与复核](src/clone_breakpoint.c#L107)

## BRK 写到哪里

`register_uprobe()` 把 inode、偏移和消费者交给内核，`filter_mm()` 筛出目标地址空间。

```c
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
```

以 Android 6.6 为例，内核的 `uprobe_write_opcode()` 复制当前代码页，在副本上写入 `BRK`，再由 `__replace_page()` 把目标映射换到副本。6.18 在需要时通过写缺页取得私有 COW 页，然后更新指令。读取目标映射时能看到 `BRK`，磁盘文件保留原字节。[6.6 页复制与指令写入](https://android.googlesource.com/kernel/common/+/46a034eca00581425741df52f0b5d634c6b6206e/kernel/events/uprobes.c#474) · [6.18 指令写入](https://android.googlesource.com/kernel/common/+/47cc2255444237bbfd61470ee16099ab99cfc82c/kernel/events/uprobes.c#493)

## 命中与原指令恢复

共享同一个 `mm` 的线程也可能执行到这条 `BRK`。线程返回用户态前，内核从 `uprobe_notify_resume()` 进入消费者。组件再核对目标线程、PC 和用户单步状态，条件都匹配才调用 `on_hit`。fork 后的子进程如果继承了这条 `BRK`，组件会把移除请求交给内核处理。

```c
if (current->mm != breakpoint->mm)
    return UPROBE_HANDLER_REMOVE;
if (READ_ONCE(breakpoint->armed) && current == breakpoint->task && user_mode(regs) &&
    instruction_pointer(regs) == breakpoint->address &&
    !READ_ONCE(current->ptrace) && !test_thread_flag(TIF_SINGLESTEP))
    breakpoint->on_hit(breakpoint, regs);
```

`on_hit` 在目标线程里跑，`pt_regs` 只在这次回调里有效，交给其他线程处理时先拷一份。比如在回调返回前写入 `regs->regs[0]`，原指令读到的就是新的 X0。PC 仍指向断点时，内核按指令类型选择模拟执行，或者把原指令放到 XOL 槽单步执行，再恢复 PC。[命中回调](src/clone_breakpoint.c#L41) · [ARM64 原指令执行](https://android.googlesource.com/kernel/common/+/46a034eca00581425741df52f0b5d634c6b6206e/arch/arm64/kernel/probes/uprobes.c#62)

## 注销与引用

注册成功后，断点持有 inode、任务和 `mm_struct` 的引用。注册期间 `get_task_mm()` 拿到的临时引用在返回前用 `mmput()` 放掉。长期保留的是 `mmgrab()` 拿到的对象引用，任务退出时可以照常回收它的映射。

```c
WRITE_ONCE(breakpoint->armed, false);
unregister_uprobe(breakpoint);
release_breakpoint(breakpoint);
```

先关掉新的命中回调，再从内核注销。6.12 和 6.18 把移除消费者和等待回调拆成 `uprobe_unregister_nosync()` 与 `uprobe_unregister_sync()`，等回调结束后才归还引用。回调如果正停着等继续，控制线程先唤醒它，放下回调要拿的锁，再注销。注销返回后就可以释放或复用断点对象。[同步注销](src/clone_breakpoint.c#L89) · [注销入口](src/clone_breakpoint.c#L175)

开源署名：**LightSnow**  
Telegram：[@XiaoxuePD](https://t.me/XiaoxuePD)  
许可证：[GPL-2.0-only](LICENSE)
