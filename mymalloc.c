#include "mymalloc.h"
#include "errno.h"
#include "sys/mman.h"
#include "string.h"

#define GET_SIZE(block) (((Block *)block)->size & ~0x7)
#define IS_ALLOCATED(block) (((Block *)block)->size & 0x1)
#define GET_PREVIOUS(block) ((Block **)((Block *)block + 1))
#define GET_NEXT(block) ((Block **)((Block *)block + 2))
#define GET_HEADER(p) (Block *)((Block *)p - 1)
#define GET_FOOTER(p) ((Block *) ((char *) GET_HEADER(p) + GET_SIZE(p) - 2 * kAlignment))
#define RETURN_PTR(block) (void *)((Block *)block + 1)
#define SET_ALLOCATED(block) (((Block *)block)->size |= 0x1)
#define SET_UNALLOCATED(block) (((Block *)block)->size &= ~0x6)
#define SET_SIZE(block, _size, alloc) (((Block *)block)->size = ((_size) | (alloc)))
#define GET_DEFENCE_E(chunk) ((Block *) ((char *) chunk + ARENA_SIZE - kAlignment))
#define GET_DATA_SIZE(block) (GET_SIZE((block)) - 2 * kAlignment)
#define GET_NEXT_BLOCK(block) (Block *)((char*) block + GET_SIZE(block))
#define GET_PREVIOUS_BLOCK(block) ((Block *)((char*) block - GET_SIZE((Block *)block - 1)))

#define GET_INDEX(size) (((size_t) size / kMinAllocationSize) - 2)

const size_t kMaxAllocationSize = ARENA_SIZE - 4 * kAlignment;

inline static size_t round_up(size_t size, size_t alignment) {
  const size_t mask = alignment - 1;
  return (size + mask) & ~mask;
}

typedef struct Block {
  // last 3 bits for allocated or not.
  size_t size;
} Block;

/*
 * function declarations begin
 */
void *my_malloc(size_t size);
void my_free(void *ptr);
int init_chunk();
void init_fence(Block *chunk);
void *allocate(Block *block, size_t size);
void coalesce(void *ptr);
void delete_from_list(Block *block, int id);
void add_to_list(Block *block, int id);
void update_footer(Block *block);
Block *extend(Block *block, size_t size);
Block *find_fit(size_t size);
Block *split_block(Block *block, size_t size);

/*
 * global variables begin
 */
void *chunk_b;
Block *fence_b;
Block *fence_e;
Block *free_list[N_LISTS];
Block *other_list;


/*
 * Function implementations begin
 */

/*
 * Update footer information by copying the header info
 */
void update_footer(Block *block) {
  Block *footer = GET_NEXT_BLOCK(block);
  footer = footer - 1;
  SET_SIZE(footer, GET_SIZE(block), IS_ALLOCATED(block));
}

void *my_malloc(size_t size) {
  // return NUll if requested size is 0
  if (size == 0)
    return NULL;
  // return NULL if requested size larger than Max allocation size
  if (size > kMaxAllocationSize) {
    errno = ENOMEM;
    return NULL;
  }
  // if there is no existed chunk requested from mmap, init it
  if (chunk_b == NULL) {
    init_chunk();
  }
  // round up the size
  size_t total_size = round_up(size, kMinAllocationSize);
  // to save previous and next pointer, min rounded allocation should be 16 bytes
  total_size = total_size == 8 ? 16 : total_size;
  // if there is no enough space, calculate extra size to request
  size_t extend_size;
  // find a block that can fit the requested size
  void *ptr = find_fit(total_size);
  // if not found, extend the chunk by requesting extra size using mmap
  if (!ptr) {
    extend_size = (total_size / ARENA_SIZE + 1) * ARENA_SIZE;
    Block *new_block = extend(fence_b, extend_size);
    return new_block;
  }
  return ptr;
}

/*
 * Init the chunk by using mmap
 */
