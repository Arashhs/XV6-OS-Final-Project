#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"


int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_ticketlockInit(void)
{
  return ticketlockInit();
}

int
sys_ticketlockTest(void)
{
  return ticketlockTest();
}

int
sys_rwinit(void)
{
  return rwinit();
}

int
sys_rwtest(void)
{
  uint pattern;
  argptr(0, (void *)&pattern, sizeof(pattern));
  if(pattern != 0 && pattern != 1)
    return -1;
  return rwtest(pattern);
}

int
sys_createThread(void)
{
  void (*func)();
  void* stack;

  if(argptr(0, (void*)&func, sizeof(void*)) < 0)
    return -1;
  if(argptr(3, (void*)&stack, sizeof(void*)) < 0)
    return -1;


  return createThread(func, stack);

}

int
sys_getThreadID(void)
{
  if (mythread() && myproc())
    return mythread()->tid;

  return -1;
}

void
sys_exitThread(void)
{
  exitThread();

}

int 
sys_joinThread(void)
{
  int tid;
  if(argint(0, &tid) < 0)
    return -1;

  return joinThread(tid);

}
