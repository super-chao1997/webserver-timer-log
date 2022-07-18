#ifndef LOCKER_H
#define LOCKER_H
#include <exception>
#include <pthread.h>
#include <semaphore.h>
// 线程同步机制封装类

// 互斥锁类
class locker {
public:
    locker() {//构造函数，初始化互斥锁
        if(pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker() {//析构函数，销毁线程锁，释放内存
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock() {//加锁
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock() {//解锁
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get()//获取互斥量
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;//互斥锁变量
};

// 条件变量类
class cond {
public:
    cond(){
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t *m_mutex) {//等待，调用了该函数，线程会阻塞
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {//等待时间，调用了该函数，线程会阻塞直到时间结束
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }
    bool signal() {//唤醒一个或者多个等待的线程
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast() {//唤醒所有的等待的线程
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

// 信号量类
class sem {
public:
    sem() {
        if( sem_init( &m_sem, 0, 0 ) != 0 ) {
            throw std::exception();
        }
    }
    sem(int num) {
        if( sem_init( &m_sem, 0, num ) != 0 ) {
            throw std::exception();
        }
    }
    ~sem() {
        sem_destroy( &m_sem );
    }
    bool wait() {// 等待信号量
        return sem_wait( &m_sem ) == 0;
    }
    bool post() {// 增加信号量
        return sem_post( &m_sem ) == 0;
    }
private:
    sem_t m_sem;
};

#endif