#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "ticketlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct ticketlock tl;
int sharedValue;

struct ticketlock wrt;
struct ticketlock mutex;
int rwSharedValue, readcnt;

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

struct thread*
mythread(void) {
  struct cpu *c;
  struct thread *t;
  pushcli();
  c = mycpu();
  t = c->thread;
  popcli();
  return t;
}

//clear a thread (mostly zombie threads)
void
clearThread(struct thread * t)
{
  if(t->state == INVALID || t->state == TZOMBIE)
    kfree(t->kstack);

  t->kstack = 0;
  t->tid = 0;
  t->state = TUNUSED;
  t->parent = 0;
  t->killed = 0;
}

//Allocate thread for process
struct thread*
allocthread(struct proc * p)
{
  struct thread *t;
  char *sp;
  int found = 0;

  acquire(&p->lock);
  for(t = p->threads; found != 1 && t < &p->threads[MAX_THREADS]; t++)
  {
    if(t->state == TUNUSED)
    {
      found = 1;
      t--;
    }
    else if(t->state == TZOMBIE)
    {
      clearThread(t);
      t->state = TUNUSED;
      found = 1;
      t--;
    }
  }

  if(!found)
    return 0;

  t->tid = nexttid++;
  t->state = EMBRYO;
  t->parent = p;
  t->killed = 0;

  // Allocate kernel stack.
  if((t->kstack = kalloc()) == 0){
    t->state = TUNUSED;
    return 0;
  }
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  release(&p->lock);
  return t;
}


//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
//  char *sp;
  struct thread *t;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == PUNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = USED;
  p->pid = nextpid++;

  release(&ptable.lock);

  initlock(&p->lock, p->name);

  t = allocthread(p);

  if (t == 0) { //No thread can be allocated for this process.
    p->state = PUNUSED;
    return 0;
  }

  p->threads[0] = *t;

  for(t = p->threads; t < &p->threads[MAX_THREADS]; t++)
    t->state = TUNUSED;

  return p;
}


//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct thread *t;
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  t= p->threads;
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(t->tf, 0, sizeof(*t->tf));
  t->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  t->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  t->tf->es = t->tf->ds;
  t->tf->ss = t->tf->ds;
  t->tf->eflags = FL_IF;
  t->tf->esp = PGSIZE;
  t->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  t->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  acquire(&curproc->lock);

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0) {
      release(&curproc->lock);
      return -1;
    }
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0) {
      release(&curproc->lock);
      return -1;
    }
  }
  curproc->sz = sz;
  switchuvm(curproc);
  
  release(&curproc->lock);

  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  struct thread *nt;
  struct thread *curthread = mythread();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  nt = np->threads;

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(nt->kstack);
    nt->kstack = 0;
    np->state = PUNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *nt->tf = *curthread->tf;

  // Clear %eax so that fork returns 0 in the child.
  nt->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  nt->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

