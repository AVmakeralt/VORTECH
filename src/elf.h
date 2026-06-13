/*
 * VORTECH Compiler - ELF64 Format Definitions
 *
 * Minimal ELF64 structures for emitting relocatable object files.
 * No external assembler. No GCC. We write machine code directly.
 */
#ifndef VORTECH_ELF_H
#define VORTECH_ELF_H

#include <stdint.h>

/* ---- ELF Identification ---- */
#define EI_NIDENT    16
#define ELFMAG0      0x7f
#define ELFMAG1      'E'
#define ELFMAG2      'L'
#define ELFMAG3      'F'
#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1
#define ELFOSABI_NONE 0

/* ---- ELF Types ---- */
#define ET_REL       1

/* ---- Machine Types ---- */
#define EM_X86_64    62

/* ---- Section Types ---- */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4

/* ---- Section Flags ---- */
#define SHF_WRITE    0x1
#define SHF_ALLOC    0x2
#define SHF_EXECINSTR 0x4

/* ---- Symbol Binding ---- */
#define STB_LOCAL    0
#define STB_GLOBAL   1

/* ---- Symbol Type ---- */
#define STT_NOTYPE   0
#define STT_FUNC     2
#define STT_FILE     4

/* ---- Special Section Indices ---- */
#define SHN_UNDEF    0
#define SHN_ABS      0xfff1

/* ---- x86-64 Relocation Types ---- */
#define R_X86_64_64    1   /* Direct 64-bit */
#define R_X86_64_PC32  2   /* PC-relative 32-bit */
#define R_X86_64_32    10  /* Direct 32-bit zero-extended */
#define R_X86_64_PLT32 4   /* 32-bit PLT address */

/* ---- ELF64 Header (64 bytes) ---- */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

/* ---- Section Header (64 bytes) ---- */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

/* ---- Symbol Table Entry (24 bytes) ---- */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* ---- Relocation Entry with Addend (24 bytes) ---- */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

/* ---- Relocation Info Macros ---- */
#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((uint32_t)(i))
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) | (uint32_t)(type))

/* ---- Symbol Info Macros ---- */
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)
#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xf))

#endif /* VORTECH_ELF_H */
