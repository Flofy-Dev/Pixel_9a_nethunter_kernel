/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  include/linux/userfaultfd_k.h
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 *
 */

#ifndef _LINUX_USERFAULTFD_K_H
#define _LINUX_USERFAULTFD_K_H

#ifdef CONFIG_USERFAULTFD

#include <linux/userfaultfd.h> /* linux/include/uapi/linux/userfaultfd.h */

#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <asm-generic/pgtable_uffd.h>
#include <linux/hugetlb_inline.h>

/* The set of all possible UFFD-related VM flags. */
#define __VM_UFFD_FLAGS (VM_UFFD_MISSING | VM_UFFD_WP | VM_UFFD_MINOR)

/*
 * CAREFUL: Check include/uapi/asm-generic/fcntl.h when defining
 * new flags, since they might collide with O_* ones. We want
 * to re-use O_* flags that couldn't possibly have a meaning
 * from userfaultfd, in order to leave a free define-space for
 * shared O_* flags.
 */
#define UFFD_CLOEXEC O_CLOEXEC
#define UFFD_NONBLOCK O_NONBLOCK

#define UFFD_SHARED_FCNTL_FLAGS (O_CLOEXEC | O_NONBLOCK)
#define UFFD_FLAGS_SET (EFD_SHARED_FCNTL_FLAGS)

/*
 * Start with fault_pending_wqh and fault_wqh so they're more likely
 * to be in the same cacheline.
 *
 * Locking order:
 *	fd_wqh.lock
 *		fault_pending_wqh.lock
 *			fault_wqh.lock
 *		event_wqh.lock
 *
 * To avoid deadlocks, IRQs must be disabled when taking any of the above locks,
 * since fd_wqh.lock is taken by aio_poll() while it's holding a lock that's
 * also taken in IRQ context.
 */

/*
 * ANDROID: CRC fix for commit f91e6b41dd11 ("userfaultfd: move userfaultfd_ctx
 * struct to header file")
 */
#ifndef __GENKSYMS__
struct userfaultfd_ctx {
	/* waitqueue head for the pending (i.e. not read) userfaults */
	wait_queue_head_t fault_pending_wqh;
	/* waitqueue head for the userfaults */
	wait_queue_head_t fault_wqh;
	/* waitqueue head for the pseudo fd to wakeup poll/read */
	wait_queue_head_t fd_wqh;
	/* waitqueue head for events */
	wait_queue_head_t event_wqh;
	/* a refile sequence protected by fault_pending_wqh lock */
	seqcount_spinlock_t refile_seq;
	/* pseudo fd refcounting */
	refcount_t refcount;
	/* userfaultfd syscall flags */
	unsigned int flags;
	/* features requested from the userspace */
	unsigned int features;
	/* released */
	bool released;
	/*
	 * Prevents userfaultfd operations (fill/move/wp) from happening while
	 * some non-cooperative event(s) is taking place. Increments are done
	 * in write-mode. Whereas, userfaultfd operations, which includes
	 * reading mmap_changing, is done under read-mode.
	 */
	struct rw_semaphore map_changing_lock;
	/* memory mappings are changing because of non-cooperative event */
	atomic_t mmap_changing;
	/* mm with one ore more vmas attached to this userfaultfd_ctx */
	struct mm_struct *mm;

	ANDROID_KABI_RESERVE(1);
};
#endif

extern int sysctl_unprivileged_userfaultfd;

extern vm_fault_t handle_userfault(struct vm_fault *vmf, unsigned long reason);

/*
 * The mode of operation for __mcopy_atomic and its helpers.
 *
 * This is almost an implementation detail (mcopy_atomic below doesn't take this
 * as a parameter), but it's exposed here because memory-kind-specific
 * implementations (e.g. hugetlbfs) need to know the mode of operation.
 */
enum mcopy_atomic_mode {
	/* A normal copy_from_user into the destination range. */
	MCOPY_ATOMIC_NORMAL,
	/* Don't copy; map the destination range to the zero page. */
	MCOPY_ATOMIC_ZEROPAGE,
	/* Just install pte(s) with the existing page(s) in the page cache. */
	MCOPY_ATOMIC_CONTINUE,
};

extern int mfill_atomic_install_pte(struct mm_struct *dst_mm, pmd_t *dst_pmd,
				    struct vm_area_struct *dst_vma,
				    unsigned long dst_addr, struct page *page,
				    bool newly_allocated, bool wp_copy);

