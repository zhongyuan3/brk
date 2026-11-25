#include <arch/asm/page.h>
#include <arch/asm/vmlayout.h>
#include <arch/pgtable.h>
#include <brk/align.h>
#include <brk/assert.h>
#include <brk/error.h>
#include <brk/fs.h>
#include <brk/kmalloc.h>
#include <brk/list.h>
#include <brk/mm.h>
#include <brk/mm_types.h>
#include <brk/path.h>
#include <brk/permission.h>
#include <brk/pgalloc.h>
#include <brk/printk.h>
#include <brk/rand.h>
#include <brk/signal.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/task.h>
#include <brk/timekeeper.h>
#include <brk/timer.h>
#include <brk/trap.h>
#include <brk/types.h>
#include <brk/vmalloc.h>
#include <uapi/auxv.h>
#include <uapi/capability.h>
#include <uapi/elf.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/limits.h>
#include <uapi/stat.h>

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

static int exec_read_exact(struct file *fp, u64 off, void *buf, size_t n)
{
	loff_t ret = file_lseek(fp, off, SEEK_SET);

	if (ret < 0) {
		pr_err("%s(): Failed to lseek file: err=%d\n", __func__,
		       (int)ret);
		return -EIO;
	}

	ssize_t rcnt = file_read(fp, buf, n);

	if (rcnt < 0) {
		pr_err("%s(): Failed to read file: err=%d\n", __func__,
		       (int)rcnt);
		return -EIO;
	}
	if ((size_t)rcnt != n) {
		pr_err("%s(): Failed to read file (short read)\n", __func__);
		return -EIO;
	}
	return 0;
}

#define EXEC_MAX_PHNUM 256
#define EXEC_MAX_DYNSZ 4096
#define EXEC_MAX_RELASZ (1 << 20)

