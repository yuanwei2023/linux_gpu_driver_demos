// SPDX-License-Identifier: GPL-2.0
/*
 * 最小 TTM 教学模块：不碰 GPU。
 * 用假 VRAM（ttm_range_manager）+ SYSTEM 域，演示：
 *   创建设备 → 分配 BO → validate/move → 空间不够 eviction → pin 免疫 → dma_resv 卡住搬移
 *
 * 对照真实 amdgpu：
 *   ttm_device_init          ↔ amdgpu_ttm.c
 *   ttm_range_man_init(VRAM) ↔ VRAM manager
 *   ttm_bo_init_reserved     ↔ amdgpu_bo_create()
 *   ttm_bo_validate          ↔ amdgpu_bo_pin / CS 前 validate
 *   funcs->move              ↔ amdgpu_bo_move() → SDMA blit
 * 本 demo 的 move 只改 placement 标签，数据始终在 ttm_tt 的系统页上。
 *
 * 针对 6.8 内核：TTM 公共头是 ttm_bo.h / ttm_device.h，
 * 没有旧的 ttm_bo_api.h / ttm_bo_driver.h。
 */
#define pr_fmt(fmt) "ttm_demo: " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/platform_device.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/ttm/ttm_resource.h>

#define DEMO_VRAM_PAGES		4		/* 16KB 假 VRAM，故意很小以便挤爆 */
#define DEMO_MAGIC		0xDEADBEEFu

struct demo_bo {
	struct ttm_buffer_object tbo;
	const char *name;
};

static struct platform_device *demo_pdev;
static struct file *demo_anon_file;
static struct drm_device demo_drm;
static struct drm_vma_offset_manager demo_vma_mgr;
static struct ttm_device demo_bdev;
static bool demo_ttm_ok;
static bool demo_vram_ok;
static bool demo_drm_ok;

/* ---------- placement：SYSTEM 无限，VRAM 只有 4 页 ---------- */
static const struct ttm_place place_sys = {
	.mem_type = TTM_PL_SYSTEM,
};

static const struct ttm_place place_vram = {
	.mem_type = TTM_PL_VRAM,
};

/* A 从顶部往下长，避免占住 start=0 把后面的空闲切碎 */
static const struct ttm_place place_vram_top = {
	.mem_type = TTM_PL_VRAM,
	.flags = TTM_PL_FLAG_TOPDOWN,
};

static struct ttm_placement pl_sys = {
	.num_placement = 1,
	.placement = &place_sys,
	.num_busy_placement = 1,
	.busy_placement = &place_sys,
};

static struct ttm_placement pl_vram = {
	.num_placement = 1,
	.placement = &place_vram,
	.num_busy_placement = 1,
	.busy_placement = &place_vram,
};

static struct ttm_placement pl_vram_top = {
	.num_placement = 1,
	.placement = &place_vram_top,
	.num_busy_placement = 1,
	.busy_placement = &place_vram_top,
};

static const char *placement_name(struct ttm_placement *pl)
{
	if (pl == &pl_sys)
		return "SYSTEM";
	return "VRAM";
}

static const char *mem_name(u32 mem_type)
{
	switch (mem_type) {
	case TTM_PL_SYSTEM:
		return "SYSTEM";
	case TTM_PL_TT:
		return "TT/GTT";
	case TTM_PL_VRAM:
		return "VRAM";
	default:
		return "OTHER";
	}
}

static void dump_bo(const char *tag, struct demo_bo *dbo)
{
	struct ttm_buffer_object *bo = &dbo->tbo;
	u32 mem = bo->resource ? bo->resource->mem_type : U32_MAX;
	unsigned long start = bo->resource ? bo->resource->start : 0;
	bool pop = bo->ttm && ttm_tt_is_populated(bo->ttm);

	pr_info("  %-8s %s size=%zuKB mem=%s start=%lu pin=%u tt=%s pop=%d\n",
		tag, dbo->name, bo->base.size >> 10,
		bo->resource ? mem_name(mem) : "NONE", start,
		bo->pin_count, bo->ttm ? "yes" : "no", pop);
}

