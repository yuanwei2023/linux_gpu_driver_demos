# dma_resv 同步机制 Demo

本模块使用手动 signal 的软件 `dma_fence`，演示 `dma_resv` 如何为一个
buffer 记录未完成操作，以及后续读写操作如何选择需要等待的 fence。

## 1. “登记 fence”的含义

```c
dma_resv_add_fence(&demo_resv, f_move,  DMA_RESV_USAGE_KERNEL);
dma_resv_add_fence(&demo_resv, f_draw,  DMA_RESV_USAGE_WRITE);
dma_resv_add_fence(&demo_resv, f_scan,  DMA_RESV_USAGE_READ);
dma_resv_add_fence(&demo_resv, f_evict, DMA_RESV_USAGE_BOOKKEEP);
```

`dma_resv_add_fence()` 将一次 buffer 相关操作的“完成凭证”登记到
`dma_resv` 中。它既不会启动操作，也不会主动等待操作完成。

本 demo 新创建的 fence 尚未 signal，因此登记后分别表示：

| Fence | Usage | 表示的状态 |
|---|---|---|
| `f_move` | `KERNEL` | 内核内存搬运尚未完成 |
| `f_draw` | `WRITE` | 对 buffer 的写操作尚未完成 |
| `f_scan` | `READ` | 对 buffer 的读操作尚未完成 |
| `f_evict` | `BOOKKEEP` | 记录生命周期，不参与普通隐式同步 |

表中存在 fence 不一定表示操作仍未完成。已 signal 的 fence 也可能暂时留在
表中，必须通过 `dma_fence_is_signaled()`、`dma_resv_test_signaled()` 或等待
接口判断其状态。

## 2. 后续操作等待哪些 fence

usage 的查询顺序为：

```text
KERNEL < WRITE < READ < BOOKKEEP
```

查询某个 usage 时，也会返回排在它前面的 fence。因此：

```text
后续新读操作
  └─ 查询 WRITE
     └─ 等待已有 KERNEL + WRITE
        不需要等待已有 READ

后续新写操作
  └─ 查询 READ
     └─ 等待已有 KERNEL + WRITE + READ

BOOKKEEP
  └─ 普通隐式同步不会等待
```

内核通过下面的 helper 选择查询范围：

```c
static inline enum dma_resv_usage dma_resv_usage_rw(bool write)
{
	return write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE;
}
```

这里看似“反过来”实际是正确的：

- 新读操作只会与已有写操作冲突。
- 新写操作会与已有读、写操作冲突。

## 3. 等待不是自动发生的

`dma_resv` 只是 buffer 的依赖登记表。后续访问者必须主动执行同步，例如：

1. 调用 `dma_resv_wait_timeout()`，让当前 CPU 线程阻塞等待；
2. 取出 fence，添加为 GPU job 的调度依赖，让 GPU job 异步等待；
3. CPU 访问 dma-buf 时，通过 `dma_buf_begin_cpu_access()` 等路径同步。

完整关系如下：

```text
提交 buffer 操作
      │
      ├─ 创建该操作的 completion fence
      │
      ├─ dma_resv_add_fence()
      │       └─ 将 fence 与 buffer 关联
      │
      ├─ 后续访问者查询冲突 fence
      │       └─ CPU wait 或加入 GPU job dependency
      │
      └─ 硬件完成
              └─ dma_fence_signal()
                     └─ 唤醒等待者/解除调度依赖
```

## 4. 编译与运行

```bash
make
sudo insmod dma_resv_demo.ko
sudo rmmod dma_resv_demo
dmesg
```

也可以直接执行：

```bash
make run
```

