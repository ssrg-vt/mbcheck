// SPDX-License-Identifier: GPL-2.0
// P0: write_seqlock -> W(data) -> write_sequnlock
// P1: read_seqbegin -> R(data) -> read_seqretry
// True negative: the reader takes no lock; read_seqretry detects the concurrent write.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("sync-09: seqlock reader-writer retry");

static DEFINE_SEQLOCK(g_seq);
static int g_data;

void sync09_write(int v)
{
	write_seqlock(&g_seq);
	g_data = v;			/* W(data) */
	write_sequnlock(&g_seq);
}
EXPORT_SYMBOL_GPL(sync09_write);

int sync09_read(void)
{
	unsigned int seq;
	int v;

	do {
		seq = read_seqbegin(&g_seq);
		v = g_data;		/* R(data) */
	} while (read_seqretry(&g_seq, seq));

	return v;
}
EXPORT_SYMBOL_GPL(sync09_read);
