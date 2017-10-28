#include <kern/kernlib.h>
#include <kern/vga.h>
#include <kern/trap.h>
#include <kern/page.h>
#include <kern/idt.h> 
#include <kern/pic.h> 
#include <kern/pci.h> 
#include <kern/pit.h> 
#include <kern/malloc.h> 
#include <kern/kernasm.h>
#include <kern/pagetbl.h>
#include <kern/vmem.h>
#include <kern/fs.h>
#include <kern/gdt.h>
#include <kern/task.h>
#include <kern/blkdev.h>
#include <kern/chardev.h>
#include <kern/netdev.h>


void _init(void);

KERNENTRY void kernel_main(void) {
  a20_enable();
	vga_init();
	puts("hello, world!");
  page_init();
  printf("%d MB(%d pages) free\n", (page_getnfree()*4)/1024, page_getnfree());
  idt_init();
  idt_register(13, IDT_INTGATE, gpe_inthandler);
  idt_register(14, IDT_INTGATE, pf_inthandler);
  idt_register(0x80, IDT_INTGATE, syscall_inthandler);
  pic_init();
  pagetbl_init();
  vmem_init();
  pit_init();
  pci_init();
  blkdev_init();
  chardev_init();
  netdev_init();

  _init();

  task_init();

  while(1);
}


