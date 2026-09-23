// shared_global.c
//
// Message-passing pattern: producer (init) heap-allocates a payload, writes
// to it, then atomically publishes the pointer via smp_store_release().
// Consumer (exit) acquires the pointer via smp_load_acquire() and reads the
// payload before freeing it.
//
// Interest for pointer analysis:
//   - `g_msg_ptr` is a GlobalObjVar (the pointer that carries the message)
//   - kmalloc return is a HeapObjVar
//   - smp_store_release(&g_msg_ptr, msg) makes g_msg_ptr point-to {heap}
//   - smp_load_acquire(&g_msg_ptr) in exit observes g_msg_ptr -> {heap}
//
// Build:
//   make -C /path/to/linux-6.19-aarch64 M=$PWD modules \
//        ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mbcheck");
MODULE_DESCRIPTION("Message-passing: atomic pointer publish to heap payload");

struct payload {
    int value;
};

/* Atomic pointer: producer stores, consumer loads via acquire/release. */
static struct payload *g_msg_ptr;

static int __init shared_global_init(void)
{
    struct payload *msg = kmalloc(sizeof(*msg), GFP_KERNEL);
    if (!msg)
        return -ENOMEM;

    msg->value = 42;

    /* Release store: publish heap pointer atomically to consumer. */
    smp_store_release(&g_msg_ptr, msg);   /* g_msg_ptr -> {heap} */
    return 0;
}

static void __exit shared_global_exit(void)
{
    /* Acquire load: observe the pointer published by the producer. */
    struct payload *msg = smp_load_acquire(&g_msg_ptr);

    if (msg) {
        pr_info("payload value = %d\n", msg->value);
        kfree(msg);
    }
}

module_init(shared_global_init);
module_exit(shared_global_exit);