static int elf_validate_exec_hdr(const struct __brk_elf_hdr *h)
{
	if (memcmp(h->e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;
	if (h->e_ident[EI_CLASS] != ELFCLASS64)
		return -ENOEXEC;
	if (h->e_ident[EI_DATA] != ELFDATA2LSB)
		return -ENOEXEC;
	if (h->e_ident[EI_VERSION] != EV_CURRENT)
		return -ENOEXEC;
	if (h->e_version != EV_CURRENT)
		return -ENOEXEC;
	if (h->e_type != ET_EXEC && h->e_type != ET_DYN)
		return -ENOEXEC;
	if (h->e_machine != EM_RISCV)
		return -ENOEXEC;
	if (h->e_phentsize != sizeof(struct __brk_elf_phdr))
		return -ENOEXEC;
	if (h->e_phnum == 0 || h->e_phnum > EXEC_MAX_PHNUM)
		return -ENOEXEC;
	/*
	 * The trap path restores full hardware FP state for the new
	 * image; a soft-float binary would misinterpret it.
	 */
	if ((h->e_flags & EF_RISCV_FLOAT_ABI_MASK) == EF_RISCV_FLOAT_ABI_SOFT)
		return -ENOEXEC;
	return 0;
}

static int exec_read_elf_header(struct file *f, struct __brk_elf_hdr *elf_hdr)
{
	ssize_t r = file_read(f, elf_hdr, sizeof(*elf_hdr));

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

static user_addr_t random_ustack(void)
{
	user_addr_t ustack_top = USER_SPACE_SIZE_MAX;

	rand_seed(ktime_get_mono_ns());
	ustack_top -= (rand_u32() % 32 + 1) * PAGE_SIZE;
	ustack_top -= USTACK_SIZE;
	return ustack_top;
}

static user_addr_t stack_push_ptr_slots(virt_addr_t *ksp, user_addr_t *sp,
					int nptr)
{
	size_t n = (size_t)(nptr + 1) * sizeof(char *);

	*ksp -= n;
	*ksp = align_down(*ksp, sizeof(char *));
	*sp -= n;
	*sp = align_down(*sp, sizeof(char *));
	return *sp;
}

static void copy_exec_strings(char **src, int n, virt_addr_t *k_pos,
			      user_addr_t *u_pos, char **dst_ptr_array)
{
	for (int i = 0; i < n; ++i) {
		size_t l = strlen(src[i]) + 1;

		memcpy((char *)*k_pos, src[i], l);
		dst_ptr_array[i] = (char *)*u_pos;
		*u_pos += l;
		*k_pos += l;
	}
}

struct exec_stack_info {
	user_addr_t entry;
	user_addr_t at_base;
	kuid_t uid;
	kuid_t euid;
	kgid_t gid;
	kgid_t egid;
	bool secure;
	const struct __brk_elf_phdr *phdrs;
	u16 phnum;
	user_addr_t phdr_vaddr; /* runtime PT_PHDR address, 0 = copy */
};

static user_addr_t stack_reserve(virt_addr_t *ksp, user_addr_t *sp, size_t n,
				 size_t align)
{
	*ksp = align_down(*ksp - n, align);
	*sp = align_down(*sp - n, align);
	return *sp;
}

static void push_args(struct exec_args *args, user_addr_t *psp,
		      virt_addr_t stack_virt,
		      const struct exec_stack_info *info, user_addr_t *pargv,
		      user_addr_t *penvp)
{
	user_addr_t sp = *psp;
	virt_addr_t ksp = stack_virt + USTACK_SIZE;
	struct {
		user_addr_t key;
		user_addr_t val;
	} auxv[16];
	int naux = 0;
	user_addr_t rand_uaddr = 0;
	user_addr_t phdr_uaddr = 0;

	ksp -= args->envp_size;
	sp -= args->envp_size;
	virt_addr_t envp_strs_kstart = ksp;
	user_addr_t envp_strs_ustart = sp;

	ksp -= args->argv_size;
	sp -= args->argv_size;
	virt_addr_t argv_strs_kstart = ksp;
	user_addr_t argv_strs_ustart = sp;

	/*
	 * Between the strings and the pointer arrays: the auxv blob
	 * (random bytes and, when the image carries no PT_PHDR, a
	 * copy of the program header table).
	 */
	if (!info->phdr_vaddr) {
		size_t phsz =
			(size_t)info->phnum * sizeof(struct __brk_elf_phdr);

		phdr_uaddr = stack_reserve(&ksp, &sp, phsz, 8);
		memcpy((void *)ksp, info->phdrs, phsz);
	}

	rand_uaddr = stack_reserve(&ksp, &sp, 16, 16);
	for (int i = 0; i < 4; ++i)
		((u32 *)(uintptr_t)ksp)[i] = rand_u32();

	auxv[naux].key = AT_PHDR;
	auxv[naux++].val = info->phdr_vaddr ? info->phdr_vaddr : phdr_uaddr;
	auxv[naux].key = AT_PHENT;
	auxv[naux++].val = sizeof(struct __brk_elf_phdr);
	auxv[naux].key = AT_PHNUM;
	auxv[naux++].val = info->phnum;
	auxv[naux].key = AT_PAGESZ;
	auxv[naux++].val = PAGE_SIZE;
	auxv[naux].key = AT_BASE;
	auxv[naux++].val = info->at_base;
	auxv[naux].key = AT_FLAGS;
	auxv[naux++].val = 0;
	auxv[naux].key = AT_ENTRY;
	auxv[naux++].val = info->entry;
	auxv[naux].key = AT_UID;
	auxv[naux++].val = info->uid;
	auxv[naux].key = AT_EUID;
	auxv[naux++].val = info->euid;
	auxv[naux].key = AT_GID;
	auxv[naux++].val = info->gid;
	auxv[naux].key = AT_EGID;
	auxv[naux++].val = info->egid;
	auxv[naux].key = AT_SECURE;
	auxv[naux++].val = info->secure ? 1 : 0;
	auxv[naux].key = AT_RANDOM;
	auxv[naux++].val = rand_uaddr;
	auxv[naux].key = AT_EXECFN;
	auxv[naux++].val = argv_strs_ustart;
	auxv[naux].key = AT_NULL;
	auxv[naux++].val = 0;
	(void)stack_reserve(&ksp, &sp, (size_t)naux * 2 * sizeof(void *), 8);
	memcpy((void *)ksp, auxv, (size_t)naux * sizeof(auxv[0]));

	user_addr_t envp_uaddr = stack_push_ptr_slots(&ksp, &sp, args->envc);
	char **envp_kstart = (char **)ksp;

	user_addr_t argv_uaddr = stack_push_ptr_slots(&ksp, &sp, args->argc);
	char **argv_kstart = (char **)ksp;

	copy_exec_strings(args->envp, args->envc, &envp_strs_kstart,
			  &envp_strs_ustart, envp_kstart);
	copy_exec_strings(args->argv, args->argc, &argv_strs_kstart,
			  &argv_strs_ustart, argv_kstart);

	argv_kstart[args->argc] = NULL;
	envp_kstart[args->envc] = NULL;

	*pargv = argv_uaddr;
	*penvp = envp_uaddr;

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

static int map_seg(struct uvm_space *mm, struct uvm_region *vma,
		   struct __brk_elf_phdr *ph, struct file *fp)
{
	int err = 0;
	size_t npgs = vma->size >> PAGE_SHIFT;
	struct page **pgs = kcalloc(npgs, sizeof(struct page *));
	size_t i = 0;
	user_addr_t addr = vma->addr;
	unsigned int flags = vma->flags;
	user_addr_t seg_vstart = ph->p_vaddr;
	user_addr_t seg_fend = ph->p_vaddr + ph->p_filesz;

	if (!pgs)
		return -ENOMEM;

	while (i < npgs) {
		struct page *pg = page_alloc(0);

		if (!pg) {
			err = -ENOMEM;
			goto failed;
		}
		phys_addr_t pa = page_to_phys(pg);
		void *va = (void *)(uintptr_t)phys_to_virt(pa);
		user_addr_t page_lo = addr;
		user_addr_t page_hi = addr + PAGE_SIZE;
		user_addr_t file_lo = page_lo > seg_vstart ? page_lo :
							     seg_vstart;
		user_addr_t file_hi = page_hi < seg_fend ? page_hi : seg_fend;

		memset(va, 0, PAGE_SIZE);
		if (file_lo < file_hi) {
			size_t n = (size_t)(file_hi - file_lo);
			u64 fo = ph->p_offset + (file_lo - seg_vstart);
			size_t dst_off = (size_t)(file_lo - page_lo);

			err = exec_read_exact(fp, fo, (char *)va + dst_off, n);
			if (err) {
				assert(pg);
				page_free(pg, 0);
				goto failed;
			}
		}
		err = map_user_pages(mm->pgd, addr, PAGE_SIZE, pa, flags);
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
	for (user_addr_t a = vma->addr; a < addr; a += PAGE_SIZE)
		unmap_user_pages(mm->pgd, a, PAGE_SIZE);
	for (size_t j = 0; j < i; ++j) {
		assert(pgs[j]);
		page_free(pgs[j], 0);
	}
	kfree(pgs);
	return err;
}

/*
 * PIE images (ET_DYN) are placed at a random page-aligned base well
 * below the randomized stack window: the brk heap grows above the
 * image and the mmap area fills the space in between.  The main
 * image and the interpreter draw from disjoint zones so two images
 * never overlap without any cross-image bookkeeping.
 */
#define PIE_ZONE_LO 0x40000000UL /* 1 GB */
#define PIE_ZONE_PAGES ((7UL << 30) >> PAGE_SHIFT)

static user_addr_t pick_pie_base(user_addr_t zone_lo)
{
	return zone_lo +
	       ((user_addr_t)(rand_u32() % PIE_ZONE_PAGES) << PAGE_SHIFT);
}

/*
 * Write into a just-mapped user page through the kernel direct map.
 * Only valid before switch_pgtable(): the pages are still owned by
 * the fresh address space under construction.
 */
static int exec_write_u64(struct uvm_space *mm, user_addr_t addr, u64 val)
{
	struct uvm_region *vma;

	list_for_each_entry(vma, &mm->seg, list) {
		user_addr_t off;

		if (addr < vma->addr || addr + 8 > vma->addr + vma->size)
			continue;
		off = addr - vma->addr;
		if ((off & (PAGE_SIZE - 1)) + 8 > PAGE_SIZE)
			return -ENOEXEC;
		if (!vma->pages || (off >> PAGE_SHIFT) >= vma->nr_pages)
			return -ENOEXEC;
		*(u64 *)((char *)phys_to_virt(
				 page_to_phys(vma->pages[off >> PAGE_SHIFT])) +
			 (off & (PAGE_SIZE - 1))) = val;
		return 0;
	}
	return -ENOEXEC;
}

/*
 * Apply the base relocations of a static PIE: every R_RISCV_RELATIVE
 * slot becomes base + addend.  This is deliberately the only dynamic
 * entry the kernel understands - no symbol resolution, no DT_NEEDED,
 * no interpreter.  Anything beyond RELATIVE in a kernel-loaded image
 * is refused.
 */
static u64 dyn_vaddr_to_off(const struct __brk_elf_phdr **loads, u32 nload,
			    user_addr_t vaddr)
{
	for (u32 i = 0; i < nload; ++i) {
		if (vaddr >= loads[i]->p_vaddr &&
		    vaddr < loads[i]->p_vaddr + loads[i]->p_memsz)
			return loads[i]->p_offset + (vaddr - loads[i]->p_vaddr);
	}
	return 0;
}

static int apply_base_relocs(struct uvm_space *mm, struct file *f,
			     const struct __brk_elf_phdr *dyn_ph,
			     const struct __brk_elf_phdr **loads, u32 nload,
			     user_addr_t base, bool strict)
{
	struct __brk_elf_dyn *dyn;
	size_t ndyn;
	user_addr_t rela = 0;
	size_t relasz = 0;
	size_t relaent = 0;
	bool has_rel = false;
	struct __brk_elf_rela *rela_tab;
	u64 rela_off;
	int err;
	size_t i;

	if (dyn_ph->p_filesz == 0 || dyn_ph->p_filesz > EXEC_MAX_DYNSZ ||
	    dyn_ph->p_filesz % sizeof(*dyn) != 0)
		return -ENOEXEC;
	ndyn = dyn_ph->p_filesz / sizeof(*dyn);
	dyn = kmalloc(dyn_ph->p_filesz);
	if (!dyn)
		return -ENOMEM;

	err = exec_read_exact(f, dyn_ph->p_offset, dyn, dyn_ph->p_filesz);
	if (err) {
		kfree(dyn);
		return err;
	}

	for (i = 0; i < ndyn; ++i) {
		switch ((u32)dyn[i].d_tag) {
		case DT_RELA:
			rela = (user_addr_t)dyn[i].d_un.d_ptr;
			break;
		case DT_RELASZ:
			relasz = (size_t)dyn[i].d_un.d_val;
			break;
		case DT_RELAENT:
			relaent = (size_t)dyn[i].d_un.d_val;
			break;
		case DT_REL:
			has_rel = true;
			break;
		case DT_NULL:
			goto dyn_done;
		}
	}
dyn_done:
	kfree(dyn);

	if (has_rel)
		return -ENOEXEC; /* RISC-V is RELA-only */
	if (!rela)
		return 0;
	if (relaent != sizeof(*rela_tab))
		return -ENOEXEC;
	if (relasz == 0 || relasz % relaent != 0 || relasz > EXEC_MAX_RELASZ)
		return -ENOEXEC;

	rela_off = dyn_vaddr_to_off(loads, nload, rela);
	if (!rela_off)
		return -ENOEXEC;

	rela_tab = kmalloc(relasz);
	if (!rela_tab)
		return -ENOMEM;
	err = exec_read_exact(f, rela_off, rela_tab, relasz);
	if (err) {
		kfree(rela_tab);
		return err;
	}

	for (i = 0; i < relasz / relaent; ++i) {
		const struct __brk_elf_rela *r = &rela_tab[i];
		u32 type = (u32)(r->r_info & 0xff);

		if (type == R_RISCV_NONE || type == R_RISCV_RELAX)
			continue;
		if (type != R_RISCV_RELATIVE) {
			/*
			 * Symbol relocations in an image loaded directly
			 * by the kernel are fatal - unless a dynamic
			 * linker is on its way to process them.
			 */
			if (strict) {
				kfree(rela_tab);
				return -ENOEXEC;
			}
			continue;
		}
		err = exec_write_u64(mm, base + r->r_offset,
				     base + (u64)r->r_addend);
		if (err) {
			kfree(rela_tab);
			return err;
		}
	}
	kfree(rela_tab);
	return 0;
}

static int load_seg(struct uvm_space *mm, struct __brk_elf_phdr *ph,
		    struct file *fp)
{
	user_addr_t start, end;
	struct uvm_region *vma;
	int err;

	start = align_down(ph->p_vaddr, PAGE_SIZE);
	end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
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

static user_addr_t find_brk(struct uvm_space *mm)
{
	struct uvm_region *vma = NULL;
	user_addr_t brk = 0;

	list_for_each_entry(vma, &mm->seg, list) {
		user_addr_t end = vma->addr + vma->size;

		if (end >= brk)
			brk = end;
	}
	return brk;
}

static int map_stack(struct uvm_space *mm, bool want_x)
{
	struct page *pg = page_alloc(USTACK_PAGE_ORDER);
	struct page **pgs;
	phys_addr_t pa;
	user_addr_t base;
	int err;

	if (!pg)
		return -ENOMEM;

	pgs = kcalloc(1, sizeof(struct page *));
	if (!pgs) {
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return -ENOMEM;
	}

	pa = page_to_phys(pg);
	unsigned int stack_perm = PTE_R | PTE_W | (want_x ? PTE_X : 0);

	base = random_ustack();
	err = map_user_pages(mm->pgd, base, USTACK_SIZE, pa, stack_perm);
	if (err) {
		kfree(pgs);
		assert(pg);
		page_free(pg, USTACK_PAGE_ORDER);
		return err;
	}
	pgs[0] = pg;

	mm->stack->addr = base;
	mm->stack->size = USTACK_SIZE;
	mm->stack->flags = stack_perm;
	mm->mmap_top = base;
	mm->stack->pages = pgs;
	mm->stack->nr_pages = 1;
	return 0;
}

/*
 * Validate and map one whole ELF image into @mm: classification,
 * bounds/overlap checks, random PIE base, entry check and kernel
 * base relocations.  @is_interp marks the interpreter (no nested
 * PT_INTERP); @has_interp means a dynamic linker will process the
 * symbol relocations, so only the strict mode rejects them.
 */
struct image_layout {
	user_addr_t base;
	user_addr_t entry;
	user_addr_t phdr_vaddr;
	bool stack_x;
};

static int elf_load_image(struct uvm_space *mm, struct file *f,
			  const struct __brk_elf_hdr *hdr,
			  const struct __brk_elf_phdr *phdrs, bool is_interp,
			  bool has_interp, user_addr_t zone_lo,
			  struct image_layout *out)
{
	const struct __brk_elf_phdr *loads[EXEC_MAX_PHNUM];
	const struct __brk_elf_phdr *phdr_seg = NULL;
	const struct __brk_elf_phdr *dyn_seg = NULL;
	bool stack_x = false;
	loff_t file_size = f->inode->size;
	u16 phnum = hdr->e_phnum;
	user_addr_t pie_base = 0;
	user_addr_t phdr_vaddr = 0;
	user_addr_t entry;
	u32 nload = 0;
	u32 i, j;
	int err;

	for (i = 0; i < phnum; ++i) {
		const struct __brk_elf_phdr *ph = &phdrs[i];

		switch (ph->p_type) {
		case PT_LOAD:
			if (ph->p_filesz > ph->p_memsz ||
			    ph->p_offset > (u64)file_size ||
			    ph->p_filesz > (u64)file_size - ph->p_offset ||
			    ph->p_memsz > USER_SPACE_SIZE_MAX) {
				err = -ENOEXEC;
				goto err;
			}
			if (ph->p_align >= PAGE_SIZE &&
			    ph->p_vaddr % ph->p_align !=
				    ph->p_offset % ph->p_align) {
				err = -ENOEXEC;
				goto err;
			}
			loads[nload++] = ph;
			break;
		case PT_PHDR:
			phdr_seg = ph;
			break;
		case PT_DYNAMIC:
			if (hdr->e_type != ET_DYN) {
				err = -ENOEXEC;
				goto err;
			}
			dyn_seg = ph;
			break;
		case PT_GNU_STACK:
			stack_x = !!(ph->p_flags & PF_X);
			break;
		case PT_INTERP:
			if (is_interp) {
				err = -ENOEXEC;
				goto err;
			}
			break;
		case PT_TLS:
			/* thread-local storage is not implemented */
			err = -ENOEXEC;
			goto err;
		default:
			/* PT_NOTE and friends are informational */
			break;
		}
	}

	if (nload == 0) {
		err = -ENOEXEC;
		goto err;
	}

	/* loads in vaddr order, pairwise non-overlapping */
	for (i = 1; i < nload; ++i) {
		const struct __brk_elf_phdr *key = loads[i];

		for (j = i; j > 0 && loads[j - 1]->p_vaddr > key->p_vaddr; --j)
			loads[j] = loads[j - 1];
		loads[j] = key;
	}
	for (i = 1; i < nload; ++i) {
		user_addr_t prev_end =
			align_up(loads[i - 1]->p_vaddr + loads[i - 1]->p_memsz,
				 PAGE_SIZE);
		user_addr_t cur_start =
			align_down(loads[i]->p_vaddr, PAGE_SIZE);

		if (cur_start < prev_end) {
			err = -ENOEXEC;
			goto err;
		}
	}

	if (hdr->e_type == ET_DYN)
		pie_base = pick_pie_base(zone_lo);

	if (pie_base + align_up(loads[nload - 1]->p_vaddr +
					loads[nload - 1]->p_memsz,
				PAGE_SIZE) >
	    USER_SPACE_SIZE_MAX) {
		err = -ENOEXEC;
		goto err;
	}

	for (i = 0; i < nload; ++i) {
		struct __brk_elf_phdr adj = *loads[i];

		adj.p_vaddr += pie_base;
		err = load_seg(mm, &adj, f);
		if (err)
			goto err;
	}

	entry = pie_base + hdr->e_entry;
	for (i = 0; i < nload; ++i) {
		if ((loads[i]->p_flags & PF_X) &&
		    entry >= pie_base + loads[i]->p_vaddr &&
		    entry < pie_base + loads[i]->p_vaddr + loads[i]->p_memsz)
			break;
	}
	if (i == nload) {
		err = -ENOEXEC;
		goto err;
	}

	if (dyn_seg) {
		err = apply_base_relocs(mm, f, dyn_seg, loads, nload, pie_base,
					!has_interp);
		if (err)
			goto err;
	}

	/*
 * AT_PHDR: point into the mapped image when the linker
 * emitted PT_PHDR, otherwise push_args copies the
 * table onto the stack.
 */
	if (phdr_seg) {
		user_addr_t at = pie_base + phdr_seg->p_vaddr;
		bool covered = false;

		for (i = 0; i < nload; ++i) {
			if (at >= pie_base + loads[i]->p_vaddr &&
			    at + (u64)phnum * sizeof(*phdrs) <=
				    pie_base + loads[i]->p_vaddr +
					    loads[i]->p_memsz) {
				covered = true;
				break;
			}
		}
		phdr_vaddr = covered ? at : 0;
	}

	out->base = pie_base;
	out->entry = entry;
	out->phdr_vaddr = phdr_vaddr;
	out->stack_x = stack_x;
	return 0;

err:
	return err;
}

/*
 * Open and pre-read the ELF interpreter named by PT_INTERP.  Any
 * failure to produce a loadable interpreter turns the exec into
 * ENOEXEC: a dynamic binary without its linker is not executable.
 */
static int exec_load_interp(const struct __brk_elf_phdr *ph,
			    struct file *main_f, struct file **f_out,
			    struct __brk_elf_hdr *hdr_out,
			    struct __brk_elf_phdr **phdrs_out)
{
	char *path;
	struct file *fi;
	int err;

	if (ph->p_filesz == 0 || ph->p_filesz > 512)
		return -ENOEXEC;
	path = kmalloc(ph->p_filesz + 1);
	if (!path)
		return -ENOMEM;
	err = exec_read_exact(main_f, ph->p_offset, path, ph->p_filesz);
	if (err) {
		kfree(path);
		return err;
	}
	path[ph->p_filesz] = '\0';
	if (!memchr(path, '\0', ph->p_filesz)) {
		kfree(path);
		return -ENOEXEC;
	}

	fi = do_openat(AT_FDCWD, path, O_RDONLY, 0);
	kfree(path);
	if (IS_ERR(fi))
		return -ENOEXEC;
	err = inode_permission(fi->inode, MAY_EXEC);
	if (err) {
		file_put(fi);
		return -ENOEXEC;
	}
	err = exec_read_elf_header(fi, hdr_out);
	if (err) {
		file_put(fi);
		return err;
	}
	*phdrs_out = kmalloc((size_t)hdr_out->e_phnum * sizeof(**phdrs_out));
	if (!*phdrs_out) {
		file_put(fi);
		return -ENOMEM;
	}
	err = exec_read_exact(fi, hdr_out->e_phoff, *phdrs_out,
			      (size_t)hdr_out->e_phnum * sizeof(**phdrs_out));
	if (err) {
		kfree(*phdrs_out);
		*phdrs_out = NULL;
		file_put(fi);
		return err;
	}
	*f_out = fi;
	return 0;
}

static int __do_execve(const char *path, struct exec_args *args)
{
	struct __brk_elf_hdr elf_hdr = { 0 };
	struct __brk_elf_hdr interp_hdr = { 0 };
	struct __brk_elf_phdr *phdrs = NULL;
	struct __brk_elf_phdr *interp_phdrs = NULL;
	struct file *interp_f = NULL;
	struct exec_stack_info exec_info = { 0 };
	user_addr_t run_entry = 0;
	bool stack_want_x = false;
	struct process *process = current_process();
	struct thread *task = current_thread();
	struct file *f = NULL;
	struct uvm_space *new_mm = NULL;
	umode_t file_mode;
	kuid_t file_uid;
	kgid_t file_gid;
	int err = 0;

	f = do_openat(AT_FDCWD, path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		err = PTR_ERR(f);
		goto err;
	}

	err = inode_permission(f->inode, MAY_EXEC);
	if (err)
		goto err;

	file_mode = f->inode->mode;
	file_uid = f->inode->uid;
	file_gid = f->inode->gid;

	err = exec_read_elf_header(f, &elf_hdr);
	if (err)
		goto err;

	phdrs = kmalloc((size_t)elf_hdr.e_phnum * sizeof(*phdrs));
	if (!phdrs) {
		err = -ENOMEM;
		goto err;
	}
	err = exec_read_exact(f, elf_hdr.e_phoff, phdrs,
			      (size_t)elf_hdr.e_phnum * sizeof(*phdrs));
	if (err)
		goto err;

	new_mm = uvm_space_create();
	if (IS_ERR(new_mm)) {
		err = PTR_ERR(new_mm);
		goto err;
	}

	/* first scan: is there a dynamic linker to hand control to? */
	for (u16 i = 0; i < elf_hdr.e_phnum; ++i) {
		if (phdrs[i].p_type == PT_INTERP) {
			err = exec_load_interp(&phdrs[i], f, &interp_f,
					       &interp_hdr, &interp_phdrs);
			if (err)
				goto err;
			break;
		}
	}

	new_mm = uvm_space_create();
	if (IS_ERR(new_mm)) {
		err = PTR_ERR(new_mm);
		new_mm = NULL;
		goto err;
	}

	if (interp_f) {
		struct image_layout li = { 0 };

		err = elf_load_image(new_mm, interp_f, &interp_hdr,
				     interp_phdrs, true, true, PIE_ZONE_LO,
				     &li);
		if (err)
			goto err;
		exec_info.at_base = li.base;
		run_entry = li.entry;
	}

	{
		struct image_layout lm = { 0 };

		err = elf_load_image(
			new_mm, f, &elf_hdr, phdrs, false, interp_f != NULL,
			interp_f ? PIE_ZONE_LO + (15UL << 30) : PIE_ZONE_LO,
			&lm);
		if (err)
			goto err;
		exec_info.entry = lm.entry;
		exec_info.phdrs = phdrs;
		exec_info.phnum = elf_hdr.e_phnum;
		exec_info.phdr_vaddr = lm.phdr_vaddr;
		stack_want_x = lm.stack_x;
		if (!interp_f)
			run_entry = lm.entry;
	}

	file_put(f);
	f = NULL;
	if (interp_f) {
		file_put(interp_f);
		interp_f = NULL;
	}
	kfree(interp_phdrs);
	interp_phdrs = NULL;

	new_mm->heap->addr = find_brk(new_mm);
	new_mm->heap->size = 0;
	new_mm->heap->flags = PTE_R | PTE_W;
	new_mm->brk = new_mm->heap->addr;

	err = map_stack(new_mm, stack_want_x);
	if (err)
		goto err;

	{
		irqstate_t cflags = spin_lock_irqsave(&process->cred_lock);

		exec_info.uid = process->cred.uid;
		exec_info.euid = process->cred.euid;
		exec_info.gid = process->cred.gid;
		exec_info.egid = process->cred.egid;
		if (file_mode & S_ISUID) {
			exec_info.euid = file_uid;
			exec_info.secure = true;
		}
		if ((file_mode & S_ISGID) && (file_mode & S_IXGRP)) {
			exec_info.egid = file_gid;
			exec_info.secure = true;
		}
		spin_unlock_irqrestore(&process->cred_lock, cflags);
	}

	{
		user_addr_t sp = new_mm->stack->addr + new_mm->stack->size;
		user_addr_t argv_uaddr, envp_uaddr;
		struct page *stack_pg = new_mm->stack->pages[0];
		phys_addr_t stack_phys = page_to_phys(stack_pg);
		virt_addr_t stack_virt = phys_to_virt(stack_phys);

		push_args(args, &sp, stack_virt, &exec_info, &argv_uaddr,
			  &envp_uaddr);
		strlcpy(process->name, args->argv[0], sizeof(process->name));

		switch_pgtable(new_mm->pgd);

		struct uvm_space *old_mm = process->mm;

		process->mm = new_mm;
		uvm_space_free(old_mm);
		signal_reset(process, task);
		arch_setup_user_entry(task->frame, run_entry, sp, args->argc,
				      argv_uaddr, envp_uaddr);
		arch_frame_reset_user_fp(task->frame);

		irqstate_t cred_flags = spin_lock_irqsave(&process->cred_lock);

		if (file_mode & S_ISUID) {
			process->cred.euid = file_uid;
			process->cred.suid = file_uid;
			process->cred.fsuid = file_uid;
			if (file_uid == 0) {
				process->cred.cap_effective = CAP_FULL_SET;
				process->cred.cap_permitted = CAP_FULL_SET;
			} else {
				process->cred.cap_effective = 0;
				process->cred.cap_permitted = 0;
			}
		}
		if ((file_mode & S_ISGID) && (file_mode & S_IXGRP)) {
			process->cred.egid = file_gid;
			process->cred.sgid = file_gid;
			process->cred.fsgid = file_gid;
		}
		spin_unlock_irqrestore(&process->cred_lock, cred_flags);
	}

	kfree(phdrs);
	/*
	 * Process-entry contract: the trap path writes this return
	 * value into a0, and BRK's crt reads argc from a0 (with argv
	 * in a1 and envp in a2) - _start's parameter list IS the
	 * register convention.  The full SysV stack (argc, argv,
	 * envp, auxv) is there as well for anything that prefers it.
	 */
	return args->argc;

err:
	kfree(phdrs);
	kfree(interp_phdrs);
	if (interp_f)
		file_put(interp_f);
	if (new_mm && !IS_ERR(new_mm))
		uvm_space_free(new_mm);
	if (!IS_ERR(f) && f)
		file_put(f);
	return err;
}

static int do_execve_common(const char *path, char **argv, char **envp)
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

int do_execve(const char *path, char **argv, char **envp)
{
	struct process *process = current_process();
	bool busy;
	int ret;
	irqstate_t flags;

	flags = spin_lock_irqsave(&process->lock);
	busy = process->nr_live != 1 || process->exec_in_progress;
	if (!busy)
		process->exec_in_progress = true;
	spin_unlock_irqrestore(&process->lock, flags);
	if (busy)
		return -EBUSY;

	ret = do_execve_common(path, argv, envp);

	flags = spin_lock_irqsave(&process->lock);
	process->exec_in_progress = false;
	spin_unlock_irqrestore(&process->lock, flags);
	return ret;
}
