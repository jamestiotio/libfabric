/*
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015 Cray Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * The buddy allocator splits the "base" block being managed into smaller
 * blocks.  Each block is a "power-of-two length".  These subblocks are kept
 * track of in a doubly linked list, or free list.  Here are the structures
 * and format of the data structures used in the buddy allocator.  For
 * a description of each field please see gnix_buddy_allocator.h.
 *
 		Handle structure:
		┌──────┬──────┬─────┬─────┬────────┬───────┐
		│ BASE │ len  │ min │ max │ nlists │ LISTS │
		└──────┴──────┴─────┴─────┴────────┴───────┘
 * The LISTS pointer points to an array of dlist structures, each containing a
 * head pointer to the begging of a free list.  Note that the first element of
 * LISTS is a pointer to the head of the "min block size" free list, the second
 * element is the head of the "min * 2 block size" free list and so on.

 		Node format as stored in a free block:
		┌──────┬──────┬──────────────────────┐
		│ NEXT │ PREV │ Remaining free bytes │
		└──────┴──────┴──────────────────────┘
 * Each NEXT and PREV pointer is stored in the first 16 bytes of the free block.
 * This means that there is a hard limit of 16 bytes on the minimum block size.
 *
 *		Bitmap layout with a min block size of 16:
 		┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
		│16│16│16│16│..│32│32│32│32│..│64│64│64│64│..│
		└──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
 * Each number above (16, 32, and 64) represents a block size that can
 * be split. The first two "32" blocks are redundantly mapped by the the first
 * 64 block, all blocks are redundant mappings of the next block size.
 *
 * When a block is split or allocated, its bit will be set in the bitmap.
 * When a block is coalesced or free'd its bit will be reset in the bitmap.
 * This improves the performance of coalescing blocks by being able to tell
 * whether a given block's buddy block is allocated or split in O(c).
 *
 * TODO: insert_sorted for fragmentation.
 * TODO: lock alloc_handle
 */

#include "gnix_buddy_allocator.h"

static inline int __gnix_buddy_create_lists(handle_t alloc_handle)
{
	size_t i, offset = 0;

	alloc_handle->nlists = (size_t) log2(alloc_handle->max /
					     (double) alloc_handle->min) + 1;
	alloc_handle->lists = calloc(1, sizeof(struct dlist_entry) *
				     alloc_handle->nlists);

	if (!alloc_handle->lists) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Could not create buddy allocator lists.\n");
		return -FI_ENOMEM;
	}

	for (i = 0; i < alloc_handle->nlists; i++) {
		dlist_init(alloc_handle->lists + i);
	}

	/* Insert free blocks of size max in sorted order into last list */
	for (i = 0; i < alloc_handle->len / alloc_handle->max; i++) {
		dlist_insert_tail(alloc_handle->base + offset,
				  alloc_handle->lists +
				  alloc_handle->nlists - 1);
		offset += alloc_handle->max;
	}

	return FI_SUCCESS;
}

/**
 * Split a block in list "j" until list "i" is reached.
 */
static inline void __gnix_buddy_split(handle_t alloc_handle, size_t j, size_t i)
{
	void *tmp = NULL;

	dlist_remove(tmp = alloc_handle->lists[j].next);

	_gnix_set_bit(&alloc_handle->bitmap,
		      BITMAP_INDEX(tmp, alloc_handle->base, alloc_handle->min,
				   OFFSET(alloc_handle->min, j)));

	/* Split the block until we reach list "i" */
	for (j--; j > i; j--) {
		_gnix_set_bit(&alloc_handle->bitmap,
			      BITMAP_INDEX(tmp +
					   OFFSET(alloc_handle->min, j),
					   alloc_handle->base,
					   alloc_handle->min,
					   OFFSET(alloc_handle->min, j)));

		dlist_insert_tail(tmp + OFFSET(alloc_handle->min, j),
				  alloc_handle->lists + j);
	}

	/* Insert last block into list "i" */
	dlist_insert_head(tmp, alloc_handle->lists + j);

	/* Insert the buddy block of tmp into list "i" */
	dlist_insert_tail(tmp + OFFSET(alloc_handle->min, j),
			  alloc_handle->lists + j);
}

/**
 * Find the first free block that can be split, then split it.
 *
 * @return 1  if the block cannot be found.
 *
 * @return 0 if the block is found.
 */
static inline int __gnix_buddy_find_block(handle_t alloc_handle, size_t i)
{
	size_t j;

	for (j = i + 1; j < alloc_handle->nlists; j++) {
		if (!dlist_empty(alloc_handle->lists + j)) {
			__gnix_buddy_split(alloc_handle, j, i);
			return 0;
		}
	}

	return 1;
}


/**
 * If the buddy block is on the free list then coalesce and insert into the next
 * list until we reach an allocated or split buddy block, or the max list size.
 */
