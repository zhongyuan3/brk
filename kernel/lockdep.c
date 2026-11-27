#include <brk/cpu.h>
#include <brk/lockdep.h>
#include <brk/panic.h>
#include <brk/printf.h>
#include <brk/spinlock.h>
#include <brk/string.h>
#include <brk/thread.h>
#include <brk/types.h>
#include <stdint.h>

#ifndef NDEBUG

#define LOCKDEP_MAX_CLASSES 256
#define LOCKDEP_NAME_LEN 48
#define LOCKDEP_INVALID_CLASS ((u16) - 1)

struct lockdep_class {
	char name[LOCKDEP_NAME_LEN];
	bool nested;
};

#define LOCKDEP_CLASS_CACHE 512

static struct lockdep_class lockdep_classes[LOCKDEP_MAX_CLASSES];
static unsigned int lockdep_nr_classes;
static u8 lockdep_edges[LOCKDEP_MAX_CLASSES][LOCKDEP_MAX_CLASSES];
static const void *lockdep_edge_caller[LOCKDEP_MAX_CLASSES][LOCKDEP_MAX_CLASSES];
static struct {
	const char *name;
	u16 id;
} lockdep_class_cache[LOCKDEP_CLASS_CACHE];

static spinlock_t lockdep_graph_lock = SPINLOCK_INITIALIZER("lockdep_graph");
static bool lockdep_ready;
static bool lockdep_enabled;

static u16 lockdep_bfs_queue[LOCKDEP_MAX_CLASSES];
static u16 lockdep_bfs_parent[LOCKDEP_MAX_CLASSES];
static u8 lockdep_bfs_seen[LOCKDEP_MAX_CLASSES];

static const char *lockdep_mode_name(enum lockdep_mode mode)
{
	return mode == LOCKDEP_READ ? "read" : "write";
}

/*
 * Classes whose instances are intentionally held together.  Same-class
 * dependency edges are skipped for these; the callers impose their own
 * ordering (for example inode rwsems in rename/link are address ordered).
 */
static bool lockdep_class_is_nested(const char *name)
{
	static const char *const nested[] = {
		"inode.rwsem",
		"fsinfo",
		"cached_page.io_lock",
		"sigaction_table",
	};

	for (size_t i = 0; i < sizeof(nested) / sizeof(nested[0]); ++i) {
		if (!strcmp(name, nested[i]))
			return true;
	}
	return false;
}

static u16 lockdep_class_id(const char *name)
{
	unsigned int i;
	unsigned int slot;
	unsigned int probe;

	if (!name)
		name = "<unnamed>";

	/*
	 * Lock names are overwhelmingly static string literals, so a
	 * pointer-keyed cache avoids a strcmp walk over every class on the
	 * lock hot path.
	 */
	slot = ((uintptr_t)name >> 4) & (LOCKDEP_CLASS_CACHE - 1);
	for (probe = 0; probe < LOCKDEP_CLASS_CACHE; ++probe) {
		if (lockdep_class_cache[slot].name == name)
			return lockdep_class_cache[slot].id;
		if (!lockdep_class_cache[slot].name)
			break;
		slot = (slot + 1) & (LOCKDEP_CLASS_CACHE - 1);
	}

	for (i = 0; i < lockdep_nr_classes; ++i) {
		if (!strcmp(lockdep_classes[i].name, name))
			goto cache;
	}

	if (lockdep_nr_classes >= LOCKDEP_MAX_CLASSES)
		return LOCKDEP_INVALID_CLASS;

	strlcpy(lockdep_classes[i].name, name, LOCKDEP_NAME_LEN);
	lockdep_classes[i].nested = lockdep_class_is_nested(name);
	lockdep_nr_classes++;

cache:
	slot = ((uintptr_t)name >> 4) & (LOCKDEP_CLASS_CACHE - 1);
	for (probe = 0; probe < LOCKDEP_CLASS_CACHE; ++probe) {
		if (!lockdep_class_cache[slot].name ||
		    lockdep_class_cache[slot].name == name) {
			lockdep_class_cache[slot].name = name;
			lockdep_class_cache[slot].id = (u16)i;
			break;
		}
		slot = (slot + 1) & (LOCKDEP_CLASS_CACHE - 1);
	}

	return (u16)i;
}

