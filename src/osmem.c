// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"

#define ALLIGNMENT 8 
#define ALIGN(size) (((size) + (ALLIGNMENT - 1)) &  ~(ALLIGNMENT-1))
#define META_SIZE ALIGN(sizeof(struct block_meta))

#define MMAP_THRESHOLD 128*1024

struct block_meta* head = NULL;
int prealloced_mem = 0;

/*	This function combines adjent free blocks into one.
	Stratrs from <head> to the end of the list.  */
void coalesce_blocks() {
	struct block_meta* iter = head;
	while (iter) {
		while (iter->status == STATUS_FREE && iter->next && iter->next->status == STATUS_FREE) {
			struct block_meta* tmp = iter->next;
			iter->next = tmp->next;
			if (iter->next) {
				tmp->next->prev = iter;}
			iter->size = iter->size + tmp->size + META_SIZE;
		}
		iter = iter->next;
	}
}

/*	 This function split the blocked passed as a parameter into two blocks.
	First block's status becomes STATUS_ALLOC, second one's becomes STATUS_FREE */
struct block_meta* split(struct block_meta* block_to_split, size_t size) {
	long long int remaining_size = block_to_split->size - ALIGN(size) - META_SIZE;
	if (remaining_size >= 8) {
		size_t remaining_size = block_to_split->size - ALIGN(size) - META_SIZE;
		struct block_meta* neighbour_block = block_to_split->next;
		block_to_split->size = ALIGN(size);
		block_to_split->next = (struct block_meta*)((char *)(block_to_split) + META_SIZE + block_to_split->size);
		block_to_split->next->prev = block_to_split;
		block_to_split->next->next = neighbour_block;
		block_to_split->status = STATUS_ALLOC;
		block_to_split->next->size = remaining_size;
		block_to_split->next->status = STATUS_FREE;
		if(neighbour_block) {
			neighbour_block->prev = block_to_split->next;
		}
		return block_to_split;
	}
	block_to_split->status = STATUS_ALLOC;
	return block_to_split;
}

/*	 Function to find a free block with the smallest size that is 
	greater than request. Returns NULL if attempt failed. */
struct block_meta* find_best(size_t request) {
	coalesce_blocks();
	struct block_meta* iter = head;
	struct block_meta* best_fiting_block = NULL;
	while (iter) {
		if (iter->size >= ALIGN(request) && iter->status == STATUS_FREE) {
			if (!best_fiting_block) {
				best_fiting_block = iter;	// Firt fitting block was found
			} else if (iter->size < best_fiting_block->size && iter->status == STATUS_FREE) {
				best_fiting_block = iter;	// Better block found
			}
		}
		iter = iter->next;
	}
	return best_fiting_block;
}

/* 	Insert in the list the block passed as a parameter. 
	The list is ordered based on the address of each block */
void insertAlloced(struct block_meta* block) {
	if (!head) {
		head = block;
		return;
	}
	struct block_meta* iter = head;
	while (iter && iter < block) {
		iter = iter->next;
	}
	if (!iter) { // Add the block to the back of the list.
		iter = head;
		while (iter->next)
			iter = iter->next;
		iter->next = block;
		block->prev = iter;
		return;
	}
	if (iter == head) { // Add before the head of the list.
		block->next = head;
		head->prev = block;
		head = block;
		return;
	}
	// Connect block to neighbours
	block->next = iter;
	block->prev = iter->prev;
	// Connect neighbours to block
	block->prev->next = block;
	block->next->prev = block;
}

/* Returs the last free block in list, if exists. */
struct block_meta *getLastExpandableBlock() {
	if (!head) {
		return NULL;
	}
	if (head->status == STATUS_MAPPED)
		return NULL;
	struct block_meta *iter = head;
	while (iter->next && iter->next->status != STATUS_MAPPED) {
		iter = iter->next;
	}
	if (iter->status == STATUS_FREE)
		return iter;
	DIE(iter->status == STATUS_MAPPED, "expanding mapped block, this block should be alloced using sbrk");
	return NULL;
}

