
static void interval_rb_augment_cb(struct rb_node *node, void *__unused)
{
	TYPE max_end = END(node);

	if (node->rb_left && MAX_END(node->rb_left) > max_end)
		max_end = MAX_END(node->rb_left);

	if (node->rb_right && MAX_END(node->rb_right) > max_end)
		max_end = MAX_END(node->rb_right);

	SET_MAX_END(node, max_end);
}

static struct rb_node *interval_insert(struct rb_root *root, struct rb_node *ins)
{
	struct rb_node **node = &(root->rb_node), *parent = NULL;

	while (*node) {
		parent = *node;
		if (START(ins) < START(*node)) {
			node = &((*node)->rb_left);
		} else if (START(ins) > START(*node)) {
			node = &((*node)->rb_right);
		} else {
			if (END(ins) < END(*node))
				node = &((*node)->rb_left);
			else if (END(ins) > END(*node))
				node = &((*node)->rb_right);
			else
				return *node;
		}
	}

	rb_link_node(ins, parent, node);
	rb_insert_color(ins, root);
	rb_augment_insert(ins, interval_rb_augment_cb, NULL);

	return ins;
}

static struct rb_node *interval_search_exact(struct rb_root *root,
		TYPE start, TYPE end)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		if (start < START(node)) {
			node = node->rb_left;
		} else if (start > START(node)) {
			node = node->rb_right;
		} else {
			if (end < END(node))
				node = node->rb_left;
			else if (end > END(node))
				node = node->rb_right;
			else
				return node;
		}
	}

	return NULL;
}

static void interval_erase(struct rb_root *root, struct rb_node *erase)
{
	struct rb_node *deepest = rb_augment_erase_begin(erase);

	rb_erase(erase, root);
	rb_augment_erase_end(deepest, interval_rb_augment_cb, NULL);
}

/*
 * Find the first overlap interval from subtree, O(log(N)).
 * Caller ensures node != NULL && MAX_END(node) > start
 * Returns:
 *   the first overlap interval
 *   NULL: no overlap interval and there is at least one node p in
 *         the subtree that START(p) >= end
 */
static struct rb_node *__interval_first_overlap(struct rb_node *node,
		TYPE start, TYPE end)
{
	for (;;) {
		BUG_ON(!node || MAX_END(node) <= start);

		if (node->rb_left && MAX_END(node->rb_left) > start)
			node = node->rb_left;
		else if (end <= START(node))
			return NULL;
		else if (END(node) > start)
			return node;
		else
			node = node->rb_right;
	}
}

/* O(log(N)) */
static struct rb_node *interval_first_overlap(struct rb_root *root,
		TYPE start, TYPE end)
{
	if (root->rb_node && MAX_END(root->rb_node) > start)
		return __interval_first_overlap(root->rb_node, start, end);
	else
		return NULL;
}

/* O(log(N)) */
static struct rb_node *interval_next_overlap(struct rb_node *node,
		TYPE start, TYPE end)
{
	struct rb_node *parent;

	if (node->rb_right && MAX_END(node->rb_right) > start)
		return __interval_first_overlap(node->rb_right, start, end);

	while ((parent = rb_parent(node)) != NULL) {
		if (node == parent->rb_right)
			continue;
		node = parent;
		if (end <= START(node))
			return NULL;
		if (END(node) > start)
			return node;
		if (node->rb_right && MAX_END(node->rb_right) > start)
			return __interval_first_overlap(node->rb_right, start, end);
	}

	return NULL;
}

/* example of for_each_overlap_interval, O(min(N, k*log(N))) */
#define for_each_overlap_interval(root, pos, start, end)	\
	for (pos = interval_first_overlap(root, start, end);	\
	     pos; pos = interval_next_overlap(pos, start, end))

#undef TYPE
#undef START
#undef END
#undef MAX_END
#undef SET_MAX_END

