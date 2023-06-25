#ifndef _M_LOCK_
#define _M_LOCK_
#include <cstddef>
#include <cstdio>
#include<pthread.h>
#include <semaphore.h>

//互斥锁的封装
class m_Mutex
{
public:
    m_Mutex()
    {
        if(pthread_mutex_init(&m_mutex, NULL)!=0)
        {
            perror("mutex error");
        }
    }
    ~m_Mutex()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex)==0;
    }
    bool unlock()
    {
       return pthread_mutex_unlock(&m_mutex)==0;
    } 
    pthread_mutex_t* get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

//条件变量的封装
class m_Cond
{
public:
    m_Cond()
    {
        if(pthread_cond_init(&m_cond, NULL)!=0)
        {
            perror("cond error");
        }
    }
    ~m_Cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(m_Mutex& m_mutx)
    {
       return pthread_cond_wait(&m_cond,m_mutx.get())==0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond)==0;
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond)==0;
    }

private:
    pthread_cond_t m_cond;
    
};

////信号量的封装
class m_Sem
{
public:
    m_Sem(int num)
    {
        if(sem_init(&m_sem,0,num)!=0)
        {
            perror("sem error");
        }
    }
    ~m_Sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem)==0;
    }
    bool post()
    {
        return sem_post(&m_sem)==0;
    }


private:
    sem_t m_sem;
};
#endif