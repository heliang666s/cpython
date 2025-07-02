#!/usr/bin/env python3
"""
锁性能测试脚本
用于验证自适应自旋锁优化的效果
"""

import threading
import time
import statistics
import multiprocessing
import sys

def lock_stress_test(lock, iterations=100000):
    """测试锁的性能"""
    def worker():
        for _ in range(iterations):
            with lock:
                # 模拟极短的临界区
                pass
    
    results = {}
    
    # 测试不同线程数
    for num_threads in [1, 2, 4, 8, 16]:
        times = []
        
        # 多次运行取平均值
        for _ in range(3):
            threads = []
            start_time = time.perf_counter()
            
            for _ in range(num_threads):
                t = threading.Thread(target=worker)
                threads.append(t)
                t.start()
            
            for t in threads:
                t.join()
            
            elapsed = time.perf_counter() - start_time
            times.append(elapsed)
        
        avg_time = statistics.mean(times)
        std_dev = statistics.stdev(times) if len(times) > 1 else 0
        ops_per_sec = (num_threads * iterations) / avg_time
        
        results[num_threads] = {
            'avg_time': avg_time,
            'std_dev': std_dev,
            'ops_per_sec': ops_per_sec
        }
        
        print(f"线程数: {num_threads:2d}, "
              f"平均耗时: {avg_time:.3f}s (±{std_dev:.3f}), "
              f"操作/秒: {ops_per_sec:,.0f}")
    
    return results

def cpu_bound_task(lock, shared_counter, iterations):
    """CPU 密集型任务测试"""
    local_sum = 0
    for i in range(iterations):
        # 一些计算
        local_sum += i * i
        
        # 偶尔访问共享资源
        if i % 1000 == 0:
            with lock:
                shared_counter[0] += 1
    
    return local_sum

def mixed_workload_test(iterations=10000):
    """混合工作负载测试"""
    lock = threading.Lock()
    shared_counter = [0]
    
    print("\n混合工作负载测试（CPU密集+锁竞争）：")
    
    for num_threads in [2, 4, 8]:
        threads = []
        start_time = time.perf_counter()
        
        for _ in range(num_threads):
            t = threading.Thread(
                target=cpu_bound_task,
                args=(lock, shared_counter, iterations)
            )
            threads.append(t)
            t.start()
        
        for t in threads:
            t.join()
        
        elapsed = time.perf_counter() - start_time
        print(f"线程数: {num_threads}, 耗时: {elapsed:.3f}s, "
              f"共享计数器: {shared_counter[0]}")
        
        shared_counter[0] = 0

def main():
    print("=" * 60)
    print("Python 锁性能测试")
    print(f"CPU 核心数: {multiprocessing.cpu_count()}")
    print(f"Python 版本: {sys.version}")
    print("=" * 60)
    
    # 基本锁测试
    print("\n基本锁竞争测试：")
    lock = threading.Lock()
    results = lock_stress_test(lock)
    
    # 分析结果
    print("\n性能分析：")
    baseline = results[1]['ops_per_sec']
    for threads, data in results.items():
        if threads > 1:
            speedup = data['ops_per_sec'] / baseline
            efficiency = speedup / threads * 100
            print(f"{threads} 线程: 加速比 {speedup:.2f}x, "
                  f"并行效率 {efficiency:.1f}%")
    
    # 混合负载测试
    mixed_workload_test()
    
    print("\n测试完成！")

if __name__ == "__main__":
    main()