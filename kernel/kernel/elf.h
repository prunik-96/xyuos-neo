#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// Load a static ELF64 image into the address space `pml4_phys`, allocating and
// mapping user pages for each PT_LOAD segment. The image is written through
// the target space's page tables, so this does NOT require `pml4_phys` to be
// the address space currently in CR3 -- the caller can build a process before
// ever switching to it.
//
// Returns 0 on success and stores the entry point in *entry_out; -1 on any
// malformed image or allocation failure.
int elf_load_into(uint64_t pml4_phys, const void *data, uint64_t size,
                  uint64_t *entry_out);

#endif
