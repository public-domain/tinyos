#include <kern/thread.h>
#include <kern/pit.h>
#include <kern/page.h>
#include <kern/pagetbl.h>
#include <kern/gdt.h>
#include <kern/kernasm.h>
#include <kern/vmem.h>
#include <kern/kernlib.h>
#include <kern/chardev.h>
#include <kern/netdev.h>
#include <kern/timer.h>
#include <kern/lock.h>
#include <kern/elf.h>
#include <kern/file.h>
#include <kern/fs.h>

#include <net/socket/socket.h>
#include <net/inet/inet.h>
#include <net/util.h>

static struct tss tss;

struct thread *current = NULL;
static pid_t pid_next = 0; 

static struct list_head run_queue;
static struct list_head wait_queue;

void thread_a(void *arg) {
  printf("arg = %d\n", (int)arg);
}

void thread_echo(void *arg) {
  char data;
  while(1) {
    if(chardev_read(DEVNO(1, 0), &data, 1) == 1) {
      if(data == 0x7f) {
        chardev_write(DEVNO(1, 0), "\b \b", 3);
      } else if(data == '\r') {
        chardev_write(DEVNO(1, 0), "\n", 1);
      } else {
        chardev_write(DEVNO(1, 0), &data, 1);
      }
      //if(sendto(sock, &data, 1, 0, (struct sockaddr *)&addr) < 0)
        //puts("sendto failed");
    }
  }


/*
  struct socket *sock;
  struct sockaddr_in addr;

  sock = socket(PF_INET, SOCK_DGRAM);
  if(sock == NULL) {
    puts("failed to open socket");
    return;
  }

  addr.family = PF_INET;
  addr.port = hton16(54321);
  addr.addr = IPADDR(192,168,4,1);

  u8 data;
  while(1) {
    if(chardev_read(0, &data, 1) == 1) {
      chardev_write(0, &data, 1);
      //if(sendto(sock, &data, 1, 0, (struct sockaddr *)&addr) < 0)
        //puts("sendto failed");
    }
  }

  close(sock);
*/
}

void thread_test(void *arg) {
  struct file *sock;
  struct sockaddr_in addr;

  char buf[2048];

  sock = socket(PF_INET, SOCK_DGRAM);
  if(sock == NULL) {
    puts("failed to open socket");
    return;
  }

  addr.family = PF_INET;
  addr.port = hton16(12345);
  addr.addr = INADDR_ANY;

  bind(sock, (struct sockaddr *)&addr);

  memset(buf, 0, sizeof(buf));
  while(1) {
    int len = recv(sock, buf, sizeof(buf), 0);
    buf[len] = '\0';
    //printf("%s\n", buf);
    printf("received %d bytes\n", len);
  }

  close(sock);
}

void thread_test2(void *arg) {
  struct file *sock0;
  struct sockaddr_in addr;
  struct sockaddr_in client;
  struct file *sock;

  sock0 = socket(PF_INET, SOCK_STREAM);
  puts("socket ok");

  addr.family = PF_INET;
  addr.port = hton16(12345);
  addr.addr = INADDR_ANY;
  bind(sock0, (struct sockaddr *)&addr);
  puts("bind ok");

  listen(sock0, 5);
  puts("listening...");

  sock = accept(sock0, (struct sockaddr *)&client);
  puts("accepted");

  u8 buf[2048];
  int len;
  while((len = recv(sock, buf, sizeof(buf), 0)) > 0) {
    printf("tcp: received %d byte\n", len);
    buf[len] = '\0';
    puts(buf);
    send(sock, "ok. ", 4, 0);
  }

puts("tcp connection closed.");

  close(sock);

  close(sock0);

}


void thread_b(void *arg) {
  if(fs_mountroot(ROOTFS_TYPE, ROOTFS_DEV))
    puts("fs: failed to mount");
  else
    puts("fs: mount succeeded");


  //thread_run(kthread_new(thread_a, 3, "thread_a"));
  thread_run(kthread_new(thread_echo, NULL, "echo task"));
  //thread_run(kthread_new(thread_test, NULL, "udp test task"));
  //thread_run(kthread_new(thread_test2, NULL, "tcp test task"));

  struct file *f3 = open("/wamcompiler.lisp", O_RDWR);
  if(f3 == NULL) {
    puts("open failed.");
    return;
  }
 
  struct file *f = open("/big.txt", O_RDWR);
  if(f == NULL) {
    puts("open failed.");
    return;
  }

  char buf[128];
  size_t count;
  off_t off = 0x620000;
  lseek(f, off, SEEK_SET);
  count = read(f, buf, 20);
  printf("%d bytes read.\n", count);
  for(int i=0; i<count; i++)
    printf("%c", buf[i]);
  lseek(f, off, SEEK_SET);
  char str[] = "write call test!!!";
  printf("\nwrite : %x\n", write(f, str, sizeof(str)));
  lseek(f, off, SEEK_SET);
  count = read(f, buf, 20);
  printf("\n\n(2nd) %d bytes read.\n", count);
  for(int i=0; i<count; i++)
    printf("%c", buf[i]);
  printf("unlink: %d\n", unlink("/hello"));
  //printf("mknod: %d\n", mknod("/foo", S_IFREG, 0));
  struct file *f2 = open("/", O_RDWR | O_DIRECTORY);
  struct dirent dirents[3];
  size_t bytes;
  if(!f2) {
    puts("open failed");
  }
  puts("");

  while(bytes = getdents(f2, dirents, sizeof(dirents))) {
    for(int i=0; i<bytes/sizeof(struct dirent); i++) {
      printf("%d %s\n", (u32)dirents[i].d_vno, dirents[i].d_name);
    }
  }
}

