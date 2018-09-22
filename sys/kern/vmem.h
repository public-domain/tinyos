#pragma once

#include <stdint.h>
#include <stddef.h>
#include <kern/fs.h>

struct mapper;
struct vm_map;

struct vm_map {
  struct vm_area *area_list;
  u32 flags;
};

struct vm_area {
  vaddr_t start;
  off_t offset;
  size_t size;
  u32 flags;
  struct mapper *mapper;
  struct vm_area *next;
  int ref;
};

struct mapper_ops {
  void *(*request)(struct mapper *m, vaddr_t offset);
};

struct mapper {
  const struct mapper_ops *ops;
  struct vm_area *area;
};

struct vm_map *vm_map_new(void);
void vm_show_area(struct vm_map *map);
int vm_add_area(struct vm_map *map, vaddr_t start, size_t size, struct mapper *mapper, u32 flags);
struct vm_area *vm_findarea(struct vm_map *map, vaddr_t addr);
void vmem_init(void);

struct mapper *anon_mapper_new(void);
struct mapper *file_mapper_new(struct file *file, off_t file_off, size_t len);