//terminate all siblings for the current thread
void
killSiblings()
{
  char found;
  struct thread * t;
  struct thread * curthread = mythread();
  struct proc * curproc = myproc();
  acquire(&curproc->lock);
  if(curthread->killed == 1)
  {
    wakeup1(curthread);
    curthread->state = INVALID; // thread should make itlsef INVALID! otherwise two cpu can run on the same thread
    release(&curproc->lock);
    acquire(&ptable.lock);
    sched();
  }
  else
    for(t = curproc->threads; t < &curproc->threads[MAX_THREADS]; t++)
      if(t->tid != curthread->tid)
        t->killed = 1;

  release(&curproc->lock);

  for(;;)
  {
    found = 0;
    acquire(&curproc->lock);
    for(t = curproc->threads; t < &curproc->threads[MAX_THREADS]; t++)
      if(t->tid != curthread->tid && t->state != INVALID && t->state != TUNUSED)
        found = 1;

    release(&curproc->lock);
    if(found) // some thread does not know the process is in dying state, lets wait for him to recover
      yield();
    else      // all of the other threads are dead
      break;
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{

  //kill all siblings
  killSiblings();

  struct proc *curproc = myproc();
  struct proc *p;
  struct thread *curthread = mythread();
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == PZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = PZOMBIE;
  curthread->state = INVALID;
  
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  struct thread *t;
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == PZOMBIE){
        // Found one.
        pid = p->pid;


        for(t = p->threads; t < &p->threads[MAX_THREADS]; t++)
          clearThread(t);


        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = PUNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}


//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  struct thread *t;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != USED)
        continue;

    for(t = p->threads; t < &p->threads[MAX_THREADS]; t++){
      if(t->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      
      c->thread = t;

      switchuvm(p);
      t->state = RUNNING;

      swtch(&(c->scheduler), t->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      
      //If process state is not used, point t to p->threads
      if(p->state != USED)
        t = &p->threads[MAX_THREADS];
      c->thread = 0;
    }
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();
  struct thread *t = mythread();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(holding(&p->lock))
    panic("sched myproc()->lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(t->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&t->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  mythread()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  struct thread *t = mythread();
  
  if(p == 0 || t == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  acquire(&p->lock);
  t->chan = chan;
  t->state = SLEEPING;
  release(&p->lock);

  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == USED) {
      acquire(&p->lock);
      for(t = p->threads; t < &p->threads[MAX_THREADS]; t++)
        if(t->state == SLEEPING && t->chan == chan)
          t->state = RUNNABLE;
      release(&p->lock);
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      for(t = p->threads; t < &p->threads[MAX_THREADS]; t++)
        if(t->state == SLEEPING)
          t->state = RUNNABLE;

      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Kill the threads of given process with pid which is used in trap.c
void
killSelf()
{
  acquire(&ptable.lock);
  wakeup1(mythread());
  mythread()->state = INVALID; // thread must make itself INVALID! Otherwise two cpu can run on the same thread
  sched();
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [PUNUSED]    "punused",
  [USED]    "used",
  [PZOMBIE]    "pzombie"
  };
/*
  static char *threadStates[] = {
  [TUNUSED]    "tunused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleeping",
  [RUNNABLE]  "runnable",
  [RUNNING]  "running",
  [TZOMBIE]  "tzombie",
  [INVALID]    "invalid",
  };
*/


  int i;
  struct proc *p;
  struct thread *t;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == PUNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);

    for(t = p->threads; t < &p->threads[MAX_THREADS]; t++) {
      if(t->state == SLEEPING){
        getcallerpcs((uint*)t->context->ebp+2, pc);
        for(i=0; i<10 && pc[i] != 0; i++)
          cprintf(" %p", pc[i]);
      }
    }
    cprintf("\n");
  }
}

int
ticketlockInit(void)
{
  initTicketlock(&tl, "ticketlock");
  sharedValue = 0;
  return 1;
}

int
ticketlockTest(void)
{
  acquireTicketlock(&tl);
  sharedValue++;
  releaseTicketlock(&tl);
  return sharedValue;
}

int
rwinit(void)
{
  initTicketlock(&wrt, "wrt");
  initTicketlock(&mutex, "mutex");
  rwSharedValue = 0;
  readcnt = 0;
  return 1;

}

int
rwtest(uint pattern)
{
  int ret;
  if(pattern == 0) { //Reader

  acquireTicketlock(&mutex);
  readcnt++;
  if(readcnt==1)
    acquireTicketlock(&wrt);
  releaseTicketlock(&mutex);

  ret = rwSharedValue; //read

  acquireTicketlock(&mutex);
  readcnt--;
  if(readcnt==0)
    releaseTicketlock(&wrt);
  releaseTicketlock(&mutex);
  return ret;
  }

  else //Writer
  {
  acquireTicketlock(&wrt);
  rwSharedValue++;
  ret = rwSharedValue;
  releaseTicketlock(&wrt);
  return ret;
  }
}

int
createThread(void(*func)(), void *stack)
{

  struct thread * t;
  t = allocthread(myproc()); //Creating a thread for running process

  if(t == 0) //Couldn't create thread
    return -1;

  *t->tf = *mythread()->tf;

  t->tf->eax = 0; // Clearing eax so that fork returns 0 in the child.
  t->tf->eip = (int) func; // Assigning program counter to start address of function
//  cprintf("t->tf->eip: %d\n", t->tf->eip);
//  cprintf("myt: %d\n", mythread()->tf->eip);
  t->tf->esp = (int) stack + 4000;
//  cprintf("1: %d\n2:%d\n", (int) stack + PGSIZE, (uint) stack + PGSIZE);
  t->tf->ebp = t->tf->esp;

  t->state = RUNNABLE;
//  cprintf("Created thread id: %d\n", t->tid);
  return t->tid;
}

void
exitThread()
{ 
  struct thread * t;
  int found = 0;

  acquire(&myproc()->lock);
  for(t = myproc()->threads; t < &myproc()->threads[MAX_THREADS]; t++)
    if( t->tid != mythread()->tid && (t->state == EMBRYO || t->state == RUNNABLE || t->state == RUNNING || t->state == SLEEPING))
      found = 1; //This means there is another thread in the process which can run, so this thread which we want to exit is not the last running thread!

  if(!found) //This means this thread is the last thread for this process which can run, so process should exit.
  {
    release(&myproc()->lock);
    wakeup(t);
    exit();
  }
  //The following lines run for a thread which is not the last runnable thread in process
  release(&myproc()->lock);

  acquire(&ptable.lock);
  wakeup1(mythread());
  mythread()->state = TZOMBIE;
  
  sched(); 

}


int
joinThread(int tid)
{
  struct thread * t;
  if(tid == mythread()->tid) //Thread is waiting for itself!
    return -1;

  for(t = myproc()->threads; t < &myproc()->threads[MAX_THREADS] && t->tid != tid; t++);
  //t is now the thread which we want to wait for to finish

  acquire(&ptable.lock);
  // found the one
  while(t->tid == tid && (t->state == EMBRYO || t->state == RUNNABLE || t->state == RUNNING || t->state == SLEEPING))
    sleep(t, &ptable.lock); //While thread with tid is not finish, sleep!

  release(&ptable.lock);

  //Thread's parent is finished!
  if(t->state == TZOMBIE)
    clearThread(t);

  return 0;
}
