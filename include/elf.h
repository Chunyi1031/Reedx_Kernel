/*
 * 由glibc elf.h删减
 */
#ifndef _ELF_H
#define _ELF_H 1

#include <types.h>

/* 基础类型(仅 64 位) */
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;

#define EI_NIDENT (16)

/* ELF 文件头 */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* 魔数与标识 */
    Elf64_Half    e_type;             /* 目标文件类型 */
    Elf64_Half    e_machine;          /* 架构 */
    Elf64_Word    e_version;          /* 文件版本 */
    Elf64_Addr    e_entry;            /* 入口虚拟地址 */
    Elf64_Off     e_phoff;            /* 程序头表文件偏移 */
    Elf64_Off     e_shoff;            /* 节头表文件偏移 */
    Elf64_Word    e_flags;            /* 处理器相关标志 */
    Elf64_Half    e_ehsize;           /* ELF 头大小 */
    Elf64_Half    e_phentsize;        /* 程序头表项大小 */
    Elf64_Half    e_phnum;            /* 程序头表项数 */
    Elf64_Half    e_shentsize;        /* 节头表项大小 */
    Elf64_Half    e_shnum;            /* 节头表项数 */
    Elf64_Half    e_shstrndx;         /* 节名字符串表索引 */
} Elf64_Ehdr;

/* 程序段头 */
typedef struct {
    Elf64_Word    p_type;             /* 段类型 */
    Elf64_Word    p_flags;            /* 段标志 */
    Elf64_Off     p_offset;           /* 段文件偏移 */
    Elf64_Addr    p_vaddr;            /* 段虚拟地址 */
    Elf64_Addr    p_paddr;            /* 段物理地址 */
    Elf64_Xword   p_filesz;           /* 段在文件中的大小 */
    Elf64_Xword   p_memsz;            /* 段在内存中的大小 */
    Elf64_Xword   p_align;            /* 段对齐 */
} Elf64_Phdr;

/* e_ident 数组索引 */
#define EI_MAG0    0
#define ELFMAG0    0x7f
#define EI_MAG1    1
#define ELFMAG1    'E'
#define EI_MAG2    2
#define ELFMAG2    'L'
#define EI_MAG3    3
#define ELFMAG3    'F'
#define ELFMAG     "\177ELF"
#define SELFMAG    4

#define EI_CLASS   4
#define ELFCLASSNONE 0
#define ELFCLASS32  1
#define ELFCLASS64  2

#define EI_DATA    5
#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EI_VERSION 6
#define EI_OSABI   7
#define EI_ABIVERSION 8
#define EI_PAD     9

/* e_type 取值 */
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2
#define ET_DYN    3
#define ET_CORE   4

/* e_machine 取值(仅 x86-64) */
#define EM_NONE   0
#define EM_X86_64 62

/* p_type 取值 */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

/* p_flags 取值 */
#define PF_X  (1 << 0)
#define PF_W  (1 << 1)
#define PF_R  (1 << 2)

/* 辅助向量(auxv)类型 */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_HWCAP   16
#define AT_SECURE  23
#define AT_RANDOM  25

#endif /* _ELF_H */