int init_chunk() {
  // init free_list to NULL
  for (int i = 0; i < N_LISTS; i++) {
    free_list[i] = NULL;
  }
  // request chunk
  chunk_b = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
  // if fails, return -1
  if (!chunk_b)
    return -1;
  // set the requested chunk to 0
  memset(chunk_b, 0, ARENA_SIZE);
  // init fences
  init_fence(chunk_b);
  // set header
  chunk_b = (void *) ((Block *) chunk_b + 1);
  SET_SIZE(chunk_b, ARENA_SIZE - 2 * kAlignment, 0);
  // update footer
  update_footer((Block *) chunk_b);
  // add to free list
  add_to_list(chunk_b, GET_INDEX(GET_DATA_SIZE(chunk_b)));
  return 0;
}

/*
 * Init fence blocks by setting them to blocks with size 8, allocated
 */
void init_fence(Block *block) {
  fence_b = block;
  fence_e = ((Block *) ((char *) chunk_b + ARENA_SIZE - kAlignment));
  SET_SIZE(fence_b, kAlignment, 1);
  SET_SIZE(fence_e, kAlignment, 1);
}

/*
 * Using first fit to find appropriate block
 */
Block *find_fit(size_t size) {
  Block *new_block = NULL;
  // use multiple free list
  int id = GET_INDEX(size);
  for (int i = id; i < N_LISTS; i++) {
    if (free_list[i] != NULL) {
      new_block = allocate(free_list[i], size);
      break;
    }
  }
  // if there is no proper block, find it in other size list
  if (!new_block) {
    Block *block = other_list;
    int i = 0;
    while (block != NULL && i < 100) {
      if ((size + 4 * kAlignment + 2 * kMinAllocationSize < GET_SIZE(block)) ||
          (size + 2 * kAlignment == GET_SIZE(block))) {
        new_block = allocate(block, size);
        break;
      }
      block = *GET_NEXT(block);
      i++;
    }
  }
  return new_block;
}

/*
 * If the size is large enough, allocate the block to return
 */
void *allocate(Block *block, size_t size) {
  // if the size is exactly the same with requested size, use the block
  if (GET_SIZE(block) == size + 2 * kAlignment) {
    SET_ALLOCATED(block);
    update_footer(block);
    // delete the block from the list
    delete_from_list(block, GET_INDEX(GET_DATA_SIZE(block)));
    // clear the previous and next pointer
    memset(block + 1, 0, 2 * kAlignment);
    return RETURN_PTR(block);
  } else if (size + 4 * kAlignment + 2 * kMinAllocationSize < GET_SIZE(block)) { // else split the block
    Block *new_block = split_block(block, size);
    // clear the previous and next pointer
    memset(new_block + 1, 0, 2 * kAlignment);
    return RETURN_PTR(new_block);
  } else {
    return NULL;
  }
}

/*
 * Delete block from list (other size list and multiple free lists)
 */
void delete_from_list(Block *block, int id) {
  if (*GET_NEXT(block)) {
    *GET_PREVIOUS(*GET_NEXT(block)) = *GET_PREVIOUS(block);
  }
  if (*GET_PREVIOUS(block)) {
    *GET_NEXT(*GET_PREVIOUS(block)) = *GET_NEXT(block);
  } else {
    // if at the beginning, update the list accordingly (other size list or multiple list)
    if (id < N_LISTS)
      free_list[id] = *GET_NEXT(block);
    else
      other_list = *GET_NEXT(block);
  }
}

/*
 * Add the block to list (other size list and multiple free lists)
 */
void add_to_list(Block *block, int id) {
  // update the list accordingly (other size list or multiple free lists)
  Block **list;
  if (id < N_LISTS)
    list = &free_list[id];
  else
    list = &other_list;
  // add the block to list
  if (!(*list)) {
    *list = block;
    *GET_NEXT(block) = NULL;
    *GET_PREVIOUS(block) = NULL;
  } else {
    *GET_NEXT(block) = *list;
    *GET_PREVIOUS(*list) = block;
    *list = block;
    *GET_PREVIOUS(block) = NULL;
  }

}

