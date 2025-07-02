#!/usr/bin/env python3
"""
Asyncio 性能测试脚本
用于验证异步任务调度优化的效果
"""

import asyncio
import time
import statistics
import sys

async def simple_task(task_id, iterations=1000):
    """简单异步任务"""
    result = 0
    for i in range(iterations):
        result += i
        # 定期让出控制权
        if i % 100 == 0:
            await asyncio.sleep(0)
    return result

async def io_bound_task(task_id, delays=10):
    """I/O 密集型任务"""
    total_delay = 0
    for i in range(delays):
        delay = 0.001  # 1ms 延迟
        await asyncio.sleep(delay)
        total_delay += delay
    return total_delay

async def mixed_task(task_id, compute_iterations=500, io_operations=5):
    """混合型任务（计算+I/O）"""
    results = []
    
    for op in range(io_operations):
        # 计算部分
        result = 0
        for i in range(compute_iterations):
            result += i * i
        results.append(result)
        
        # I/O 部分
        await asyncio.sleep(0.001)
    
    return sum(results)

async def benchmark_tasks(task_func, num_tasks, *args):
    """通用基准测试函数"""
    start_time = time.perf_counter()
    
    tasks = [task_func(i, *args) for i in range(num_tasks)]
    results = await asyncio.gather(*tasks)
    
    elapsed = time.perf_counter() - start_time
    return elapsed, results

async def stress_test_event_loop(duration=1.0):
    """事件循环压力测试"""
    async def micro_task():
        await asyncio.sleep(0)
        return 1
    
    start_time = time.perf_counter()
    task_count = 0
    
    while time.perf_counter() - start_time < duration:
        tasks = [micro_task() for _ in range(100)]
        await asyncio.gather(*tasks)
        task_count += 100
    
    elapsed = time.perf_counter() - start_time
    tasks_per_second = task_count / elapsed
    
    return task_count, tasks_per_second

async def main():
    print("=" * 60)
    print("Asyncio 性能测试")
    print(f"Python 版本: {sys.version}")
    print("=" * 60)
    
    # 1. 简单任务测试
    print("\n1. 简单任务调度测试：")
    for num_tasks in [10, 50, 100, 500, 1000]:
        times = []
        for _ in range(3):
            elapsed, _ = await benchmark_tasks(simple_task, num_tasks)
            times.append(elapsed)
        
        avg_time = statistics.mean(times)
        tasks_per_sec = num_tasks / avg_time
        print(f"任务数: {num_tasks:4d}, 平均耗时: {avg_time:.3f}s, "
              f"任务/秒: {tasks_per_sec:.0f}")
    
    # 2. I/O 密集型测试
    print("\n2. I/O 密集型任务测试：")
    for num_tasks in [10, 50, 100]:
        elapsed, _ = await benchmark_tasks(io_bound_task, num_tasks)
        print(f"任务数: {num_tasks:3d}, 耗时: {elapsed:.3f}s")
    
    # 3. 混合型任务测试
    print("\n3. 混合型任务测试：")
    for num_tasks in [10, 25, 50]:
        elapsed, _ = await benchmark_tasks(mixed_task, num_tasks)
        print(f"任务数: {num_tasks:3d}, 耗时: {elapsed:.3f}s")
    
    # 4. 事件循环压力测试
    print("\n4. 事件循环压力测试：")
    for duration in [0.5, 1.0]:
        count, tps = await stress_test_event_loop(duration)
        print(f"测试时长: {duration}s, 完成任务: {count}, "
              f"任务/秒: {tps:,.0f}")
    
    # 5. 并发限制测试
    print("\n5. 大规模并发测试：")
    semaphore = asyncio.Semaphore(100)  # 限制并发数
    
    async def limited_task(task_id):
        async with semaphore:
            await asyncio.sleep(0.01)
            return task_id
    
    for num_tasks in [100, 500, 1000]:
        start = time.perf_counter()
        tasks = [limited_task(i) for i in range(num_tasks)]
        await asyncio.gather(*tasks)
        elapsed = time.perf_counter() - start
        print(f"任务数: {num_tasks:4d}, 耗时: {elapsed:.3f}s, "
              f"吞吐量: {num_tasks/elapsed:.0f} 任务/秒")
    
    print("\n测试完成！")

if __name__ == "__main__":
    asyncio.run(main())