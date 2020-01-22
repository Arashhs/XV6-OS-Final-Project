// Mutual exclusion ticket locks.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "ticketlock.h"

void
initTicketlock(struct ticketlock *lk, char *name)
{
  lk->name = name;
  lk->turn = 0;
  lk->ticket = 0;
  lk->cpu = 0;
}

// Acquire the lock, get a ticket.
void
acquireTicketlock(struct ticketlock *lk)
{
  int ticket;
  ticket = fetch_and_add(&lk->ticket, 1);
  while(ticket != lk->turn)
  ;

  // Record info about lock acquisition for debugging.
  lk->cpu = mycpu();
}

// Release the lock, give turn to the next ticket
void
releaseTicketLock(struct ticketlock *lk)
{

  lk->pcs[0] = 0;
  lk->cpu = 0;

  lk->turn++;

  
}