/*
 * Split the block and return the right one
 */
Block *split_block(Block *block, size_t size) {
  // update list
  int id = GET_INDEX(GET_DATA_SIZE(block));
  delete_from_list(block, id);
  SET_SIZE(block, GET_SIZE(block) - size - 2 * kAlignment, 0);
  update_footer(block);
  add_to_list(block, GET_INDEX(GET_DATA_SIZE(block)));

  // update new block
  Block *new_block = GET_NEXT_BLOCK(block);
  SET_SIZE(new_block, size + 2 * kAlignment, 1);
  update_footer(new_block);
  return new_block;
}

/*
 * Extend the chunk by reusing mmap
 */
Block *extend(Block *extend, size_t size) {
  chunk_b = mmap(extend,
                 (char *) fence_e + kAlignment - (char *) fence_b + 2 * kAlignment + size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 0,
                 0);
  SET_SIZE(fence_e, size + 2 * kAlignment, 1);
  // update the block and fence at the end
  Block *new_block = fence_e;
  update_footer(new_block);
  fence_e = (Block *) ((char *) fence_e + size + 2 * kAlignment);
  SET_SIZE(fence_e, kAlignment, 1);
  return new_block;
}

void my_free(void *ptr) {
  // if NULL, do nothing
  if (!ptr)
    return;
  // set the memory to 0 first
  memset(ptr, 0, GET_DATA_SIZE(GET_HEADER(ptr)));
  // coalesce
  coalesce(ptr);
}

/*
 * Coalesce the blocks accordingly
 */
void coalesce(void *ptr) {
  Block *block = GET_HEADER(ptr);
  Block *pre = GET_PREVIOUS_BLOCK(block);
  Block *next = GET_NEXT_BLOCK(block);
  int pre_alloc = IS_ALLOCATED(pre);
  int next_alloc = IS_ALLOCATED(next);
  size_t size = GET_SIZE(block);
  // pre alloc, next free
  if (pre_alloc == 1 && next_alloc == 0) {
    // delete from list first
    delete_from_list(next, GET_INDEX(GET_DATA_SIZE(next)));
    size_t next_size = GET_SIZE(next);
    // clear pointers
    memset(GET_NEXT_BLOCK(block) - 1, 0, 4 * kAlignment);
    // update new size
    SET_SIZE(block, GET_SIZE(block) + next_size, 0);
    // add the free block to new list
    add_to_list(block, GET_INDEX(GET_DATA_SIZE(block)));
    // update footer
    update_footer(block);
  } else if (pre_alloc == 0 && next_alloc == 1) { // pre not alloc, next alloc
    delete_from_list(pre, GET_INDEX(GET_DATA_SIZE(pre)));
    memset(GET_NEXT_BLOCK(pre) - 1, 0, 4 * kAlignment);
    SET_SIZE(pre, GET_SIZE(pre) + size, 0);
    add_to_list(pre, GET_INDEX(GET_DATA_SIZE(pre)));
    update_footer(pre);
  } else if (pre_alloc == 0 && next_alloc == 0) { // pre and next all not allocated
    delete_from_list(next, GET_INDEX(GET_DATA_SIZE(next)));
    delete_from_list(pre, GET_INDEX(GET_DATA_SIZE(pre)));
    size_t next_size = GET_SIZE(next);
    memset(GET_NEXT_BLOCK(pre) - 1, 0, 4 * kAlignment);
    SET_SIZE(pre, size + GET_SIZE(pre) + next_size, 0);
    add_to_list(pre, GET_INDEX(GET_DATA_SIZE(pre)));
    update_footer(pre);
  } else { // pre and next all allocated
    SET_SIZE(block, GET_SIZE(block), 0);
    add_to_list(block, GET_INDEX(GET_DATA_SIZE(block)));
    update_footer(block);
  }
}
