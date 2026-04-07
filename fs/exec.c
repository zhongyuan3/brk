#include <aosd/align.h>
#include <aosd/asm.h>
#include <aosd/assert.h>
#include <aosd/elf.h>
#include <aosd/errno.h>
#include <aosd/fcntl.h>
#include <aosd/fs.h>
#include <aosd/limits.h>
#include <aosd/list.h>
#include <aosd/mm.h>
#include <aosd/mm_types.h>
#include <aosd/pgalloc.h>
#include <aosd/pgtable.h>
#include <aosd/printk.h>
#include <aosd/process.h>
#include <aosd/riscv.h>
#include <aosd/slab.h>
#include <aosd/string.h>
#include <aosd/timer.h>
#include <aosd/vmalloc.h>

struct execve_args {
	char **argv;
	char **envp;
	size_t argv_size;
	size_t envp_size;
	int argc;
	int envc;
};

static uint64_t random_ustack(void)
{
	uint64_t ustack_top = USER_SPACE_SIZE_MAX;
	timer_srand();
	ustack_top -= (timer_rand() % 32 + 1) * PAGE_SIZE;
	ustack_top -= USTACK_SIZE;
	return ustack_top;
}

static void push_args(struct execve_args *args, uint64_t *psp,
		      uint64_t stack_virt, struct mm_struct *mm,
		      struct process *proc)
{
	uint64_t n;
	uint64_t sp = *psp;
	uint64_t ksp = stack_virt + USTACK_SIZE;

	ksp -= args->envp_size;
	sp -= args->envp_size;
	uint64_t envp_strs_kstart = ksp;
	uint64_t envp_strs_ustart = sp;

	ksp -= args->argv_size;
	sp -= args->argv_size;
	uint64_t argv_strs_kstart = ksp;
	uint64_t argv_strs_ustart = sp;

	n = (args->envc + 1) * sizeof(char *);
	ksp -= n;
	ksp = align_down(ksp, sizeof(char *));
	sp -= n;
	sp = align_down(sp, sizeof(char *));
	proc->tf.a2 = sp;
	char **envp_kstart = (char **)ksp;

	n = (args->argc + 1) * sizeof(uint64_t);
	ksp -= n;
	ksp = align_down(ksp, sizeof(char *));
	sp -= n;
	sp = align_down(sp, sizeof(char *));
	proc->tf.a1 = sp;
	char **argv_kstart = (char **)ksp;

	for (int i = 0; i < args->envc; ++i) {
		n = strlen(args->envp[i]) + 1;
		memcpy((char *)envp_strs_kstart, args->envp[i], n);
		envp_kstart[i] = (char *)envp_strs_ustart;
		envp_strs_ustart += n;
		envp_strs_kstart += n;
	}

	for (int i = 0; i < args->argc; ++i) {
		n = strlen(args->argv[i]) + 1;
		memcpy((char *)argv_strs_kstart, args->argv[i], n);
		argv_kstart[i] = (char *)argv_strs_ustart;
		argv_strs_ustart += n;
		argv_strs_kstart += n;
	}

	ksp -= sizeof(int);
	sp -= sizeof(int);
	*(int *)ksp = args->argc;

	ksp = align_down(ksp, 16);
	sp = align_down(sp, 16);

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

static int map_seg(struct mm_struct *mm, struct vm_area *vma,
		   struct elf64_phdr *ph, struct file *fp)
{
	int err = 0;

	size_t npgs = vma->size >> PAGE_SHIFT;
	struct page **pgs = kcalloc(npgs, sizeof(struct page *));
	if (!pgs)
		return -ENOMEM;