/*	 Returns a (void *)ptr to a region of memmory allocated on heap 
	of size <size>. */
void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;
	if (size < MMAP_THRESHOLD - META_SIZE) {
		struct block_meta *new_block = find_best(size);
		if (new_block) {
			new_block = split(new_block, size);
			return (void *)(new_block) + META_SIZE;
		} else {
			if (prealloced_mem == 0) {
				// Prealloc memmory if the program has no memmory alloced before the
				// program break
				void *new_brk = sbrk(ALIGN(MMAP_THRESHOLD));
				DIE(new_brk == (void *)-1, "sbrk filed");
				new_block = (struct block_meta *)new_brk;
				new_block->next = new_block->prev = NULL;
				new_block->size = ALIGN(MMAP_THRESHOLD) - META_SIZE;
				new_block->status = STATUS_FREE;
				insertAlloced(new_block);
				new_block = split(new_block, size);
				prealloced_mem = 1;
				return (void *)(new_block) + META_SIZE;
			} else {
				// Memmory is already prealloced and full. SBRK request.
				// First try to expand the last block on the list if possible.
				struct block_meta* last_exp_block = getLastExpandableBlock();
				if (!last_exp_block) {
					// The last block is not free
					void *new_brk = sbrk(ALIGN(size) + META_SIZE);
					DIE(new_brk == (void *)-1, "sbrk failed");
					new_block = (struct block_meta *)new_brk;
					new_block->next = new_block->prev = NULL;
					new_block->size = ALIGN(size);
					new_block->status = STATUS_ALLOC;
					insertAlloced(new_block);
					return (void *)(new_block) + META_SIZE;
				} else {
					// The last block can be expanded
					DIE(ALIGN(size) - last_exp_block->size <= 0, "no need to expand");
					sbrk(ALIGN(size) - last_exp_block->size);
					last_exp_block->status = STATUS_ALLOC;
					last_exp_block->size = ALIGN(size);
					return (void*)last_exp_block + META_SIZE;
				}
			}
		}
	} else {
		void *mem = mmap(NULL, META_SIZE + ALIGN(size), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		DIE(mem == MAP_FAILED, "map failed");
		struct block_meta *new_block = (struct block_meta *)mem;
		new_block->size = ALIGN(size);
		new_block->next = new_block->prev = NULL;
		new_block->status = STATUS_MAPPED;
		return (void *)(new_block) + META_SIZE;
	}
}

/* Frees the memmory that *ptr points to. */
void os_free(void *ptr)
{
	if (!ptr)
		return;
	struct block_meta* block = (struct block_meta*)(ptr - META_SIZE);
	DIE((block->status != STATUS_ALLOC) && (block->status != STATUS_FREE) &&
		(block->status != STATUS_MAPPED), "free | invalid pointer");
	if (block->status == STATUS_FREE) {
		puts("ERROR: Double free!");
		return;}
	if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
		return;
	}
	if (block->status == STATUS_MAPPED) {
		DIE(munmap(block, block->size + META_SIZE) == -1, "Unmap failed!");
	}
}

/*	 Returns a pointer to memmory which has been initialzied with zeroes.
	The size of the memmory region is equal to nmbemb * size.
	nmemb = number of elemetns; size = size of each element; */
