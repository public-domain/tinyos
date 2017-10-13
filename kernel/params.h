#pragma once

#define KERN_VMEM_ADDR					0xc0000000u
#define MEMORYMAP_ADDR					0x500u
#define KERN_CODE_ADDR					0x7e00u
#define KERN_STACK_ADDR					0x7bffu
#define PROTMEM_ADDR						0x100000u
#define KERN_STRAIGHT_MAP_SIZE	0x38000000u //896MB

#define KERN_VMEM_TO_PHYS(v)		((v) - KERN_VMEM_ADDR)
#define PHYS_TO_KERN_VMEM(p)		((p) + KERN_VMEM_ADDR)

#define PAGESIZE			4096

#define MAX_BLKDEV		64
#define MAX_CHARDEV		128
#define MAX_NETDEV		64 
#define MAX_FSINFO		32
#define MAX_MOUNT			32

#define GDT_SEL_NULL			0*8
#define GDT_SEL_CODESEG_0	1*8
#define GDT_SEL_DATASEG_0	2*8
#define GDT_SEL_CODESEG_3	3*8
#define GDT_SEL_DATASEG_3	4*8
#define GDT_SEL_TSS				5*8

