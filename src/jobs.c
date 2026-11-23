#include "jobs.h"
#include "http.h"

#include <SDL3/SDL.h>

#define MAX_THREADS 8
/* How many JOB_REST jobs run at once: big downloads, that mustn't hold up
 * what's asked for meanwhile. */
#define MAX_REST 3
/* Threads kept for JOB_NOW: what the user does never waits for the rest. */
#define RESERVED 2

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
   struct job *head[JOB_LEVELS], *tail[JOB_LEVELS];
   /* Running: JOB_REST, and all but JOB_NOW. */
   int rest, later;
   SDL_Thread *threads[MAX_THREADS];
   int count;
   bool quit;
   SDL_AtomicInt abort;
} jobs;

/* The next job to run, NULL if there's none now. Under the lock. */
static struct job *take(int *level)
{
   int l;

   for (l = 0; l < JOB_LEVELS; l++)
   {
      struct job *j = jobs.head[l];
      if (!j || (l == JOB_REST && jobs.rest >= MAX_REST)
            || (l != JOB_NOW && jobs.later >= SDL_max(1, jobs.count - RESERVED)))
         continue;
      jobs.head[l] = j->next;
      if (!j->next)
         jobs.tail[l] = NULL;
      *level = l;
      return j;
   }
   return NULL;
}

static int SDLCALL worker(void *unused)
{
   while (true)
   {
      struct job *j = NULL;
      int level = 0;

      SDL_LockMutex(jobs.lock);
      while (!jobs.quit && !(j = take(&level)))
         SDL_WaitCondition(jobs.wake, jobs.lock);
      if (jobs.quit)
      {
         SDL_UnlockMutex(jobs.lock);
         if (j)
         {
            SDL_free(j->data);
            SDL_free(j);
         }
         return 0;
      }
      jobs.rest += level == JOB_REST;
      jobs.later += level != JOB_NOW;
      SDL_UnlockMutex(jobs.lock);

      j->fn(j->data);
      SDL_free(j);

      /* This thread looks for the next itself: nobody needs waking. */
      SDL_LockMutex(jobs.lock);
      jobs.rest -= level == JOB_REST;
      jobs.later -= level != JOB_NOW;
      SDL_UnlockMutex(jobs.lock);
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
   for (i = 0; i < JOB_LEVELS; i++)
      while ((j = jobs.head[i]))
      {
         jobs.head[i] = j->next;
         SDL_free(j->data);
         SDL_free(j);
      }
   SDL_DestroyCondition(jobs.wake);
   SDL_DestroyMutex(jobs.lock);
   SDL_zero(jobs);
}

void jobs_add(void (*fn)(void *data), void *data, enum job_level level, bool first)
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
   if (first || !jobs.head[level])
   {
      j->next = jobs.head[level];
      jobs.head[level] = j;
      if (!jobs.tail[level])
         jobs.tail[level] = j;
   }
   else
   {
      jobs.tail[level]->next = j;
      jobs.tail[level] = j;
   }
   SDL_SignalCondition(jobs.wake);
   SDL_UnlockMutex(jobs.lock);
}

SDL_AtomicInt *jobs_abort(void)
{
   return &jobs.abort;
}
