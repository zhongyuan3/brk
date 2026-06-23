#include <arch/pgtable.h>
#include <arch/trapframe.h>
#include <asm/page.h>
#include <asm/vas_layout.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kernel.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timer.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <uapi/brk/errno.h>
#include <uapi/brk/limits.h>
#include <uapi/elf.h>
#include <uapi/fcntl.h>

struct exec_args {
	char **argv;
	char **envp;
	size_t argv_size;
	size_t envp_size;
	int argc;
	int envc;
};

struct exec_strings_acc {
	int count;
	size_t bytes;
};

static int exec_read_exact(struct fs_file *fp, u64 off, void *buf, size_t n)
{
	loff_t ret = fs_file_lseek(fp, off, SEEK_SET);

	if (ret < 0) {
		klog_error("%s(): Failed to lseek file: %s\n", __func__,
			   strerror(ret));
		return -EIO;
	}

	ssize_t rcnt = fs_file_read(fp, buf, n);

	if (rcnt < 0) {
		klog_error("%s(): Failed to read file: %s\n", __func__,
			   strerror(rcnt));
		return -EIO;
	}
	if ((size_t)rcnt != n) {
		klog_error("%s(): Failed to read file (short read)\n",
			   __func__);
		return -EIO;
	}
	return 0;
}