void thread_idle(void *arg) {
  while(1)
    cpu_halt();
}

void dispatcher_init() {
  current = NULL;
  list_init(&run_queue);
  list_init(&wait_queue);

  bzero(&tss, sizeof(struct tss));
  tss.ss0 = GDT_SEL_DATASEG_0;

  gdt_init();
  gdt_settssbase(&tss);
  ltr(GDT_SEL_TSS);

  thread_run(kthread_new(thread_idle, NULL, "idle task"));
  thread_run(kthread_new(thread_b, NULL, "fs test thread"));
}

void dispatcher_run() {
  jmpto_current();
}

void kstack_setaddr() {
  tss.esp0 = (u32)((u8 *)(current->kstack) + current->kstacksize);
}

struct thread *kthread_new(void (*func)(void *), void *arg, const char *name) {
  struct thread *t = malloc(sizeof(struct thread));
  bzero(t, sizeof(struct thread));
  t->name = name;
  t->vmmap = vm_map_new();
  t->state = TASK_STATE_RUNNING;
  t->pid = pid_next++;
  t->regs.cr3 = procpdt_new();
  //prepare kernel stack
  t->kstack = page_alloc();
  bzero(t->kstack, PAGESIZE);
  t->kstacksize = PAGESIZE;
  t->regs.esp = (u32)((u8 *)(t->kstack) + t->kstacksize - 4);
  *(u32 *)t->regs.esp = (u32)arg;
  t->regs.esp -= 4;
  *(u32 *)t->regs.esp = (u32)thread_exit;
  t->regs.esp -= 4;
  *(u32 *)t->regs.esp = (u32)func;
  t->regs.esp -= 4*5;
  *(u32 *)t->regs.esp = 0x200; //initial eflags(IF=1)
  return t;
}

#define USER_STACK_BOTTOM ((vaddr_t)0xc0000000)
#define USER_STACK_SIZE ((size_t)0x1000)

int thread_exec(const char *path) {
  /*int (*entrypoint)(void) = elf32_load(vno);
  if(entrypoint == NULL)
    return -1;

  //prepare user space stack
  vm_add_area(current->vmmap, USER_STACK_BOTTOM-USER_STACK_SIZE, USER_STACK_SIZE, anon_mapper_new(), 0);
  printf("loaded: %x - %x (stack, anon mapping)\n", USER_STACK_BOTTOM-USER_STACK_SIZE, USER_STACK_BOTTOM-USER_STACK_SIZE+USER_STACK_SIZE);


  jmpto_userspace(entrypoint, USER_STACK_BOTTOM - 4);
*/
  return 0;
}


void thread_run(struct thread *t) {
  t->state = TASK_STATE_RUNNING;
  if(current == NULL)
    current = t;
  else
    list_pushback(&(t->link), &run_queue);
}

static void thread_free(struct thread *t) {
  page_free(t->kstack);
  free(t);
}

void thread_sched() {
  //printf("sched: nextpid=%d esp=%x\n", current->pid, current->regs.esp);
  switch(current->state) {
  case TASK_STATE_RUNNING:
    list_pushback(&(current->link), &run_queue);
    break;
  case TASK_STATE_WAITING:
    list_pushback(&(current->link), &wait_queue);
    break;
  case TASK_STATE_EXITED:
    thread_free(current);
    break;
  }
  struct list_head *next = list_pop(&run_queue);
  if(next == NULL) {
    puts("no thread!");
    while(1)
      cpu_halt();
  }
  current = container_of(next, struct thread, link);
}

void thread_yield() {
  IRQ_DISABLE 
  _thread_yield();
  IRQ_RESTORE
}

void thread_sleep(const void *cause) {
  //printf("thread#%d sleep\n", current->pid);
  current->state = TASK_STATE_WAITING;
  current->waitcause = cause;
  thread_yield();
}

void thread_sleep_after_unlock(void *cause, mutex *mtx) {
IRQ_DISABLE
  mutex_unlock(mtx);
  thread_sleep(cause);
IRQ_RESTORE
}


void thread_wakeup(const void *cause) {
  struct list_head *h, *tmp;
  list_foreach_safe(h, tmp, &wait_queue) {
    struct thread *t = container_of(h, struct thread, link); 
    if(t->waitcause == cause) {
      //printf("thread#%d wakeup\n", t->pid);
      t->state = TASK_STATE_RUNNING;
      list_remove(h);
      list_pushfront(h, &run_queue);
    }
  }
}

void thread_set_alarm(void *cause, u32 expire) {
  timer_start(expire, thread_wakeup, cause);
}

void thread_exit() {
  printf("thread#%d(%s) exit\n", current->pid, current->name?current->name:"???");
  current->state = TASK_STATE_EXITED;
  thread_yield();
}
