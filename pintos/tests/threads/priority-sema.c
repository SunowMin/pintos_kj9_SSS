/* Tests that the highest-priority thread waiting on a semaphore
   is the first to wake up. */
/* 세마포어에 대기 중인 스레드 중에서 가장 높은 우선순위를 가진 스레드를 먼저 깨워야 한다. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static thread_func priority_sema_thread;
static struct semaphore sema;

void test_priority_sema(void)
{
  int i;

  /* This test does not work with the MLFQS. */
  ASSERT(!thread_mlfqs);

  sema_init(&sema, 0); // 세마포어를 0으로 초기화해서, sema_down() 호출 시 곧바로 블록되게 만든다.
  thread_set_priority(PRI_MIN);
  for (i = 0; i < 10; i++)
  {
    int priority = PRI_DEFAULT - (i + 3) % 10 - 1;
    char name[16];
    snprintf(name, sizeof name, "priority %d", priority);
    thread_create(name, priority, priority_sema_thread, NULL); // sema_up(&sema)로 블록시킨다.
  }

  for (i = 0; i < 10; i++)
  {
    sema_up(&sema);
    msg("Back in main thread.");
  }
}

static void
priority_sema_thread(void *aux UNUSED)
{
  sema_down(&sema);
  msg("Thread %s woke up.", thread_name());
}
