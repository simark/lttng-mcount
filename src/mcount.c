#include <malloc.h>
#include <pthread.h>

#include "util/list.h"
#include "util/compiler.h"

#include "mcount-arch.h"
#include "dynamic.h"

#define TRACEPOINT_DEFINE
#define TRACEPOINT_CREATE_PROBES
#define TP_IP_PARAM func_addr
#include "func-tp.h"

_Atomic(int) lttng_mcount_ready = 0;
static pthread_key_t td_key;

struct lttng_mcount_stack_frame {
	unsigned long child_ip;
	unsigned long parent_ip;
	struct list_head list;
};

struct lttng_mcount_thread_data {
	int recursion_guard;
	struct list_head stack;
};

static void mcount_thread_data_dtor(void *data)
{
	struct lttng_mcount_thread_data *tdp = data;
	struct lttng_mcount_stack_frame *pos = NULL, *next = NULL;

	list_for_each_entry_safe(pos, next, &tdp->stack, list) {
		list_del(&pos->list);
		free(pos);
	}
}

__attribute__((constructor))
void mcount_init()
{
	struct lttng_mcount_thread_data *tdp;

	(void) pthread_key_create(&td_key, mcount_thread_data_dtor);

	dynamic_init();

	lttng_mcount_ready = 1;
}

static void mcount_prepare_thread()
{
	struct lttng_mcount_thread_data *tdp =
		malloc(sizeof(struct lttng_mcount_thread_data));

	tdp->recursion_guard = 0;
	INIT_LIST_HEAD(&tdp->stack);

	(void) pthread_setspecific(td_key, tdp);
}

struct lttng_mcount_thread_data *mcount_get_thread_data()
{
	struct lttng_mcount_thread_data *tdp;
	tdp = pthread_getspecific(td_key);
	if(unlikely(!tdp)) {
		mcount_prepare_thread();
		tdp = pthread_getspecific(td_key);
	}
	return tdp;
}

static int mcount_should_stop()
{
	struct lttng_mcount_thread_data *tdp;

	// Check global lock for complete init
	if (unlikely(!lttng_mcount_ready))
		return -1;
	// Check thread-local lock to avoid infinite recursion
	tdp = mcount_get_thread_data();
	if (!tdp || tdp->recursion_guard)
		return -1;
	return 0;
}

int mcount_entry(unsigned long *parent_loc, unsigned long child,
		 struct mcount_regs *regs)
{
	struct lttng_mcount_thread_data *tdp;
	struct lttng_mcount_stack_frame *sf;

	if (unlikely(mcount_should_stop()))
		return -1;

	tdp = mcount_get_thread_data();
	tdp->recursion_guard = 1;

	sf = malloc(sizeof(struct lttng_mcount_stack_frame));
	sf->child_ip = child;
	sf->parent_ip = *parent_loc;
	INIT_LIST_HEAD(&sf->list);

	list_add(&sf->list, &tdp->stack);

	/* trace with lttng */
	tracepoint(lttng_mcount, func_entry,
		   (void *)sf->child_ip,
		   (void *)sf->parent_ip);

	tdp->recursion_guard = 0;
	return 0;
}

unsigned long mcount_exit(long *retval)
{
	struct lttng_mcount_thread_data *tdp;
	struct lttng_mcount_stack_frame *sf;
	unsigned long retaddr;

	tdp = mcount_get_thread_data();
	tdp->recursion_guard = 1;

	sf = list_entry(tdp->stack.next,
			struct lttng_mcount_stack_frame, list);
	retaddr = sf->parent_ip;

	/* trace with lttng */
	tracepoint(lttng_mcount, func_exit,
		   (void *)sf->child_ip,
		   (void *)sf->parent_ip);

	list_del(&sf->list);
	free(sf);

	tdp->recursion_guard = 0;

	return retaddr;
}

// Stub out gmon
void __visible_default __monstartup(unsigned long low, unsigned long high) {}
void __visible_default _mcleanup(void) {}
