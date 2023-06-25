#ifndef THREAD_POOL
#define THREAD_POOL

#include <cstddef>
#include <exception>
#include<pthread.h>
#include<list>
#include"lock.h"             //处理下怎么都能找得到？
#include<cstdio>
#include <semaphore.h>
//线程池类
template<typename T>
class ThreadPool
{
public:
    
    ThreadPool(int thread_num=8,int max_task_queue=10000);//构造函数，创建线程，初始化任务队列大小
    ~ThreadPool();
    bool append(T* task);//生产者向任务队列添加任务，添加一个，信号量加一
private:
static void* worker(void *arg);//传给工作进程创建时的工作函数，消费者，从任务队列取出任务，信号量减一
void run();
private:
    //线程数量
    int thread_num;
    
    //线程池数组,大小为thread_num
    pthread_t* threads;

    //任务队列最大任务量
    int max_task_queue;

    //请求队列
    std::list<T*> task_queue;

    //互斥锁
    m_Mutex queue_mutex;

    //信号量
    m_Sem queue_sem;//初始化为0

    //是否结束线程
    bool _stop;
};


//1.初始化线程池对象
template<typename T>
ThreadPool<T>::ThreadPool(int thread_num,int max_task_queue)
:thread_num(thread_num),max_task_queue(max_task_queue),threads(NULL),
_stop(false),queue_sem(0)
{
    if(thread_num<=0||max_task_queue<=0)
    { throw::std::exception(); }

    //申请创建线程空间
    threads=new pthread_t[thread_num];
    if(!threads)
    { throw::std::exception(); }

    //创建线程
    for(int i=0;i<thread_num;i++)
    {
        printf("create the %dth thread\n",i+1);
        if(pthread_create(&threads[i], NULL, worker, this)!=0)
        {
            delete [] threads;
            throw::std::exception();
        }

        //创建线程成功则线程分离
        pthread_detach(threads[i]);

    }
}

//2.销毁线程池对象  
template<typename T>
ThreadPool<T>::~ThreadPool()
{
    delete [] threads;
    _stop=true;
}

//3.生产者，添加任务
template<typename T>
bool ThreadPool<T>::append(T *task)
{
    queue_mutex.lock();//队列上锁
    if(task_queue.size()>=max_task_queue)
    {
        queue_mutex.unlock();
        return false;
    }

    task_queue.push_back(task);
    queue_mutex.unlock();
    queue_sem.post();
    return true;
}

//消费者，工作函数，传入this指针访问线程池对象参数，要不停运行调用run函数
template<typename T>
void* ThreadPool<T>::worker(void *arg)
{
    ThreadPool* pool=(ThreadPool* )arg;
    pool->run();
    return pool;

}


//不停消费的函数，线程池对象不销毁就一直工作，没有任务（检测信号量）就阻塞
template<typename T>
void ThreadPool<T>::run()
{
    while (!_stop) 
    {
        queue_sem.wait();
        //取出任务
        queue_mutex.lock();
        if(task_queue.empty())
        {
            queue_mutex.unlock();
            continue;
        }
        T* task=task_queue.front();
        task_queue.pop_front();
        queue_mutex.unlock();

        if(!task)
        {
            continue;
        }
        //执行任务
        task->process();
    }
}
#endif