static bool lockdep_reachable(u16 from, u16 to)
{
	unsigned int head = 0;
	unsigned int tail = 0;
	unsigned int i;

	if (from == to)
		return true;

	memset(lockdep_bfs_seen, 0, lockdep_nr_classes);
	lockdep_bfs_seen[from] = 1;
	lockdep_bfs_parent[from] = LOCKDEP_INVALID_CLASS;
	lockdep_bfs_queue[tail++] = from;

	while (head < tail) {
		u16 class = lockdep_bfs_queue[head++];

		for (i = 0; i < lockdep_nr_classes; ++i) {
			if (!lockdep_edges[class][i] || lockdep_bfs_seen[i])
				continue;
			lockdep_bfs_parent[i] = class;
			if (i == to)
				return true;
			lockdep_bfs_seen[i] = 1;
			lockdep_bfs_queue[tail++] = (u16)i;
		}
	}

	return false;
}

static bool lockdep_enter(irqstate_t *flags)
{
	struct cpu *cpu;

	/*
	 * Keep interrupts disabled until lockdep_leave().  The recursion
	 * counter is per-CPU, so a preemption between setting it and finishing
	 * the operation would make the next thread skip lockdep entirely and
	 * leave stale held-lock entries behind.
	 */
	*flags = local_irq_save();

	if (!lockdep_ready || !lockdep_enabled) {
		local_irq_restore(*flags);
		return false;
	}
	if (this_cpuid_safe() == (cpuid_t)-1) {
		local_irq_restore(*flags);
		return false;
	}

	cpu = this_cpu();
	if (cpu->lockdep_recursion) {
		local_irq_restore(*flags);
		return false;
	}

	cpu->lockdep_recursion++;
	return true;
}

static void lockdep_leave(irqstate_t flags)
{
	this_cpu()->lockdep_recursion--;
	local_irq_restore(flags);
}

static void lockdep_report(struct thread *thread, const char *reason,
			   u16 new_id, enum lockdep_mode new_mode, u16 held_id,
			   enum lockdep_mode held_mode, void *caller)
{
	char buf[1024];
	size_t off;
	int n;

	n = snprintf(
		buf, sizeof(buf),
		"lockdep: %s: new=%s (%s) held=%s (%s) tid=%d caller=%p stack:",
		reason, lockdep_classes[new_id].name,
		lockdep_mode_name(new_mode), lockdep_classes[held_id].name,
		lockdep_mode_name(held_mode), thread->tid, caller);
	off = n < 0 ? 0 : (size_t)n;
	if (off >= sizeof(buf))
		off = sizeof(buf) - 1;

	if (!strcmp(reason, "dependency cycle")) {
		u16 path[LOCKDEP_MAX_CLASSES];
		unsigned int len = 0;
		u16 class = held_id;

		while (class != LOCKDEP_INVALID_CLASS &&
		       len < LOCKDEP_MAX_CLASSES) {
			path[len++] = class;
			if (class == new_id)
				break;
			class = lockdep_bfs_parent[class];
		}

		for (unsigned int i = len; i > 1; --i) {
			u16 from = path[i - 1];
			u16 to = path[i - 1];
			const void *edge_caller = lockdep_edge_caller[from][to];

			n = snprintf(buf + off, sizeof(buf) - off, " %s@%p ->",
				     lockdep_classes[to].name, edge_caller);
			if (n < 0)
				break;
			if ((size_t)n >= sizeof(buf) - off) {
				off = sizeof(buf) - 1;
				break;
			}
			off += (size_t)n;
		}
	}

	for (unsigned int i = 0; i < thread->lockdep_depth; ++i) {
		struct lockdep_held *held = &thread->lockdep_held[i];

		n = snprintf(buf + off, sizeof(buf) - off, " %s(%s)",
			     lockdep_classes[held->class_id].name,
			     lockdep_mode_name(held->mode));
		if (n < 0)
			break;
		if ((size_t)n >= sizeof(buf) - off) {
			off = sizeof(buf) - 1;
			break;
		}
		off += (size_t)n;
	}

	panic("%s\n", buf);
}

void lockdep_init(void)
{
	if (lockdep_ready)
		return;

	lockdep_nr_classes = 0;
	memset(lockdep_edges, 0, sizeof(lockdep_edges));
	lockdep_ready = true;
	lockdep_enabled = true;
}

