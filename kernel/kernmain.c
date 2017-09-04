#include <stddef.h>
#include <stdint.h>
#include "common.h"
#include "vga.h"
#include "trap.h"
#include "page.h"
#include "idt.h" 
#include "pic.h" 
#include "pit.h" 
#include "malloc.h" 
#include "kernasm.h"
#include "pagetbl.h"
#include "vmem.h"

KERNENTRY void kernel_main(void) {
	vga_init();
	puts("hello, world!");
  page_init();
  printf("%d pages free\n", page_getnfree());
  idt_init();
  pic_init();
  idt_register(13, IDT_INTGATE, gpe_isr);
  idt_register(14, IDT_INTGATE, pf_isr);
  pagetbl_init();
  vmem_init();
  //pit_init();
  sti();
  //gengpe();
  struct foo {
    int a, b;
  } *x;
  x = malloc(sizeof(struct foo));
  if(x == NULL) {
    puts("malloc() failed.");
  } else {
    x->a = 6;
    x->b = 123;
    printf("%d, %d\n", x->a, x->b);
    free(x);
    puts(".....");
  }
  volatile int *a = (int*)0x6600;
  *a = 12345;
  printf("*a = %d\n", *a);
  *a = 1234;
  printf("*a = %d\n", *a);
  puts("bye.");
while(1);
}


