// SPDX-License-Identifier: GPL-2.0
/*
 * 最小 dma_resv 教学模块：不碰 GPU，用手动 signal 的软件 fence
 * 演示 "登记表 + usage 偏序 + 三段式写入 + 迭代读取"。
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>

/* 
// dma_resv.h 里没有对外导出的函数，只有内核内部使用的接口。
// 这里我们自己声明一下，方便 demo 使用。 这是主要的函数.
void dma_resv_init(struct dma_resv *obj);
void dma_resv_fini(struct dma_resv *obj);
int dma_resv_reserve_fences(struct dma_resv *obj, unsigned int num_fences);
void dma_resv_add_fence(struct dma_resv *obj, struct dma_fence *fence,
			enum dma_resv_usage usage);
void dma_resv_replace_fences(struct dma_resv *obj, uint64_t context,
			     struct dma_fence *fence,
			     enum dma_resv_usage usage);
int dma_resv_get_fences(struct dma_resv *obj, enum dma_resv_usage usage,
			unsigned int *num_fences, struct dma_fence ***fences);
int dma_resv_get_singleton(struct dma_resv *obj, enum dma_resv_usage usage,
			   struct dma_fence **fence);
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src);
long dma_resv_wait_timeout(struct dma_resv *obj, enum dma_resv_usage usage,
			   bool intr, unsigned long timeout);
void dma_resv_set_deadline(struct dma_resv *obj, enum dma_resv_usage usage,
			   ktime_t deadline);
bool dma_resv_test_signaled(struct dma_resv *obj, enum dma_resv_usage usage);
void dma_resv_describe(struct dma_resv *obj, struct seq_file *seq);

*/


/* ============ 第一步：造一个最简单的 fence ============
 * 真实世界里 fence 由 GPU ring 的中断来 signal。
 * 这里我们不接硬件，全部手动 dma_fence_signal()。
 */
struct demo_fence {
	struct dma_fence base;		/* 必须是第一个成员 */
	spinlock_t lock;
	char name[16];
};

static const char *demo_driver_name(struct dma_fence *f)
{
	return "dma_resv_demo";
}

static const char *demo_timeline_name(struct dma_fence *f)
{
	return container_of(f, struct demo_fence, base)->name;
}

static const struct dma_fence_ops demo_fence_ops = {
	.get_driver_name   = demo_driver_name,
	.get_timeline_name = demo_timeline_name,
};

/* ctx 相同 = 同一条“时间线”（同一个 ring / 同一个队列） */
static struct dma_fence *demo_fence_new(const char *name, u64 ctx, u64 seqno)
{
	struct demo_fence *df = kzalloc(sizeof(*df), GFP_KERNEL);

	if (!df)
		return NULL;

	strscpy(df->name, name, sizeof(df->name));
	spin_lock_init(&df->lock);
	dma_fence_init(&df->base, &demo_fence_ops, &df->lock, ctx, seqno);
	return &df->base;
}

/* ============ 第二步：一个 dma_resv 就是一块 buffer 的登记表 ============ */
static struct dma_resv demo_resv;

static struct dma_fence *f_move, *f_draw, *f_scan, *f_evict, *f_draw2;

static const char *usage_str(enum dma_resv_usage u)
{
	static const char * const s[] = { "KERNEL", "WRITE", "READ", "BOOKKEEP" };

	return s[u];
}

/* 用不同 usage 去查同一张表，看到的集合不一样 —— 这就是偏序 */
static void demo_dump(enum dma_resv_usage query)
{
	struct dma_resv_iter cursor;
	struct dma_fence *f;
	int n = 0;

	pr_info("  查询 usage=%-8s ->", usage_str(query));
	dma_resv_for_each_fence(&cursor, &demo_resv, query, f) {
		pr_cont(" %s(%s)", demo_timeline_name(f),
			usage_str(dma_resv_iter_usage(&cursor)));
		n++;
	}
	pr_cont("   [共 %d 个]\n", n);
}

