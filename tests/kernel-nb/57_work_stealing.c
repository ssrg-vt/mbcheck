// SPDX-License-Identifier: GPL-2.0
// 57_work_stealing.c — Work-stealing scheduler (N-peer, self-directed)
//
// Non-blocking pattern: WORK-STEALING (peer-to-peer task redistribution).
//
// Work stealing is used in schedulers where threads first exhaust their own
// local deque, then steal from a randomly-chosen peer's deque.  Every thread
// is simultaneously a producer (pushing its own tasks) and a consumer (popping
// or stealing tasks).  There is no fixed producer–consumer asymmetry.
//
// This litmus models the pointer-analysis-relevant core:
//   - N worker "slots", each with a pointer to a task queue head.
//   - A worker pushes tasks to its own slot (xchg to prepend).
//   - A stealer reads another worker's slot (xchg to take the whole list).
//
// The xchg-based steal is obstruction-free: a stealer atomically takes the
// entire list, processes it, and any concurrent push races with the xchg
// (the pusher retries via CAS).
//
// Used in:
//   - Linux kernel's workqueue (work_struct stealing between per-CPU lists)
//   - Go goroutine scheduler (runq steal)
//   - Cilk / TBB (Chase-Lev deque steal — litmus 36 models the deque itself)
//
// Ordering edges of interest:
//   push(wid, task): kmalloc() → task →{heap}
//                   xchg(&g_slots[wid], old_head); task →{heap} in new list
//   steal(from):    xchg(&g_slots[from], NULL) → stolen →{heap list}
//                   traverse stolen list: node →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-57: work-stealing — N-peer push/steal via xchg");

#define N_WORKERS  3

struct ws_task {
	int             id;
	struct ws_task *next;
};

/* Per-worker slot: head of a singly-linked task list. */
static struct ws_task *g_slots[N_WORKERS];

/* Push a task onto worker wid's slot (prepend via xchg). */
static int ws_push(int wid, int task_id)
{
	struct ws_task *task, *old;

	task = kmalloc(sizeof(*task), GFP_KERNEL);   /* HeapObjVar */
	if (!task)
		return -ENOMEM;
	task->id = task_id;

	/* Atomically prepend: set task->next = old_head, swing slot. */
	do {
		old        = READ_ONCE(g_slots[wid]);   /* old →{heap|NULL} */
		task->next = old;
	} while (cmpxchg(&g_slots[wid], old, task) != old);
	/* g_slots[wid] →{heap} (task) */
	return 0;
}

/* Steal all tasks from worker 'from' (xchg to NULL). */
static struct ws_task *ws_steal(int from)
{
	return xchg(&g_slots[from], (struct ws_task *)NULL);
	/* Returns the stolen list head →{heap|NULL}; g_slots[from] = NULL */
}

static int __init ws57_init(void)
{
	int i;

	/* Each worker pushes tasks to its own slot. */
	ws_push(0, 10);
	ws_push(0, 11);
	ws_push(1, 20);
	ws_push(2, 30);
	ws_push(2, 31);

	pr_info("ws57: all workers pushed tasks\n");
	return 0;
}

static void __exit ws57_exit(void)
{
	int w;

	/* Each worker (or stealer) drains one slot. */
	for (w = 0; w < N_WORKERS; w++) {
		struct ws_task *stolen, *n;

		stolen = ws_steal(w);   /* stolen →{heap|NULL} */
		while (stolen) {
			n = stolen->next;   /* n →{heap|NULL} */
			pr_info("ws57: worker %d processed task %d\n", w, stolen->id);
			kfree(stolen);
			stolen = n;
		}
	}
}

module_init(ws57_init);
module_exit(ws57_exit);
