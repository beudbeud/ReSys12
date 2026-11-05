// ReSys12: pthread implementation of pcsx_rearmed's small thread API (frontend/pcsxr-threads.h) for the libretro
// build; the Vita build has shim/rs12_threads_vita.c. GPL-2.0-or-later.
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/psx/frontend/pcsxr-threads.h"

int pcsxr_sthread_core_count = 1;

struct sthread { pthread_t t; void (*func)(void *); };
struct slock { pthread_mutex_t m; };
struct scond { pthread_cond_t c; };

void pcsxr_sthread_init(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	pcsxr_sthread_core_count = n > 0 ? (int)n : 1;
}

static void *entry(void *p)
{
	((struct sthread *)p)->func(NULL);
	return NULL;
}

struct sthread *pcsxr_sthread_create(void (*thread_func)(void *), enum pcsxr_thread_type type)
{
	(void)type;
	struct sthread *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->func = thread_func;
	if (pthread_create(&t->t, NULL, entry, t)) {
		free(t);
		return NULL;
	}
	return t;
}

void sthread_join(struct sthread *t)
{
	if (!t)
		return;
	pthread_join(t->t, NULL);
	free(t);
}

struct slock *slock_new(void)
{
	struct slock *l = malloc(sizeof(*l));
	if (l)
		pthread_mutex_init(&l->m, NULL);
	return l;
}
void slock_free(struct slock *l) { if (l) { pthread_mutex_destroy(&l->m); free(l); } }
void slock_lock(struct slock *l) { pthread_mutex_lock(&l->m); }
void slock_unlock(struct slock *l) { pthread_mutex_unlock(&l->m); }

struct scond *scond_new(void)
{
	struct scond *c = malloc(sizeof(*c));
	if (c)
		pthread_cond_init(&c->c, NULL);
	return c;
}
void scond_free(struct scond *c) { if (c) { pthread_cond_destroy(&c->c); free(c); } }
void scond_wait(struct scond *c, struct slock *l) { pthread_cond_wait(&c->c, &l->m); }
void scond_signal(struct scond *c) { pthread_cond_signal(&c->c); }
