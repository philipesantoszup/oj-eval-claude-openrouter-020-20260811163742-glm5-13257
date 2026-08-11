#include "buddy.h"
#define NULL ((void *)0)

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MAX_PAGES 32768

static char *base_addr = NULL;
static int total_pages = 0;

// For each page index, store the rank of the block it belongs to
// - If the page is the head of a free block: positive rank
// - If the page is the head of an allocated block: positive rank
// - If the page is not the head: we need to determine from context
static int block_rank[MAX_PAGES];
static int is_allocated[MAX_PAGES];  // 1 if allocated, 0 if free
static int is_block_head[MAX_PAGES]; // 1 if head of a block

// Free lists: linked list of free block heads for each rank
static int free_list_head[MAX_RANK + 2];  // Head of free list for each rank
static int free_next[MAX_PAGES];          // Next pointer for free list

// Convert address to page index
static int addr_to_idx(void *p) {
    if (p == NULL) return -1;
    char *addr = (char *)p;
    if (addr < base_addr) return -1;
    long long diff = addr - base_addr;
    if (diff % PAGE_SIZE != 0) return -1;
    int idx = diff / PAGE_SIZE;
    if (idx < 0 || idx >= total_pages) return -1;
    return idx;
}

// Convert page index to address
static void *idx_to_addr(int idx) {
    return base_addr + (long long)idx * PAGE_SIZE;
}

// Get buddy index for a block at given index and rank
static int get_buddy(int idx, int rank) {
    int block_size = 1 << (rank - 1);  // 2^(rank-1) pages
    // Buddy calculation: XOR with block_size
    return idx ^ block_size;
}

// Add a free block to the free list
static void add_to_free_list(int idx, int rank) {
    free_next[idx] = free_list_head[rank];
    free_list_head[rank] = idx;
}

// Remove a free block from the free list
static void remove_from_free_list(int idx, int rank) {
    if (free_list_head[rank] == idx) {
        free_list_head[rank] = free_next[idx];
    } else {
        int prev = free_list_head[rank];
        while (prev != -1 && free_next[prev] != idx) {
            prev = free_next[prev];
        }
        if (prev != -1) {
            free_next[prev] = free_next[idx];
        }
    }
    free_next[idx] = -1;
}

// Pop the first free block of a given rank
static int pop_from_free_list(int rank) {
    int idx = free_list_head[rank];
    if (idx != -1) {
        free_list_head[rank] = free_next[idx];
        free_next[idx] = -1;
    }
    return idx;
}

// Initialize a block: mark all pages in the block
static void init_block(int idx, int rank, int allocated) {
    int block_size = 1 << (rank - 1);
    for (int i = 0; i < block_size; i++) {
        block_rank[idx + i] = rank;
        is_allocated[idx + i] = allocated;
        is_block_head[idx + i] = (i == 0) ? 1 : 0;
    }
}

// Split a free block at given index and rank into two blocks of rank-1
static int split_block(int idx, int rank) {
    remove_from_free_list(idx, rank);

    int new_rank = rank - 1;
    int half_size = 1 << (new_rank - 1);

    // First half
    init_block(idx, new_rank, 0);
    add_to_free_list(idx, new_rank);

    // Second half - this will be returned or further split
    init_block(idx + half_size, new_rank, 0);

    return idx + half_size;
}

// Try to merge a free block with its buddy
static void try_merge(int idx, int rank) {
    if (rank >= MAX_RANK) return;  // Can't merge beyond max rank

    int buddy = get_buddy(idx, rank);
    int block_size = 1 << (rank - 1);

    // Check if buddy is valid and also free with same rank
    if (buddy >= 0 && buddy < total_pages &&
        !is_allocated[buddy] && is_block_head[buddy] &&
        block_rank[buddy] == rank) {
        // Can merge!
        remove_from_free_list(idx, rank);
        remove_from_free_list(buddy, rank);

        int new_rank = rank + 1;
        int merged_idx = (idx < buddy) ? idx : buddy;

        // Initialize merged block
        init_block(merged_idx, new_rank, 0);
        add_to_free_list(merged_idx, new_rank);

        // Try to merge further
        try_merge(merged_idx, new_rank);
    }
}

int init_page(void *p, int pgcount) {
    base_addr = (char *)p;
    total_pages = pgcount;

    // Initialize all arrays
    for (int i = 0; i < pgcount; i++) {
        block_rank[i] = 0;
        is_allocated[i] = 0;
        is_block_head[i] = 0;
        free_next[i] = -1;
    }

    for (int r = 1; r <= MAX_RANK + 1; r++) {
        free_list_head[r] = -1;
    }

    // Calculate how to divide pgcount into blocks
    // We need to handle cases where pgcount is not a power of 2
    int remaining = pgcount;
    int idx = 0;

    while (remaining > 0) {
        // Find the largest power of 2 that fits
        int rank = 1;
        int size = 1;
        while (size * 2 <= remaining) {
            size *= 2;
            rank++;
        }

        // Initialize this block as free
        init_block(idx, rank, 0);
        add_to_free_list(idx, rank);

        idx += size;
        remaining -= size;
    }

    return OK;
}

void *alloc_pages(int rank) {
    // Validate rank
    if (rank < 1 || rank > MAX_RANK) {
        return (void *)(long long)(-EINVAL);
    }

    // Find a free block of the required rank or higher
    int found_rank = -1;
    for (int r = rank; r <= MAX_RANK; r++) {
        if (free_list_head[r] != -1) {
            found_rank = r;
            break;
        }
    }

    if (found_rank == -1) {
        return (void *)(long long)(-ENOSPC);
    }

    // Split blocks until we get the required rank
    int idx = pop_from_free_list(found_rank);

    while (found_rank > rank) {
        // Split: first half becomes new candidate, second half goes to free list
        int new_rank = found_rank - 1;
        int half_size = 1 << (new_rank - 1);

        // First half is our candidate (lower address)
        init_block(idx, new_rank, 0);

        // Second half goes to free list (higher address)
        init_block(idx + half_size, new_rank, 0);
        add_to_free_list(idx + half_size, new_rank);

        found_rank = new_rank;
    }

    // Mark the allocated block
    init_block(idx, rank, 1);

    return idx_to_addr(idx);
}

int return_pages(void *p) {
    int idx = addr_to_idx(p);

    if (idx < 0) {
        return -EINVAL;
    }

    if (!is_block_head[idx]) {
        return -EINVAL;
    }

    if (!is_allocated[idx]) {
        return -EINVAL;
    }

    int rank = block_rank[idx];

    // Mark as free
    init_block(idx, rank, 0);
    add_to_free_list(idx, rank);

    // Try to merge with buddy
    try_merge(idx, rank);

    return OK;
}

int query_ranks(void *p) {
    int idx = addr_to_idx(p);

    if (idx < 0) {
        return -EINVAL;
    }

    // For allocated pages, return the allocation rank
    if (is_allocated[idx]) {
        // Find the head of the block
        int head_idx = idx;
        while (!is_block_head[head_idx]) {
            head_idx--;
        }
        return block_rank[head_idx];
    }

    // For free pages, return the rank of the free block containing it
    // Find the head of the free block
    int head_idx = idx;
    while (!is_block_head[head_idx]) {
        head_idx--;
    }
    return block_rank[head_idx];
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    int count = 0;
    int idx = free_list_head[rank];
    while (idx != -1) {
        count++;
        idx = free_next[idx];
    }
    return count;
}