static void dump_vram(const char *tag)
{
	struct ttm_resource_manager *man = ttm_manager_type(&demo_bdev, TTM_PL_VRAM);

	pr_info("  %-8s VRAM usage=%llu / %llu bytes\n",
		tag, ttm_resource_manager_usage(man),
		(u64)DEMO_VRAM_PAGES << PAGE_SHIFT);
}

/* ---------- TTM driver callbacks：驱动需要填的那几张表 ---------- */

static struct ttm_tt *demo_tt_create(struct ttm_buffer_object *bo, u32 page_flags)
{
	struct ttm_tt *tt;
	int err;

	tt = kzalloc(sizeof(*tt), GFP_KERNEL);
	if (!tt)
		return NULL;

	err = ttm_tt_init(tt, bo, page_flags, ttm_cached, 0);
	if (err) {
		kfree(tt);
		return NULL;
	}
	return tt;
}

static void demo_tt_destroy(struct ttm_device *bdev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

/*
 * 真 GPU：amdgpu_bo_move() 先 ttm_bo_wait_ctx()，再 SDMA blit。
 * TTM 核心在 validate 时并不自动等 BO 上的 dma_resv；等不等是 move 回调的责任。
 * 假 VRAM 没有 iomem，数据本来就在 ttm_tt 页上，wait 完只换 resource 标签。
 */
static int demo_move(struct ttm_buffer_object *bo, bool evict,
		     struct ttm_operation_ctx *ctx,
		     struct ttm_resource *new_mem,
		     struct ttm_place *hop)
{
	struct demo_bo *dbo = container_of(bo, struct demo_bo, tbo);
	const char *from = bo->resource ? mem_name(bo->resource->mem_type) : "NONE";
	int err;

	pr_info("  move %s %s -> %s evict=%d\n",
		dbo->name, from, mem_name(new_mem->mem_type), evict);

	err = ttm_bo_wait_ctx(bo, ctx);
	if (err) {
		pr_info("  wait_ctx err=%d（dma_resv 上还有未完成 fence）\n", err);
		return err;
	}

	ttm_bo_move_null(bo, new_mem);
	return 0;
}

/* 被挤出 VRAM 时，落到 SYSTEM（amdgpu 则优先 GTT，再 SYSTEM） */
static void demo_evict_flags(struct ttm_buffer_object *bo,
			     struct ttm_placement *placement)
{
	*placement = pl_sys;
}

static struct ttm_device_funcs demo_funcs = {
	.ttm_tt_create       = demo_tt_create,
	.ttm_tt_destroy      = demo_tt_destroy,
	.move                = demo_move,
	.eviction_valuable   = ttm_bo_eviction_valuable,
	.evict_flags         = demo_evict_flags,
};

static void demo_bo_destroy(struct ttm_buffer_object *bo)
{
	drm_gem_object_release(&bo->base);
	kfree(container_of(bo, struct demo_bo, tbo));
}

static struct demo_bo *demo_bo_create(const char *name, size_t size,
				      struct ttm_placement *pl)
{
	struct demo_bo *dbo;
	struct ttm_operation_ctx ctx = { };
	int err;

	dbo = kzalloc(sizeof(*dbo), GFP_KERNEL);
	if (!dbo)
		return ERR_PTR(-ENOMEM);

	dbo->name = name;
	drm_gem_private_object_init(&demo_drm, &dbo->tbo.base, size);

	/* 成功时 BO 仍处于 reserved；失败时内部已经 destroy/kfree */
	err = ttm_bo_init_reserved(&demo_bdev, &dbo->tbo, ttm_bo_type_kernel,
				   pl, 1, &ctx, NULL, NULL, demo_bo_destroy);
	if (err)
		return ERR_PTR(err);

	pr_info("  create %s bytes_moved=%llu\n", name, ctx.bytes_moved);
	dump_bo("create", dbo);
	ttm_bo_unreserve(&dbo->tbo);
	return dbo;
}

static int demo_bo_validate(struct demo_bo *dbo, struct ttm_placement *pl,
			    bool no_wait_gpu)
{
	struct ttm_operation_ctx ctx = { .no_wait_gpu = no_wait_gpu };
	int err;

	err = ttm_bo_reserve(&dbo->tbo, false, false, NULL);
	if (err)
		return err;

	err = ttm_bo_validate(&dbo->tbo, pl, &ctx);
	pr_info("  validate %s -> %s bytes_moved=%llu err=%d no_wait=%d\n",
		dbo->name, placement_name(pl),
		ctx.bytes_moved, err, no_wait_gpu);
	ttm_bo_unreserve(&dbo->tbo);
	if (!err)
		dump_bo("after", dbo);
	return err;
}

static int demo_bo_cpu_rw(struct demo_bo *dbo, u32 *inout, bool write)
{
	struct ttm_bo_kmap_obj map;
	bool is_iomem;
	u32 *p;
	int err;

	err = ttm_bo_reserve(&dbo->tbo, false, false, NULL);
	if (err)
		return err;

	err = ttm_bo_kmap(&dbo->tbo, 0, 1, &map);
	if (err) {
		ttm_bo_unreserve(&dbo->tbo);
		return err;
	}

	p = ttm_kmap_obj_virtual(&map, &is_iomem);
	if (write)
		*p = *inout;
	else
		*inout = *p;

	ttm_bo_kunmap(&map);
	ttm_bo_unreserve(&dbo->tbo);
	return 0;
}

/* ---------- 软件 fence：模拟 GPU 还在用这块 BO ---------- */
static spinlock_t demo_fence_lock;

static const char *demo_fence_driver(struct dma_fence *f)
{
	return "ttm_demo";
}

static const char *demo_fence_timeline(struct dma_fence *f)
{
	return "fake-gpu";
}

static const struct dma_fence_ops demo_fence_ops = {
	.get_driver_name   = demo_fence_driver,
	.get_timeline_name = demo_fence_timeline,
};

static struct dma_fence *demo_fence_new(void)
{
	struct dma_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	dma_fence_init(f, &demo_fence_ops, &demo_fence_lock,
		       dma_fence_context_alloc(1), 1);
	dma_fence_enable_sw_signaling(f);
	return f;
}

static int demo_attach_busy_fence(struct demo_bo *dbo, struct dma_fence *f)
{
	struct dma_resv *resv = dbo->tbo.base.resv;
	int err;

	dma_resv_lock(resv, NULL);
	err = dma_resv_reserve_fences(resv, 1);
	if (!err)
		dma_resv_add_fence(resv, f, DMA_RESV_USAGE_KERNEL);
	dma_resv_unlock(resv);
	return err;
}

/*
 * 最小 DRM 设备：不调用 drm_dev_alloc()，因此不向全局 DRM minor idr 申请编号。
 * 多卡机器（例如 64 张 GPU）上 minor 可能已经耗尽，drm_dev_alloc 会返回 -ENOSPC。
 * TTM 只需要 address_space + vma manager，不必注册 /dev/dri 节点。
 */
static const struct file_operations demo_anon_fops = {
	.llseek = noop_llseek,
};

static struct drm_driver demo_drm_driver = {
	.driver_features = DRIVER_GEM,
	.name            = "ttm_demo",
	.desc            = "TTM teaching demo",
	.date            = "20260826",
	.major           = 1,
	.minor           = 0,
};

static int demo_setup_device(void)
{
	int err;

	demo_pdev = platform_device_register_simple("ttm_demo",
						    PLATFORM_DEVID_NONE,
						    NULL, 0);
	if (IS_ERR(demo_pdev))
		return PTR_ERR(demo_pdev);

	memset(&demo_drm, 0, sizeof(demo_drm));
	memset(&demo_vma_mgr, 0, sizeof(demo_vma_mgr));
	demo_drm.dev = &demo_pdev->dev;
	demo_drm.driver = &demo_drm_driver;
	demo_drm.driver_features = DRIVER_GEM;

	demo_anon_file = anon_inode_getfile("[ttm_demo]", &demo_anon_fops,
					    NULL, O_RDWR | O_CLOEXEC);
	if (IS_ERR(demo_anon_file)) {
		err = PTR_ERR(demo_anon_file);
		demo_anon_file = NULL;
		goto err_pdev;
	}
	demo_drm.anon_inode = demo_anon_file->f_inode;
	ihold(demo_drm.anon_inode);

	demo_drm.vma_offset_manager = &demo_vma_mgr;
	drm_vma_offset_manager_init(demo_drm.vma_offset_manager,
				    DRM_FILE_PAGE_OFFSET_START,
				    DRM_FILE_PAGE_OFFSET_SIZE);
	demo_drm_ok = true;

	err = ttm_device_init(&demo_bdev, &demo_funcs, demo_drm.dev,
			      demo_anon_file->f_inode->i_mapping,
			      demo_drm.vma_offset_manager, false, false);
	if (err)
		goto err_drm;
	demo_ttm_ok = true;

	/* use_tt=true：假 VRAM 仍用系统页做 backing，避免真 iomem */
	err = ttm_range_man_init(&demo_bdev, TTM_PL_VRAM, true, DEMO_VRAM_PAGES);
	if (err)
		goto err_ttm;
	demo_vram_ok = true;

	pr_info("device ready: fake VRAM = %u pages (%u KB)\n",
		DEMO_VRAM_PAGES, (DEMO_VRAM_PAGES * (unsigned int)PAGE_SIZE) / 1024);
	return 0;

err_ttm:
	ttm_device_fini(&demo_bdev);
	demo_ttm_ok = false;
err_drm:
	if (demo_drm_ok) {
		drm_vma_offset_manager_destroy(demo_drm.vma_offset_manager);
		if (demo_drm.anon_inode)
			iput(demo_drm.anon_inode);
		demo_drm.anon_inode = NULL;
		demo_drm.vma_offset_manager = NULL;
		demo_drm_ok = false;
	}
	if (demo_anon_file) {
		fput(demo_anon_file);
		demo_anon_file = NULL;
	}
err_pdev:
	platform_device_unregister(demo_pdev);
	demo_pdev = NULL;
	return err;
}

static void demo_teardown_device(void)
{
	if (demo_vram_ok) {
		ttm_range_man_fini(&demo_bdev, TTM_PL_VRAM);
		demo_vram_ok = false;
	}
	if (demo_ttm_ok) {
		ttm_device_fini(&demo_bdev);
		demo_ttm_ok = false;
	}
	if (demo_drm_ok) {
		drm_vma_offset_manager_destroy(demo_drm.vma_offset_manager);
		if (demo_drm.anon_inode)
			iput(demo_drm.anon_inode);
		demo_drm.anon_inode = NULL;
		demo_drm.vma_offset_manager = NULL;
		demo_drm_ok = false;
	}
	if (demo_anon_file) {
		fput(demo_anon_file);
		demo_anon_file = NULL;
	}
	if (demo_pdev) {
		platform_device_unregister(demo_pdev);
		demo_pdev = NULL;
	}
}

static void demo_bo_put(struct demo_bo *dbo)
{
	if (dbo && !IS_ERR(dbo))
		ttm_bo_put(&dbo->tbo);
}

static int demo_run(void)
{
	struct demo_bo *a = NULL, *b = NULL, *c = NULL, *d = NULL;
	struct dma_fence *busy = NULL;
	u32 magic = DEMO_MAGIC, got = 0;
	int err;

	pr_info("=== TTM demo start ===\n");

	/* [1] SYSTEM 分配：只记账，页可以还没 populate */
	pr_info("[1] 在 SYSTEM 创建 A (4KB)\n");
	a = demo_bo_create("A", PAGE_SIZE, &pl_sys);
	if (IS_ERR(a))
		return PTR_ERR(a);

	err = demo_bo_cpu_rw(a, &magic, true);
	if (err) {
		pr_err("CPU write A failed: %d\n", err);
		goto out;
	}
	pr_info("  CPU 写入 A[0]=0x%x，此时 ttm_tt 被 populate\n", magic);
	dump_bo("write", a);

	/* [2] dma_resv 上挂着未完成 fence 时，TTM 拒绝搬移 */
	pr_info("[2] 给 A 登记未 signal 的 KERNEL fence，再 validate 到 VRAM（TOPDOWN）\n");
	spin_lock_init(&demo_fence_lock);
	busy = demo_fence_new();
	if (!busy) {
		err = -ENOMEM;
		goto out;
	}
	err = demo_attach_busy_fence(a, busy);
	if (err)
		goto out;

	err = demo_bo_validate(a, &pl_vram_top, true);
	pr_info("  no_wait_gpu=1 期望 -EBUSY，实际 %d（%s）\n",
		err, err == -EBUSY ? "符合" : "意外");
	if (err && err != -EBUSY)
		goto out;

	dma_fence_signal(busy);
	pr_info("  signal 之后再搬：A 从 VRAM 顶部往下分配，避免切碎空闲区间\n");
	err = demo_bo_validate(a, &pl_vram_top, true);
	if (err)
		goto out;
	dump_vram("after-A");

	/* [3] 再放一个 8KB 进 VRAM：4+8=12KB，还剩 4KB */
	pr_info("[3] 直接在 VRAM 创建 B (8KB)\n");
	b = demo_bo_create("B", PAGE_SIZE * 2, &pl_vram);
	if (IS_ERR(b)) {
		err = PTR_ERR(b);
		b = NULL;
		goto out;
	}
	dump_vram("after-B");

	/* [4] C 也要 8KB。布局是 [B B][_][A]，连续空闲不够，先 eviction A */
	pr_info("[4] 再创建 C (8KB) 进 VRAM：应只 eviction A，B 留在 VRAM\n");
	c = demo_bo_create("C", PAGE_SIZE * 2, &pl_vram);
	if (IS_ERR(c)) {
		err = PTR_ERR(c);
		c = NULL;
		goto out;
	}
	dump_bo("evicted", a);
	dump_bo("kept", b);
	dump_bo("new", c);
	dump_vram("after-C");

	err = demo_bo_cpu_rw(a, &got, false);
	if (err)
		goto out;
	pr_info("  eviction 后读 A[0]=0x%x（%s），说明 move_null 只换标签、页还在\n",
		got, got == DEMO_MAGIC ? "magic 还在" : "数据丢了");

	/* [5] pin 住 B 后，再挤 8KB：只能踢 C，不能踢 B
	 * 6.8 已删除 TTM_PL_FLAG_NO_EVICT，改用 ttm_bo_pin()。
	 */
	pr_info("[5] pin B，再创建 D (8KB)：应 eviction C，B 留在 VRAM\n");
	err = ttm_bo_reserve(&b->tbo, false, false, NULL);
	if (err)
		goto out;
	ttm_bo_pin(&b->tbo);
	ttm_bo_unreserve(&b->tbo);

	d = demo_bo_create("D", PAGE_SIZE * 2, &pl_vram);
	if (IS_ERR(d)) {
		err = PTR_ERR(d);
		d = NULL;
		goto out_unpin;
	}
	dump_bo("pinned", b);
	dump_bo("evicted", c);
	dump_bo("new", d);
	dump_vram("after-D");
	err = 0;

out_unpin:
	if (!ttm_bo_reserve(&b->tbo, false, false, NULL)) {
		if (b->tbo.pin_count)
			ttm_bo_unpin(&b->tbo);
		ttm_bo_unreserve(&b->tbo);
	}

out:
	demo_bo_put(d);
	demo_bo_put(c);
	demo_bo_put(b);
	demo_bo_put(a);
	dma_fence_put(busy);

	if (!err)
		pr_info("=== TTM demo done ===\n");
	else
		pr_err("=== TTM demo failed: %d ===\n", err);
	return err;
}

static int __init ttm_demo_init(void)
{
	int err;

	err = demo_setup_device();
	if (err)
		return err;

	err = demo_run();
	/* 跑完立刻拆设备，避免 anon_inode file 把模块钉在 in-use */
	demo_teardown_device();
	return err;
}

static void __exit ttm_demo_exit(void)
{
	pr_info("=== TTM demo unloaded ===\n");
}

module_init(ttm_demo_init);
module_exit(ttm_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal TTM teaching demo");
MODULE_SOFTDEP("pre: ttm");
