
#ifndef __THREADPOLL_H
#define __THREADPOLL_H 

#include <vector>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <memory>

#define MAX_THREAD_NUM 256

class ThreadPool
{
private:
	using Task = std::function<void()>;
	std::vector<std::thread> pool;	    	//线程池
	std::queue<Task> tasks;				    //任务

	std::mutex m_lock;				
	std::condition_variable m_cond;			//条件变量

	std::atomic<bool> stoped;				//是否关闭任务提交
	std::atomic<int>  idl_thread_num;		//空闲线程数

public:
	ThreadPool(unsigned short thread_num);
	~ThreadPool();
	int idl_num();
	template<typename F, typename ...Args>
		auto enqueue(F &&f, Args&& ...args) ->std::future<decltype(f(args...))>;
};

inline ThreadPool::ThreadPool(unsigned short thread_num):stoped(false)
{
	if (thread_num > MAX_THREAD_NUM)
		thread_num = MAX_THREAD_NUM;

	idl_thread_num = thread_num < 1 ? 1 : thread_num;
	for (int i = 0; i < idl_thread_num; i++)
	{
		pool.emplace_back(
				[this]()
				{
					//工作线程
					while (!this->stoped)
					{
						Task task;
						//获取任务函数
						{
						//unique_lock 块结束后自动释放锁
							std::unique_lock<std::mutex> lock (this->m_lock);
							//阻塞,直到有任务
							this->m_cond.wait(lock, [this]()
							{
								return this->stoped.load() || !this->tasks.empty();
							}
							);
							if(this->stoped || this->tasks.empty())
								return;
							task = std::move(tasks.front());  //拿到任务
							tasks.pop();
						}
						idl_thread_num++;
						//执行任务
						task();
						idl_thread_num--;

					}
				}
		);
	}
}

inline ThreadPool::~ThreadPool()
{
	stoped.store(true);
	m_cond.notify_all();

	for (std::thread &thread : pool)
	{
		if (thread.joinable())
			thread.join();
	}
}

template<typename F, typename ...Args>
auto ThreadPool::enqueue(F &&f, Args&&...args) ->std::future<decltype(f(args...))>
{
	if(stoped.load())
		throw std::runtime_error("commit on thread pool is stoped");
	using RetType = decltype(f(args...));  //获取函数f的返回值类型
	auto task = std::make_shared<std::packaged_task<RetType()>>(
			std::bind(std::forward<F>(f), std::forward<Args>(args)...)
			);
	//得到task 返回值 
	std::future<RetType> future = task->get_future();
	//添加任务
	{
		std::lock_guard<std::mutex> lock(m_lock);
		tasks.emplace(
				[task]()
				{
					(*task)();
				}
			);
	}

	m_cond.notify_one(); //唤醒线程
	return future;
}

inline int ThreadPool::idl_num()
{
	return idl_thread_num;
}


#endif