void lockdep_acquire(const char *name, const void *lock, enum lockdep_mode mode,
		     bool trylock, void *caller)
{
	struct thread *thread;
	enum lockdep_mode held_mode = LOCKDEP_WRITE;
	u16 new_id;
	u16 bad_id = LOCKDEP_INVALID_CLASS;
	const char *reason = NULL;
	irqstate_t flags;
	bool bad = false;

	if (!lockdep_enter(&flags))
		return;

	thread = current_thread();
	if (!thread) {
		lockdep_leave(flags);
		return;
	}

	raw_spin_lock(&lockdep_graph_lock);

	if (!lockdep_enabled) {
		raw_spin_unlock(&lockdep_graph_lock);
		lockdep_leave(flags);
		return;
	}

	new_id = lockdep_class_id(name);
	if (new_id == LOCKDEP_INVALID_CLASS) {
		lockdep_enabled = false;
		raw_spin_unlock(&lockdep_graph_lock);
		lockdep_leave(flags);
		panic("lockdep: class table is full\n");
	}

	if (thread->lockdep_depth >= LOCKDEP_MAX_HELD) {
		lockdep_enabled = false;
		raw_spin_unlock(&lockdep_graph_lock);
		lockdep_leave(flags);
		panic("lockdep: held-lock stack overflow\n");
	}

	for (unsigned int i = 0; i < thread->lockdep_depth; ++i) {
		struct lockdep_held *held = &thread->lockdep_held[i];

		if (held->lock == lock) {
			reason = "recursive lock acquisition";
			bad_id = held->class_id;
			held_mode = held->mode;
			bad = true;
			break;
		}

		if (held->class_id == new_id) {
			if (lockdep_classes[new_id].nested)
				continue;
			if (held->mode == LOCKDEP_READ && mode == LOCKDEP_READ)
				continue;
			/*
			 * A successful trylock never waits.  It may nest two
			 * instances of the same class; the edges added by
			 * later blocking acquisitions still catch real order
			 * inversions.
			 */
			if (trylock)
				continue;
			reason = "recursive lock class";
			bad_id = held->class_id;
			held_mode = held->mode;
			bad = true;
			break;
		}

		if (trylock)
			continue;

		/*
		 * Two readers do not block each other.  Every other pair
		 * creates a real hold-and-wait dependency.
		 */
		if (held->mode == LOCKDEP_READ && mode == LOCKDEP_READ)
			continue;

		if (!lockdep_edges[held->class_id][new_id]) {
			lockdep_edges[held->class_id][new_id] = 1;
			lockdep_edge_caller[held->class_id][new_id] = caller;
			if (lockdep_reachable(new_id, held->class_id)) {
				reason = "dependency cycle";
				bad_id = held->class_id;
				held_mode = held->mode;
				bad = true;
				break;
			}
		}
	}

	if (bad) {
		lockdep_enabled = false;
		raw_spin_unlock(&lockdep_graph_lock);
		lockdep_leave(flags);
		lockdep_report(thread, reason, new_id, mode, bad_id, held_mode,
			       caller);
		panic("lockdep: deadlock detected\n");
	}

	thread->lockdep_held[thread->lockdep_depth].lock = lock;
	thread->lockdep_held[thread->lockdep_depth].class_id = new_id;
	thread->lockdep_held[thread->lockdep_depth].mode = mode;
	thread->lockdep_depth++;

	raw_spin_unlock(&lockdep_graph_lock);
	lockdep_leave(flags);
}

void lockdep_release(const void *lock)
{
	struct thread *thread;
	irqstate_t flags;

	if (!lockdep_enter(&flags))
		return;

	thread = current_thread();
	if (!thread) {
		lockdep_leave(flags);
		return;
	}

	/*
	 * The held stack is private to the current thread.  Interrupts stay
	 * disabled until lockdep_leave(), so no other context can observe or
	 * modify it here.
	 */
	for (unsigned int i = thread->lockdep_depth; i > 0; --i) {
		if (thread->lockdep_held[i - 1].lock != lock)
			continue;

		for (unsigned int j = i - 1; j + 1 < thread->lockdep_depth; ++j)
			thread->lockdep_held[j] = thread->lockdep_held[j + 1];
		thread->lockdep_depth--;
		break;
	}

	lockdep_leave(flags);
}

bool lockdep_is_held(const void *lock)
{
	struct thread *thread;
	irqstate_t flags;
	bool held = false;

	if (!lockdep_enter(&flags))
		return false;

	thread = current_thread();
	if (!thread) {
		lockdep_leave(flags);
		return false;
	}

	for (unsigned int i = 0; i < thread->lockdep_depth; ++i) {
		if (thread->lockdep_held[i].lock == lock) {
			held = true;
			break;
		}
	}
	lockdep_leave(flags);
	return held;
}

#endif