extern ssize_t mcopy_atomic(struct userfaultfd_ctx *ctx, unsigned long dst_start,
			    unsigned long src_start, unsigned long len, __u64 mode);
extern ssize_t mfill_zeropage(struct userfaultfd_ctx *ctx,
			      unsigned long dst_start, unsigned long len);
extern ssize_t mcopy_continue(struct userfaultfd_ctx *ctx, unsigned long dst_start,
			      unsigned long len);
extern int mwriteprotect_range(struct userfaultfd_ctx *ctx, unsigned long start,
			       unsigned long len, bool enable_wp);
extern void uffd_wp_range(struct mm_struct *dst_mm, struct vm_area_struct *vma,
			  unsigned long start, unsigned long len, bool enable_wp);

/* move_pages */
void double_pt_lock(spinlock_t *ptl1, spinlock_t *ptl2);
void double_pt_unlock(spinlock_t *ptl1, spinlock_t *ptl2);
ssize_t move_pages(struct userfaultfd_ctx *ctx, unsigned long dst_start,
		   unsigned long src_start, unsigned long len, __u64 flags);
int move_pages_huge_pmd(struct mm_struct *mm, pmd_t *dst_pmd, pmd_t *src_pmd, pmd_t dst_pmdval,
			struct vm_area_struct *dst_vma,
			struct vm_area_struct *src_vma,
			unsigned long dst_addr, unsigned long src_addr);

/* mm helpers */
static inline bool is_mergeable_vm_userfaultfd_ctx(struct vm_area_struct *vma,
					struct vm_userfaultfd_ctx vm_ctx)
{
	return vma->vm_userfaultfd_ctx.ctx == vm_ctx.ctx;
}

/*
 * Never enable huge pmd sharing on some uffd registered vmas:
 *
 * - VM_UFFD_WP VMAs, because write protect information is per pgtable entry.
 *
 * - VM_UFFD_MINOR VMAs, because otherwise we would never get minor faults for
 *   VMAs which share huge pmds. (If you have two mappings to the same
 *   underlying pages, and fault in the non-UFFD-registered one with a write,
 *   with huge pmd sharing this would *also* setup the second UFFD-registered
 *   mapping, and we'd not get minor faults.)
 */
static inline bool uffd_disable_huge_pmd_share(struct vm_area_struct *vma)
{
	return vma->vm_flags & (VM_UFFD_WP | VM_UFFD_MINOR);
}

/*
 * Don't do fault around for either WP or MINOR registered uffd range.  For
 * MINOR registered range, fault around will be a total disaster and ptes can
 * be installed without notifications; for WP it should mostly be fine as long
 * as the fault around checks for pte_none() before the installation, however
 * to be super safe we just forbid it.
 */
static inline bool uffd_disable_fault_around(struct vm_area_struct *vma)
{
	return vma->vm_flags & (VM_UFFD_WP | VM_UFFD_MINOR);
}

static inline bool userfaultfd_missing(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_UFFD_MISSING;
}

static inline bool userfaultfd_wp(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_UFFD_WP;
}

static inline bool userfaultfd_minor(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_UFFD_MINOR;
}

static inline bool userfaultfd_pte_wp(struct vm_area_struct *vma,
				      pte_t pte)
{
	return userfaultfd_wp(vma) && pte_uffd_wp(pte);
}

static inline bool userfaultfd_huge_pmd_wp(struct vm_area_struct *vma,
					   pmd_t pmd)
{
	return userfaultfd_wp(vma) && pmd_uffd_wp(pmd);
}

static inline bool userfaultfd_armed(struct vm_area_struct *vma)
{
	return vma->vm_flags & __VM_UFFD_FLAGS;
}

static inline bool vma_can_userfault(struct vm_area_struct *vma,
				     unsigned long vm_flags)
{
	if ((vm_flags & VM_UFFD_MINOR) &&
	    (!is_vm_hugetlb_page(vma) && !vma_is_shmem(vma)))
		return false;
#ifndef CONFIG_PTE_MARKER_UFFD_WP
	/*
	 * If user requested uffd-wp but not enabled pte markers for
	 * uffd-wp, then shmem & hugetlbfs are not supported but only
	 * anonymous.
	 */
	if ((vm_flags & VM_UFFD_WP) && !vma_is_anonymous(vma))
		return false;
#endif
	return vma_is_anonymous(vma) || is_vm_hugetlb_page(vma) ||
	    vma_is_shmem(vma);
}

