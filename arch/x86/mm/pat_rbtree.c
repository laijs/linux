/*
 * Handle caching attributes in page tables (PAT)
 *
 * Authors: Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *          Suresh B Siddha <suresh.b.siddha@intel.com>
 *
 * Interval tree (augmented rbtree) used to store the PAT memory type
 * reservations.
 */

#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/gfp.h>

#include <asm/pgtable.h>
#include <asm/pat.h>

#include "pat_internal.h"

/*
 * The memtype tree keeps track of memory type for specific
 * physical memory areas. Without proper tracking, conflicting memory
 * types in different mappings can cause CPU cache corruption.
 *
 * The tree is an interval tree (augmented rbtree) with tree ordered
 * on starting address. Tree can contain multiple entries for
 * different regions which overlap. All the aliases have the same
 * cache attributes of course.
 *
 * memtype_lock protects the rbtree.
 */

static struct rb_root memtype_rbroot = RB_ROOT;

#define TYPE u64
#define START(node) (container_of(node, struct memtype, rb)->start)
#define END(node) (container_of(node, struct memtype, rb)->end)
#define MAX_END(node) (container_of(node, struct memtype, rb)->subtree_max_end)
#define SET_MAX_END(node, val) 						\
	do {								\
		container_of(node, struct memtype, rb)->subtree_max_end = val;\
	} while (0)
#include <linux/interval_tree_tmpl.h>

static int memtype_rb_check_conflict(struct rb_root *root,
				u64 start, u64 end,
				unsigned long reqtype, unsigned long *newtype)
{
	struct rb_node *node;
	struct memtype *match;
	int found_type = reqtype;

	node = interval_first_overlap(&memtype_rbroot, start, end);
	if (node == NULL)
		goto success;

	match = container_of(node, struct memtype, rb);
	if (match->type != found_type && newtype == NULL)
		goto failure;

	dprintk("Overlap at 0x%Lx-0x%Lx\n", match->start, match->end);
	found_type = match->type;

	while ((node = interval_next_overlap(node, start, end)) != NULL) {
		match = container_of(node, struct memtype, rb);

		if (match->type != found_type)
			goto failure;
	}

success:
	if (newtype)
		*newtype = found_type;

	return 0;

failure:
	printk(KERN_INFO "%s:%d conflicting memory types "
		"%Lx-%Lx %s<->%s\n", current->comm, current->pid, start,
		end, cattr_name(found_type), cattr_name(match->type));
	return -EBUSY;
}

int rbt_memtype_check_insert(struct memtype *new, unsigned long *ret_type)
{
	int err = 0;
	struct rb_node *node;

	err = memtype_rb_check_conflict(&memtype_rbroot, new->start, new->end,
						new->type, ret_type);

	if (!err) {
		if (ret_type)
			new->type = *ret_type;

		node = interval_insert(&memtype_rbroot, &new->rb);
		if (node != &new->rb) {
			/* insert duplicated */
			struct rb_node **p = &node->rb_right;

			while (*p) {
				node = *p;
				p = &node->rb_left;
			}

			rb_link_node(&new->rb, node, p);
			rb_insert_color(&new->rb, &memtype_rbroot);
			rb_augment_insert(&new->rb, interval_rb_augment_cb,
					NULL);
		}
	}
	return err;
}

struct memtype *rbt_memtype_erase(u64 start, u64 end)
{
	struct rb_node *node;

	node = interval_search_exact(&memtype_rbroot, start, end);
	if (!node)
		return NULL;

	interval_erase(&memtype_rbroot, node);

	return container_of(node, struct memtype, rb);
}

struct memtype *rbt_memtype_lookup(u64 addr)
{
	struct rb_node *node;

	node = interval_first_overlap(&memtype_rbroot, addr, addr + PAGE_SIZE);
	if (!node)
		return NULL;
	return container_of(node, struct memtype, rb);
}

#if defined(CONFIG_DEBUG_FS)
int rbt_memtype_copy_nth_element(struct memtype *out, loff_t pos)
{
	struct rb_node *node;
	int i = 1;

	node = rb_first(&memtype_rbroot);
	while (node && pos != i) {
		node = rb_next(node);
		i++;
	}

	if (node) { /* pos == i */
		struct memtype *this = container_of(node, struct memtype, rb);
		*out = *this;
		return 0;
	} else {
		return 1;
	}
}
#endif