void *os_calloc(size_t nmemb, size_t size)
{
	size_t request = nmemb * size;
	if (request == 0)
		return NULL;
	if (ALIGN(request) + META_SIZE >= 4096) { // mmap syscall
		void* ptr = mmap(NULL, ALIGN(request) + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		struct block_meta* new_block = ptr;
		new_block->size = ALIGN(request);
		new_block->next = new_block->prev = NULL;
		new_block->status = STATUS_MAPPED;
		return (void *)new_block + META_SIZE;
	} else { // sbrk syscall
		struct block_meta *new_block = find_best(request);
		if (new_block) {
			new_block = split(new_block, request);
			memset((void *)(new_block) + META_SIZE, 0, new_block->size);
			return (void *)(new_block) + META_SIZE;
		} else {
			if (prealloced_mem == 0) {
				// Prealloc memmory just once in the program
				void *new_brk = sbrk(ALIGN(MMAP_THRESHOLD));
				DIE(new_brk == (void *)-1, "sbrk filed");
				new_block = (struct block_meta *)new_brk;
				new_block->next = new_block->prev = NULL;
				new_block->size = ALIGN(MMAP_THRESHOLD) - META_SIZE;
				new_block->status = STATUS_FREE;
				insertAlloced(new_block);
				new_block = split(new_block, request);
				prealloced_mem = 1;
				memset((void *)(new_block) + META_SIZE, 0, new_block->size);
				return (void *)(new_block) + META_SIZE;
			} else {
				// Memmory is already prealloced and full. SBRK request
				// Try to expand the last block if possible.
				struct block_meta* last_exp_block = getLastExpandableBlock();
				if (!last_exp_block) { // Last block is NOT free
					void *new_brk = sbrk(ALIGN(request) + META_SIZE);
					DIE(new_brk == (void *)-1, "sbrk failed");
					new_block = (struct block_meta *)new_brk;
					new_block->next = new_block->prev = NULL;
					new_block->size = ALIGN(request);
					new_block->status = STATUS_ALLOC;
					insertAlloced(new_block);
					memset((void *)new_block + META_SIZE, 0, ALIGN(request));
					return (void *)(new_block) + META_SIZE;
				} else { // Can expand the block.
					DIE(ALIGN(request) - last_exp_block->size <= 0, "no need to expand");
					sbrk(ALIGN(request) - last_exp_block->size);
					last_exp_block->status = STATUS_ALLOC;
					last_exp_block->size = ALIGN(request);
					memset((void *)last_exp_block + META_SIZE, 0, ALIGN(request));
					return (void*)last_exp_block + META_SIZE;
				}
			}
		}
	}
}

/*	 Reallocates the memmory pointed to by *ptr to a new one 
	of size <size>. */
void *os_realloc(void *ptr, size_t size)
{
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	if (ptr == NULL)
		return os_malloc(size);
	if (!ptr)
		return os_malloc(size);
	struct block_meta* block = (struct block_meta *)(ptr - META_SIZE);
	if (block->status == STATUS_FREE) {
		return NULL;
	}
	coalesce_blocks();
	if (block->status == STATUS_MAPPED) {
		if (block->size == ALIGN(size))
			return ptr;
		void *new_ptr = os_malloc(size);
		memcpy(new_ptr, ptr, block->size > ALIGN(size) ? size : block->size);
		os_free(ptr);
		return new_ptr;
	}
	if (block->status == STATUS_ALLOC) {
		if (block->size == ALIGN(size)) {
			return ptr;
		}
		if (block->size > ALIGN(size)) {
			block = split(block, size);
			return (void *)block + META_SIZE;
		}
		if (!block->next) {
			sbrk(ALIGN(size) - block->size);
			block->size = ALIGN(size);
			return (void *)block + META_SIZE;
		}
		if (block->next->status == STATUS_FREE && block->size + META_SIZE + block->next->size >= ALIGN(size)) {
			struct block_meta* tmp = block->next;
			block->size = block->size + META_SIZE + tmp->size;
			block->next = tmp->next;
			if (tmp->next) {
				tmp->next->prev = block;
			}
			block = split(block, size);
			return (void *)block + META_SIZE;
		}
		void *new_ptr = os_malloc(ALIGN(size));
		memcpy(new_ptr, ptr, block->size);

		block->status = STATUS_FREE;
		return new_ptr;
	}
	DIE(1, "Invalid (void *)ptr");
}