extern int dup_userfaultfd(struct vm_area_struct *, struct list_head *);
extern void dup_userfaultfd_complete(struct list_head *);
void dup_userfaultfd_fail(struct list_head *);

extern void mremap_userfaultfd_prep(struct vm_area_struct *,
				    struct vm_userfaultfd_ctx *);
extern void mremap_userfaultfd_complete(struct vm_userfaultfd_ctx *,
					unsigned long from, unsigned long to,
					unsigned long len);

extern bool userfaultfd_remove(struct vm_area_struct *vma,
			       unsigned long start,
			       unsigned long end);

extern int userfaultfd_unmap_prep(struct vm_area_struct *vma,
		unsigned long start, unsigned long end, struct list_head *uf);
extern void userfaultfd_unmap_complete(struct mm_struct *mm,
				       struct list_head *uf);

#else /* CONFIG_USERFAULTFD */

/* mm helpers */
static inline vm_fault_t handle_userfault(struct vm_fault *vmf,
				unsigned long reason)
{
	return VM_FAULT_SIGBUS;
}

static inline bool is_mergeable_vm_userfaultfd_ctx(struct vm_area_struct *vma,
					struct vm_userfaultfd_ctx vm_ctx)
{
	return true;
}

static inline bool userfaultfd_missing(struct vm_area_struct *vma)
{
	return false;
}

static inline bool userfaultfd_wp(struct vm_area_struct *vma)
{
	return false;
}

static inline bool userfaultfd_minor(struct vm_area_struct *vma)
{
	return false;
}

static inline bool userfaultfd_pte_wp(struct vm_area_struct *vma,
				      pte_t pte)
{
	return false;
}

static inline bool userfaultfd_huge_pmd_wp(struct vm_area_struct *vma,
					   pmd_t pmd)
{
	return false;
}


static inline bool userfaultfd_armed(struct vm_area_struct *vma)
{
	return false;
}

static inline int dup_userfaultfd(struct vm_area_struct *vma,
				  struct list_head *l)
{
	return 0;
}

static inline void dup_userfaultfd_complete(struct list_head *l)
{
}

static inline void dup_userfaultfd_fail(struct list_head *l)
{
}

static inline void mremap_userfaultfd_prep(struct vm_area_struct *vma,
					   struct vm_userfaultfd_ctx *ctx)
{
}

static inline void mremap_userfaultfd_complete(struct vm_userfaultfd_ctx *ctx,
					       unsigned long from,
					       unsigned long to,
					       unsigned long len)
{
}

static inline bool userfaultfd_remove(struct vm_area_struct *vma,
				      unsigned long start,
				      unsigned long end)
{
	return true;
}

static inline int userfaultfd_unmap_prep(struct vm_area_struct *vma,
					 unsigned long start, unsigned long end,
					 struct list_head *uf)
{
	return 0;
}

static inline void userfaultfd_unmap_complete(struct mm_struct *mm,
					      struct list_head *uf)
{
}

static inline bool uffd_disable_fault_around(struct vm_area_struct *vma)
{
	return false;
}

#endif /* CONFIG_USERFAULTFD */

static inline bool pte_marker_entry_uffd_wp(swp_entry_t entry)
{
#ifdef CONFIG_PTE_MARKER_UFFD_WP
	return is_pte_marker_entry(entry) &&
	    (pte_marker_get(entry) & PTE_MARKER_UFFD_WP);
#else
	return false;
#endif
}

static inline bool pte_marker_uffd_wp(pte_t pte)
{
#ifdef CONFIG_PTE_MARKER_UFFD_WP
	swp_entry_t entry;

	if (!is_swap_pte(pte))
		return false;

	entry = pte_to_swp_entry(pte);

	return pte_marker_entry_uffd_wp(entry);
#else
	return false;
#endif
}

/*
 * Returns true if this is a swap pte and was uffd-wp wr-protected in either
 * forms (pte marker or a normal swap pte), false otherwise.
 */
static inline bool pte_swp_uffd_wp_any(pte_t pte)
{
#ifdef CONFIG_PTE_MARKER_UFFD_WP
	if (!is_swap_pte(pte))
		return false;

	if (pte_swp_uffd_wp(pte))
		return true;

	if (pte_marker_uffd_wp(pte))
		return true;
#endif
	return false;
}

#endif /* _LINUX_USERFAULTFD_K_H */