static int elf_validate_exec_hdr(const struct elf64_hdr *h)
{
	if (memcmp(h->e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;
	if (h->e_type != ET_EXEC)
		return -ENOEXEC;
	return 0;
}

static int elf_read_phdr(struct fs_file *f, u64 off, struct elf64_phdr *phdr)
{
	ssize_t r;
	loff_t ret = fs_file_lseek(f, off, SEEK_SET);

	if (ret < 0)
		return -EIO;
	r = fs_file_read(f, phdr, sizeof(*phdr));
	if (r < 0)
		return -EIO;
	if ((size_t)r != sizeof(*phdr))
		return -ENOEXEC;
	return 0;
}

static int exec_read_elf_header(struct fs_file *f, struct elf64_hdr *elf_hdr)
{
	ssize_t r = fs_file_read(f, elf_hdr, sizeof(*elf_hdr));

	if (r < 0)
		return -EIO;
	if (r != sizeof(*elf_hdr))
		return -ENOEXEC;
	return elf_validate_exec_hdr(elf_hdr);
}

static int exec_accum_strings(char **arr, size_t *total_size,
			      struct exec_strings_acc *out)
{
	out->count = 0;
	out->bytes = 0;
	for (; arr[out->count]; ++out->count) {
		size_t l = strlen(arr[out->count]) + 1;

		out->bytes += l;
		*total_size += l;
		if (*total_size > ARG_MAX)
			return -E2BIG;
	}
	return 0;
}

static u64 random_ustack(void)
{
	u64 ustack_top = USER_SPACE_SIZE_MAX;

	timer_srand();
	ustack_top -= (timer_rand() % 32 + 1) * PAGE_SIZE;
	ustack_top -= USTACK_SIZE;
	return ustack_top;
}

static u64 stack_push_ptr_slots(u64 *ksp, u64 *sp, int nptr)
{
	size_t n = (size_t)(nptr + 1) * sizeof(char *);

	*ksp -= n;
	*ksp = round_down(*ksp, sizeof(char *));
	*sp -= n;
	*sp = round_down(*sp, sizeof(char *));
	return *sp;
}

static void copy_exec_strings(char **src, int n, u64 *k_pos, u64 *u_pos,
			      char **dst_ptr_array)
{
	for (int i = 0; i < n; ++i) {
		size_t l = strlen(src[i]) + 1;

		memcpy((char *)*k_pos, src[i], l);
		dst_ptr_array[i] = (char *)*u_pos;
		*u_pos += l;
		*k_pos += l;
	}
}

static void push_args(struct exec_args *args, u64 *psp, u64 stack_virt,
		      struct task_control_block *task)
{
	u64 sp = *psp;
	u64 ksp = stack_virt + USTACK_SIZE;

	ksp -= args->envp_size;
	sp -= args->envp_size;
	u64 envp_strs_kstart = ksp;
	u64 envp_strs_ustart = sp;

	ksp -= args->argv_size;
	sp -= args->argv_size;
	u64 argv_strs_kstart = ksp;
	u64 argv_strs_ustart = sp;

	arch_tf_set_a2(task->tf, stack_push_ptr_slots(&ksp, &sp, args->envc));
	char **envp_kstart = (char **)ksp;

	arch_tf_set_a1(task->tf, stack_push_ptr_slots(&ksp, &sp, args->argc));
	char **argv_kstart = (char **)ksp;

	copy_exec_strings(args->envp, args->envc, &envp_strs_kstart,
			  &envp_strs_ustart, envp_kstart);
	copy_exec_strings(args->argv, args->argc, &argv_strs_kstart,
			  &argv_strs_ustart, argv_kstart);

	argv_kstart[args->argc] = NULL;
	envp_kstart[args->envc] = NULL;

	ksp -= sizeof(int);
	sp -= sizeof(int);
	*(int *)ksp = args->argc;

	ksp = round_down(ksp, 16);
	sp = round_down(sp, 16);

	*psp = sp;
}

static unsigned int flags_to_perm(unsigned int flags)
{
	unsigned int perm = 0;

	if (flags & PF_X)
		perm |= PTE_X;
	if (flags & PF_W)
		perm |= PTE_W;
	if (flags & PF_R)
		perm |= PTE_R;
	return perm;
}

static int map_seg(struct uvm_space *mm, struct uvm_region *vma,
		   struct elf64_phdr *ph, struct fs_file *fp)
{
	int err = 0;
	size_t npgs = vma->size >> PAGE_SHIFT;
	struct page **pgs = kcalloc(npgs, sizeof(struct page *));
	size_t i = 0;
	u64 addr = vma->addr;
	unsigned int flags = vma->flags;
	u64 seg_vstart = ph->p_vaddr;
	u64 seg_fend = ph->p_vaddr + ph->p_filesz;

	if (!pgs)
		return -ENOMEM;

	while (i < npgs) {
		struct page *pg = page_alloc(0);

		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		u64 pa = page_to_phys(pg);
		void *va = (void *)phys_to_virt(pa);
		u64 page_lo = addr;
		u64 page_hi = addr + PAGE_SIZE;
		u64 file_lo = page_lo > seg_vstart ? page_lo : seg_vstart;
		u64 file_hi = page_hi < seg_fend ? page_hi : seg_fend;

		memset(va, 0, PAGE_SIZE);
		if (file_lo < file_hi) {
			size_t n = (size_t)(file_hi - file_lo);
			u64 fo = ph->p_offset + (file_lo - seg_vstart);
			size_t dst_off = (size_t)(file_lo - page_lo);

			err = exec_read_exact(fp, fo, (char *)va + dst_off, n);
			if (err) {
				ASSERT(pg);
				page_free(pg, 0);
				goto failed;
			}
		}
		err = uvmap(mm->pgd, addr, PAGE_SIZE, pa, flags);
		if (err) {
			ASSERT(pg);
			page_free(pg, 0);
			goto failed;
		}
		pgs[i] = pg;
		addr += PAGE_SIZE;
		++i;
	}

	vma->pages = pgs;
	vma->nr_pages = npgs;
	return 0;

failed:
	for (u64 a = vma->addr; a < addr; a += PAGE_SIZE)
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (size_t j = 0; j < i; ++j) {
		ASSERT(pgs[j]);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

static int load_seg(struct uvm_space *mm, struct elf64_phdr *ph,
		    struct fs_file *fp)
{
	u64 start, end;
	struct uvm_region *vma;
	int err;

	start = round_down(ph->p_vaddr, PAGE_SIZE);
	end = round_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
	if (end == start)
		return 0;

	vma = uvm_region_alloc();
	if (!vma)
		return -ENOMEM;
	vma->addr = start;
	vma->size = end - start;
	vma->flags = flags_to_perm(ph->p_flags);

	err = map_seg(mm, vma, ph, fp);
	if (err) {
		uvm_region_free(vma);
		return err;
	}

	list_add(&vma->list, &mm->seg);
	return 0;
}

static u64 find_brk(struct uvm_space *mm)
{
	struct uvm_region *vma = NULL;
	u64 brk = 0;

	list_for_each_entry(vma, &mm->seg, list) {
		u64 end = vma->addr + vma->size;

		if (end >= brk)
			brk = end;
	}
	return brk;
}

static int map_stack(struct uvm_space *mm)
{
	struct page *pg = page_alloc(USTACK_PAGE_ORDER);
	struct page **pgs;
	u64 pa;
	u64 base;
	int err;

	if (!pg)
		return -ENOMEM;

	pgs = kcalloc(1, sizeof(struct page *));
	if (!pgs) {
		ASSERT(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	pa = page_to_phys(pg);
	base = random_ustack();
	err = uvmap(mm->pgd, base, USTACK_SIZE, pa, PTE_R | PTE_W);
	if (err) {
		kfree(pgs);
		ASSERT(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return err;
	}
	pgs[0] = pg;

	mm->stack->addr = base;
	mm->stack->size = USTACK_SIZE;
	mm->stack->flags = PTE_R | PTE_W;
	mm->stack->pages = pgs;
	mm->stack->nr_pages = 1;
	return 0;
}

static int __do_execve(const char *path, struct exec_args *args)
{
	struct elf64_hdr elf_hdr = { 0 };
	struct elf64_phdr phdr = { 0 };
	struct task_control_block *task = current_task();
	struct fs_file *f = NULL;
	struct uvm_space *new_mm = NULL;
	int err = 0;

	f = do_openat(AT_FDCWD, path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		goto err;
	}

	err = exec_read_elf_header(f, &elf_hdr);
	if (err)
		goto err;

	new_mm = uvm_space_create();
	if (IS_ERR(new_mm)) {
		err = PTR_ERR(new_mm);
		goto err;
	}

	{
		u64 phoff = elf_hdr.e_phoff;

		for (u16 i = 0; i < elf_hdr.e_phnum;
		     ++i, phoff += sizeof(phdr)) {
			err = elf_read_phdr(f, phoff, &phdr);
			if (err)
				goto err;
			if (phdr.p_type != PT_LOAD)
				continue;
			err = load_seg(new_mm, &phdr, f);
			if (err)
				goto err;
		}
	}

	fs_file_put(f);
	f = NULL;

	new_mm->heap->addr = find_brk(new_mm);
	new_mm->heap->size = 0;
	new_mm->heap->flags = PTE_R | PTE_W;
	new_mm->brk = new_mm->heap->addr;

	err = map_stack(new_mm);
	if (err)
		goto err;

	{
		u64 sp = new_mm->stack->addr + new_mm->stack->size;
		struct page *stack_pg = new_mm->stack->pages[0];
		u64 stack_phys = page_to_phys(stack_pg);
		u64 stack_virt = phys_to_virt(stack_phys);

		push_args(args, &sp, stack_virt, task);
		strlcpy(task->name, args->argv[0], sizeof(task->name));

		switch_pgtable(new_mm->pgd);

		struct uvm_space *old_mm = task->mm;

		task->mm = new_mm;
		uvm_space_put(old_mm);
		signal_reset(task);
		arch_tf_set_user_entry(task->tf, elf_hdr.e_entry, sp);
	}

	return args->argc;

err:
	if (new_mm && !IS_ERR(new_mm))
		uvm_space_put(new_mm);
	if (!IS_ERR(f) && f)
		fs_file_put(f);
	return err;
}

int do_execve(const char *path, char **argv, char **envp)
{
	struct exec_args args;
	struct exec_strings_acc argv_acc;
	struct exec_strings_acc env_acc;
	size_t total_size;
	char *empty_envp[1] = { NULL };
	char *argv_if_empty[2];

	if (!path || !argv)
		return -EFAULT;
	if (!envp)
		envp = empty_envp;

	total_size = 0;
	if (exec_accum_strings(argv, &total_size, &argv_acc))
		return -E2BIG;

	if (argv_acc.count == 0) {
		size_t plen = strlen(path) + 1;

		total_size = plen;
		if (total_size > ARG_MAX)
			return -E2BIG;
		argv_if_empty[0] = (char *)path;
		argv_if_empty[1] = NULL;
		args.argv = argv_if_empty;
		args.argc = 1;
		args.argv_size = plen;
	} else {
		args.argv = argv;
		args.argc = argv_acc.count;
		args.argv_size = argv_acc.bytes;
	}

	total_size += (size_t)(args.argc + 1) * sizeof(char *);
	if (total_size > ARG_MAX)
		return -E2BIG;

	args.envp = envp;
	if (exec_accum_strings(envp, &total_size, &env_acc))
		return -E2BIG;

	args.envc = env_acc.count;
	args.envp_size = env_acc.bytes;
	total_size += (size_t)(env_acc.count + 1) * sizeof(char *);
	if (total_size > ARG_MAX)
		return -E2BIG;

	total_size += sizeof(int);
	if (total_size > ARG_MAX)
		return -E2BIG;

	return __do_execve(path, &args);
}
