#include "jobs.h"
#include "http.h"

#include <SDL3/SDL.h>

#define MAX_THREADS 8

struct job
{
   void (*fn)(void *data);
   void *data;
   struct job *next;
};

static struct
{
   SDL_Mutex *lock;
   SDL_Condition *wake;
   struct job *head, *tail;
   SDL_Thread *threads[MAX_THREADS];
   int count;
   bool quit;
   SDL_AtomicInt abort;
} jobs;

static int SDLCALL worker(void *unused)
{
   while (true)
   {
      struct job *j;

      SDL_LockMutex(jobs.lock);
      while (!jobs.head && !jobs.quit)
         SDL_WaitCondition(jobs.wake, jobs.lock);
      if (jobs.quit)
      {
         SDL_UnlockMutex(jobs.lock);
         return 0;
      }
      j = jobs.head;
      jobs.head = j->next;
      if (!jobs.head)
         jobs.tail = NULL;
      SDL_UnlockMutex(jobs.lock);

      j->fn(j->data);
      SDL_free(j);
   }
}

void jobs_init(int threads)
{
   int i;

   jobs.lock = SDL_CreateMutex();
   jobs.wake = SDL_CreateCondition();
   jobs.count = SDL_clamp(threads, 1, MAX_THREADS);
   for (i = 0; i < jobs.count; i++)
      jobs.threads[i] = SDL_CreateThread(worker, "fusebox-job", NULL);
}

void jobs_quit(void)
{
   struct job *j;
   int i;

   if (!jobs.lock)
      return;
   SDL_SetAtomicInt(&jobs.abort, 1);
   SDL_LockMutex(jobs.lock);
   jobs.quit = true;
   SDL_BroadcastCondition(jobs.wake);
   SDL_UnlockMutex(jobs.lock);
   for (i = 0; i < jobs.count; i++)
      SDL_WaitThread(jobs.threads[i], NULL);
   while ((j = jobs.head))
   {
      jobs.head = j->next;
      SDL_free(j->data);
      SDL_free(j);
   }
   SDL_DestroyCondition(jobs.wake);
   SDL_DestroyMutex(jobs.lock);
   SDL_zero(jobs);
}

void jobs_add(void (*fn)(void *data), void *data, bool urgent)
{
   struct job *j = SDL_calloc(1, sizeof(*j));

   if (!j)
   {
      SDL_free(data);
      return;
   }
   j->fn = fn;
   j->data = data;
   SDL_LockMutex(jobs.lock);
   if (urgent)
   {
      j->next = jobs.head;
      jobs.head = j;
      if (!jobs.tail)
         jobs.tail = j;
   }
   else
   {
      if (jobs.tail)
         jobs.tail->next = j;
      else
         jobs.head = j;
      jobs.tail = j;
   }
   SDL_SignalCondition(jobs.wake);
   SDL_UnlockMutex(jobs.lock);
}

SDL_AtomicInt *jobs_abort(void)
{
   return &jobs.abort;
}