	uint64_t off = ph->p_offset;
	uint64_t filesz = ph->p_filesz;
	size_t i = 0;
	uint64_t addr = vma->addr;
	unsigned int flags = vma->flags;
	while (i < npgs) {
		struct page *pg = page_alloc(0);
		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		uint64_t pa = page_to_phys(pg);
		void *va = (void *)phys_to_virt(pa);
		if (filesz > 0) {
			size_t rsz = filesz > PAGE_SIZE ? PAGE_SIZE : filesz;
			size_t rcnt = 0;
			off_t ret = file_seek(fp, off, SEEK_SET);
			if (ret < 0) {
				assert(pg);
				page_free(pg, 0);
				goto failed;
			}
			err = file_read(fp, va, rsz, &rcnt);
			if (err) {
				assert(pg);
				page_free(pg, 0);
				goto failed;
			}
			if (rcnt != rsz) {
				err = -EIO;
				assert(pg);
				page_free(pg, 0);
				goto failed;
			}
			filesz -= rsz;
			off += rsz;
		} else {
			memset(va, 0, PAGE_SIZE);
		}
		err = uvmap(mm->pgd, addr, PAGE_SIZE, pa, flags);
		if (err) {
			assert(pg);
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
	for (uint64_t a = vma->addr; a < addr; a += PAGE_SIZE)
		uvunmap(mm->pgd, a, PAGE_SIZE);
	for (size_t j = 0; j < i; ++j) {
		assert(pgs[j]);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

static int load_seg(struct mm_struct *mm, struct elf64_phdr *ph,
		    struct file *fp)
{
	uint64_t start, end;
	struct vm_area *vma;
	int err;

	start = align_down(ph->p_vaddr, PAGE_SIZE);
	end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
	if (end == start)
		return 0;

	vma = vm_area_alloc();
	if (!vma)
		return -ENOMEM;
	vma->addr = start;
	vma->size = end - start;
	vma->flags = flags_to_perm(ph->p_flags);

	err = map_seg(mm, vma, ph, fp);
	if (err) {
		vm_area_free(vma);
		return err;
	}

	list_add(&vma->list, &mm->seg);
	return 0;
}

static uint64_t find_brk(struct mm_struct *mm)
{
	struct vm_area *vma = NULL;
	uint64_t brk = 0;
	list_for_each_entry(vma, &mm->seg, list) {
		uint64_t end = vma->addr + vma->size;
		if (end >= brk)
			brk = end;
	}
	return brk;
}

static int map_stack(struct mm_struct *mm)
{
	struct page *pg = page_alloc(USTACK_PAGE_ORDER);
	if (!pg)
		return -ENOMEM;

	struct page **pgs = kcalloc(1, sizeof(struct page *));
	if (!pgs) {
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	uint64_t pa = page_to_phys(pg);
	uint64_t base = random_ustack();
	int err = uvmap(mm->pgd, base, USTACK_SIZE, pa, PTE_R | PTE_W);
	if (err) {
		kfree(pgs);
		assert(pg);
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

static int __do_execve(const char *path, struct execve_args *args)
{
	struct elf64_hdr elf_hdr = { 0 };
	struct elf64_phdr phdr = { 0 };
	struct process *proc = current_process();
	struct file *f = NULL;
	struct mm_struct *new_mm = NULL;
	int err = 0;

	err = do_openat(AT_FDCWD, path, O_RDONLY, 0, &f);
	if (err)
		goto err;

	size_t r = 0;
	err = file_read(f, &elf_hdr, sizeof(elf_hdr), &r);
	if (err)
		goto err;

	if (r != sizeof(struct elf64_hdr)) {
		err = -ENOEXEC;
		goto err;
	}

	if (memcmp(elf_hdr.e_ident, ELFMAG, SELFMAG) != 0) {
		err = -ENOEXEC;
		goto err;
	}

	if (elf_hdr.e_type != ET_EXEC) {
		err = -ENOEXEC;
		goto err;
	}

	new_mm = mm_alloc();
	if (!new_mm) {
		err = -ENOMEM;
		goto err;
	}

	uint16_t i = 0;
	uint64_t off = elf_hdr.e_phoff;
	for (; i < elf_hdr.e_phnum; ++i, off += sizeof(phdr)) {
		off_t ret = file_seek(f, off, SEEK_SET);
		if (ret < 0)
			goto err;
		err = file_read(f, &phdr, sizeof(phdr), &r);
		if (err)
			goto err;
		if (r != sizeof(phdr))
			goto err;
		if (phdr.p_type != PT_LOAD)
			continue;
		err = load_seg(new_mm, &phdr, f);
		if (err)
			goto err;
	}

	file_put(f);
	f = NULL;

	new_mm->heap->addr = find_brk(new_mm);
	new_mm->heap->size = 0;
	new_mm->heap->flags = PTE_R | PTE_W;
	new_mm->brk = new_mm->heap->addr;

	err = map_stack(new_mm);
	if (err)
		goto err;

	uint64_t sp = new_mm->stack->addr + new_mm->stack->size;
	struct page *stack_pg = new_mm->stack->pages[0];
	uint64_t stack_phys = page_to_phys(stack_pg);
	uint64_t stack_virt = phys_to_virt(stack_phys);
	push_args(args, &sp, stack_virt, new_mm, proc);

	strlcpy(proc->name, args->argv[0], sizeof(proc->name));

	switch_pgtable(new_mm->pgd);

	struct mm_struct *old_mm = proc->mm;
	proc->mm = new_mm;
	mm_free(old_mm);
	proc->tf.epc = elf_hdr.e_entry;
	proc->tf.sp = sp;

	return args->argc;

err:
	if (new_mm)
		mm_free(new_mm);
	if (f)
		file_put(f);
	return err;
}

int do_execve(char *path, char **argv, char **envp)
{
	struct execve_args args;
	int cnt;
	size_t size;
	size_t len;
	size_t total_size;

	args.argv = argv;
	args.envp = envp;
	total_size = 0;

	for (cnt = 0, size = 0; argv[cnt];) {
		len = strlen(argv[cnt]) + 1;
		size += len;
		++cnt;
		total_size += len;
		if (total_size > ARG_MAX)
			return -E2BIG;
	}
	args.argc = cnt;
	args.argv_size = size;
	total_size += (cnt + 1) * sizeof(char *);
	if (total_size > ARG_MAX)
		return -E2BIG;

	for (cnt = 0, size = 0; envp[cnt];) {
		len = strlen(envp[cnt]) + 1;
		size += len;
		++cnt;
		total_size += len;
		if (total_size > ARG_MAX)
			return -E2BIG;
	}
	args.envc = cnt;
	args.envp_size = size;
	total_size += (cnt + 1) * sizeof(char *);
	if (total_size > ARG_MAX)
		return -E2BIG;

	total_size += sizeof(int);
	if (total_size > ARG_MAX)
		return -E2BIG;

	return __do_execve(path, &args);
}
