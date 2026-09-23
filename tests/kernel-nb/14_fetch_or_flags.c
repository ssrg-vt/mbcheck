// SPDX-License-Identifier: GPL-2.0
// 14_fetch_or_flags.c — Fetch-or bitmask for state machine transitions
//
// Non-blocking pattern: BITMASK STATE MACHINE via atomic_fetch_or.
//
// Many kernel subsystems use a single atomic word as a bit-field of flags.
// atomic_fetch_or atomically ORs in one or more bits and returns the OLD
// value, letting the caller inspect the previous state.  This enables:
//   - Non-blocking transitions: set a bit and observe what was set before
//   - One-time transitions: set a bit, confirm it wasn't already set
//   - Detecting conflicting state changes
//
// Example: a device state word with bits INIT, RUNNING, STOPPING, STOPPED.
// Transition RUNNING→STOPPING: set STOPPING and verify RUNNING was set and
// STOPPING was not.
//
// Key API (kernel-memory-ordering-apis.md §1.1):
//   atomic_fetch_or(mask, v)   — bitwise OR, return old value; full barrier
//   atomic_fetch_and(mask, v)  — bitwise AND (for clearing), return old
//   atomic_read(v)             — relaxed read of current flags
//
// Folly analogue: folly::AtomicBitSet, folly::detail::Futex flags field.
//
// Ordering edges of interest for pointer analysis:
//   init:  kmalloc() → ctx           HeapObjVar
//          g_ctx = ctx                g_ctx →{heap}
//          atomic_set(&ctx->flags, DEVICE_INIT)
//   start: atomic_fetch_or(RUNNING, &ctx->flags) → old
//   stop:  atomic_fetch_or(STOPPING, &ctx->flags) → old
//   read:  g_ctx → ctx                ctx →{heap}

#include <linux/init.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("NB-14: bitmask state machine via atomic_fetch_or");

/* Device state bits. */
#define DEVICE_INIT     BIT(0)
#define DEVICE_RUNNING  BIT(1)
#define DEVICE_STOPPING BIT(2)
#define DEVICE_STOPPED  BIT(3)

struct device_ctx {
	atomic_t flags;
	int      id;
};

static struct device_ctx *g_ctx;

/* Returns true if the RUNNING→STOPPING transition succeeded. */
static bool ctx_stop(struct device_ctx *ctx)
{
	int old;

	/*
	 * Atomically set STOPPING and observe the old state.
	 * If RUNNING was set and STOPPING was clear, the transition is valid.
	 */
	old = atomic_fetch_or(DEVICE_STOPPING, &ctx->flags);  /* full barrier */
	return (old & DEVICE_RUNNING) && !(old & DEVICE_STOPPING);
}

static int __init flags_init(void)
{
	struct device_ctx *ctx;
	int old;

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);    /* HeapObjVar */
	if (!ctx)
		return -ENOMEM;

	ctx->id = 42;
	atomic_set(&ctx->flags, DEVICE_INIT);
	g_ctx = ctx;                                /* g_ctx →{heap} */

	/* Transition INIT → RUNNING. */
	old = atomic_fetch_or(DEVICE_RUNNING, &ctx->flags);
	pr_info("flags: INIT→RUNNING, old=0x%x new=0x%x\n",
		old, atomic_read(&ctx->flags));

	/* Transition RUNNING → STOPPING (non-blocking). */
	if (ctx_stop(ctx))
		pr_info("flags: RUNNING→STOPPING ok\n");

	return 0;
}

static void __exit flags_exit(void)
{
	struct device_ctx *ctx = g_ctx;              /* ctx →{heap} */

	if (!ctx)
		return;

	/* Clear STOPPING, set STOPPED (fetch_and + fetch_or). */
	atomic_fetch_and(~DEVICE_STOPPING, &ctx->flags);
	atomic_fetch_or(DEVICE_STOPPED, &ctx->flags);

	pr_info("flags: final=0x%x\n", atomic_read(&ctx->flags));
	kfree(ctx);
}

module_init(flags_init);
module_exit(flags_exit);