static inline void __gnix_buddy_coalesce(handle_t alloc_handle, void **ptr,
					 size_t *bsize)
{
	while (*bsize < alloc_handle->max &&
	       !_gnix_test_bit(&alloc_handle->bitmap,
			       BITMAP_INDEX(BUDDY(*ptr, *bsize,
						  alloc_handle->base),
					    alloc_handle->base,
					    alloc_handle->min,
					    *bsize))) {

		dlist_remove(BUDDY(*ptr, *bsize, alloc_handle->base));

		/* Ensure ptr is the beginning of the new block */
		if (*ptr > BUDDY(*ptr, *bsize, alloc_handle->base)) {
			*ptr = BUDDY(*ptr, *bsize, alloc_handle->base);
		}

		*bsize *= 2;

		_gnix_clear_bit(&alloc_handle->bitmap,
				BITMAP_INDEX(*ptr, alloc_handle->base,
					     alloc_handle->min, *bsize));
	}
}

int _gnix_buddy_allocator_create(void *base, size_t len, size_t max,
				 handle_t *alloc_handle)
{
	char err_buf[256] = {0}, *error = NULL;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	/* Ensure parameters are valid */
	if (!base || !len || !max || max > len || !alloc_handle ||
	    IS_NOT_POW_TWO(max) || (len % max)) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Invalid parameter to buddy_allocator_create.\n");
		return -FI_EINVAL;
	}

	*alloc_handle = calloc(1, sizeof(struct gnix_buddy_alloc_handle));
	if (!alloc_handle) {
		error = strerror_r(errno, err_buf, sizeof(err_buf));
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Could not create buddy allocator handle.",
			  error);
		return -FI_ENOMEM;
	}

	alloc_handle[0]->base = base;
	alloc_handle[0]->len = len;

	alloc_handle[0]->min = 16;
	alloc_handle[0]->max = max;

	if (__gnix_buddy_create_lists(alloc_handle[0])) {
		return -FI_ENOMEM;
	}

	return _gnix_alloc_bitmap(&alloc_handle[0]->bitmap,
				      len / alloc_handle[0]->min * 2);
}

int _gnix_buddy_allocator_destroy(handle_t alloc_handle)
{
	int fi_errno;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (!alloc_handle) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Invalid parameter to buddy_allocator_destroy.\n");
		return -FI_EINVAL;
	}

	free(alloc_handle->lists);

	if ((fi_errno = _gnix_free_bitmap(&alloc_handle->bitmap))) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Failed to free buddy_allocator_handle bitmap.");
		return fi_errno;
	}

	free(alloc_handle);

	return FI_SUCCESS;
}

int _gnix_buddy_alloc(handle_t alloc_handle, void **ptr, size_t len)
{
	size_t bsize, i = 0;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (!alloc_handle || !ptr || !len || len > alloc_handle->max) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Invalid parameter to buddy_allocator_alloc.\n");
		return -FI_EINVAL;
	}

	bsize = BLOCK_SIZE(len, alloc_handle->min);
	i = (size_t) LIST_INDEX(bsize, alloc_handle->min);

	if (dlist_empty(alloc_handle->lists + i) &&
	    __gnix_buddy_find_block(alloc_handle, i)) {
		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Could not allocate buddy block.\n");
		return -FI_ENOMEM;
	}

	/*
	 * Remove the block from opposite sides of adjacent lists to reduce
	 * fragmentation and improve coalescing
	 */
	if (i % 2) {
		dlist_remove(*ptr = alloc_handle->lists[i].prev);
	} else {
		dlist_remove(*ptr = alloc_handle->lists[i].next);
	}

	_gnix_set_bit(&alloc_handle->bitmap,
		      BITMAP_INDEX(*ptr, alloc_handle->base, alloc_handle->min,
				   bsize));

	return FI_SUCCESS;
}

int _gnix_buddy_free(handle_t alloc_handle, void *ptr, size_t len)
{
	size_t bsize;

	GNIX_TRACE(FI_LOG_EP_CTRL, "\n");

	if (!alloc_handle || !len || len > alloc_handle->max ||
	    ptr >= alloc_handle->base + alloc_handle->len  ||
	    ptr < alloc_handle->base) {

		GNIX_WARN(FI_LOG_EP_CTRL,
			  "Invalid parameter to buddy_allocator_free.\n");
		return -FI_EINVAL;
	}

	bsize = BLOCK_SIZE(len, alloc_handle->min);

	_gnix_clear_bit(&alloc_handle->bitmap,
			BITMAP_INDEX(ptr, alloc_handle->base, alloc_handle->min,
				     bsize));

	__gnix_buddy_coalesce(alloc_handle, &ptr, &bsize);

	dlist_insert_tail(ptr, alloc_handle->lists +
			  LIST_INDEX(bsize, alloc_handle->min));

	return FI_SUCCESS;
}