static int __init demo_init(void)
{
	pr_info("=== dma_resv demo start ===\n");

	dma_resv_init(&demo_resv);

	/* 4 条互不相干的时间线，模拟 4 个不同来源的操作 */
	f_move  = demo_fence_new("move",  dma_fence_context_alloc(1), 1);
	f_draw  = demo_fence_new("draw",  dma_fence_context_alloc(1), 1);
	f_scan  = demo_fence_new("scan",  dma_fence_context_alloc(1), 1);
	f_evict = demo_fence_new("evict", dma_fence_context_alloc(1), 1);
	if (!f_move || !f_draw || !f_scan || !f_evict)
		return -ENOMEM;

	/* ---------- 写侧：严格的三段式 ---------- */
	dma_resv_lock(&demo_resv, NULL);		/* ① 上锁 */

	if (dma_resv_reserve_fences(&demo_resv, 4)) {	/* ② 预留槽位，只有这步会失败 */
		dma_resv_unlock(&demo_resv);
		return -ENOMEM;
	}

	/* ③ 登记：此后不会失败，因为槽位已经买好了 */
	dma_resv_add_fence(&demo_resv, f_move,  DMA_RESV_USAGE_KERNEL);//f_move -> KERNEL
	dma_resv_add_fence(&demo_resv, f_draw,  DMA_RESV_USAGE_WRITE);//f_draw -> WRITE
	dma_resv_add_fence(&demo_resv, f_scan,  DMA_RESV_USAGE_READ);//f_scan -> READ
	dma_resv_add_fence(&demo_resv, f_evict, DMA_RESV_USAGE_BOOKKEEP);//f_evict -> BOOKKEEP

	pr_info("[A] 登记了 4 个 fence，用不同 usage 查询：\n");
	demo_dump(DMA_RESV_USAGE_KERNEL);
	demo_dump(DMA_RESV_USAGE_WRITE);
	demo_dump(DMA_RESV_USAGE_READ);
	demo_dump(DMA_RESV_USAGE_BOOKKEEP);

	/* ---------- 同 context 会被就地替换，表不会膨胀 ---------- */
	f_draw2 = demo_fence_new("draw", f_draw->context, 2);	/* 同 ctx，seqno 更大 */
	if (!f_draw2) {
		dma_resv_unlock(&demo_resv);
		return -ENOMEM;
	}
	dma_resv_add_fence(&demo_resv, f_draw2, DMA_RESV_USAGE_WRITE);

	pr_info("[B] 再加一个同 context 的新 fence(draw seq=2)，注意总数没变：\n");
	demo_dump(DMA_RESV_USAGE_BOOKKEEP);

	dma_resv_unlock(&demo_resv);			/* ④ 解锁 */

	/* ---------- 读侧：不持锁也能问“做完了没” ---------- */
	pr_info("[C] signal 之前：\n");
	pr_info("  test_signaled(KERNEL)=%d  test_signaled(READ)=%d\n",
		dma_resv_test_signaled(&demo_resv, DMA_RESV_USAGE_KERNEL),
		dma_resv_test_signaled(&demo_resv, DMA_RESV_USAGE_READ));

	dma_fence_signal(f_move);	/* 假装搬运 DMA 干完了 */

	pr_info("[D] 只 signal 了 move(KERNEL) 之后：\n");
	pr_info("  test_signaled(KERNEL)=%d  test_signaled(READ)=%d\n",
		dma_resv_test_signaled(&demo_resv, DMA_RESV_USAGE_KERNEL),
		dma_resv_test_signaled(&demo_resv, DMA_RESV_USAGE_READ));

	/* 全部 signal，wait 立刻返回 */
	dma_fence_signal(f_draw2);
	dma_fence_signal(f_scan);
	dma_fence_signal(f_evict);

	pr_info("[E] 全部 signal 后 wait_timeout(BOOKKEEP) 返回 %ld (>0 表示没超时)\n",
		dma_resv_wait_timeout(&demo_resv, DMA_RESV_USAGE_BOOKKEEP,
				      false, HZ));

	pr_info("=== dma_resv demo done ===\n");
	return 0;
}

static void __exit demo_exit(void)
{
	dma_resv_fini(&demo_resv);	/* 丢掉表里持有的引用 */

	/* 丢掉我们自己持有的那份引用 */
	dma_fence_put(f_move);
	dma_fence_put(f_draw);
	dma_fence_put(f_draw2);
	dma_fence_put(f_scan);
	dma_fence_put(f_evict);

	pr_info("=== dma_resv demo unloaded ===\n");
}

module_init(demo_init);
module_exit(demo_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal dma_resv teaching demo");
