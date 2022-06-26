#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool {

public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();

    bool append(T *request, int state);     // 添加任务到任务队列中
    bool append_p(T *request);              

private:
    static void *worker(void *arg);         // 线程的主函数，之所以要定义为 static，是因为 c++ 的要求
    void run();                             // 处理客户请求的逻辑单元

private:
    int m_thread_number;                // 线程池中的线程数
    int m_max_requests;                 // 任务队列中允许的最大请求数
    pthread_t *m_threads;               // 线程池中的线程句柄数组，其大小为 m_thread_number
    std::list<T *> m_workqueue;         // 工作队列
    locker m_queuelocker;               // 保护工作队列的互斥锁，使得多个线程能够互斥地对任务队列进行访问
    sem m_queuestat;                    // 信号量，表示工作队列中是否有任务需要处理，用于同步线程对任务队列的访问（比如，什么时候可以访问队列，什么时候该等待）
    connection_pool *m_connPool;        // 数据库
    int m_actor_model;                  // 并发模型
};

// 线程池的构造函数
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : 
     m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool) {

    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_number];         // 保存线程池中的线程句柄数组
    if (!m_threads)
        throw std::exception();

    // 创建 thread_number 个线程，并设置为脱离状态
    for (int i = 0; i < thread_number; ++i) {
        // std::cout << "create the " << i << "th thread" << std::endl;
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}

// 给工作队列添加任务对象（reactor 模式）
template <typename T>
bool threadpool<T>::append(T *request, int state) {

    m_queuelocker.lock();                           // 需要对队列上锁，因为该队列被所有线程共享
    if (m_workqueue.size() >= m_max_requests) {     // 如果工作队列已满
        m_queuelocker.unlock();
        return false;
    }

    request->m_state = state;
    m_workqueue.push_back(request);                 // 添加请求到工作队列中
    m_queuelocker.unlock();
    m_queuestat.post();                             // 信号量增 1
    return true;
}

// 给工作队列添加任务对象（proactor 模式）
template <typename T>
bool threadpool<T>::append_p(T *request) {

    m_queuelocker.lock();                           // 需要对队列上锁，因为该队列被所有线程共享
    if (m_workqueue.size() >= m_max_requests) {     // 如果工作队列已满
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);                 // 添加请求到工作队列中
    m_queuelocker.unlock();
    m_queuestat.post();                             // 信号量增 1
    return true;
}

// 线程的主函数
template <typename T>
void *threadpool<T>::worker(void *arg) {

    threadpool *pool = (threadpool *)arg;
    pool->run();            // 处理客户请求的逻辑单元
    return pool;
}

// 处理客户请求的逻辑单元
template <typename T>
void threadpool<T>::run() {

    while (true) {
        m_queuestat.wait();                 // 等待信号量，也即等待任务队列中存在请求
        m_queuelocker.lock();               // 如果请求到来，则获取任务队列的一个请求

        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        T *request = m_workqueue.front();   // 从任务队列中取出一个请求
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request)
            continue;

        if (1 == m_actor_model) {           // 如果是 reactor 模式的话
            if (0 == request->m_state) {            // 如果是读数据的话
                if (request->read_once()) {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);           // 为 tcp 连接申请一个数据库连接, 注意connectRAII 定义的位置，当线程处理完本次连接请求时，连接将会被释放
                    request->process();             // 调用请求中的处理函数，处理请求
                }
                else {                                          // 如果读数据失败
                    request->improv = 1;                                // 子线程读数据完毕
                    request->timer_flag = 1;                            // 子线程读数据失败
                }
            }
            else {                                  // 如果是写数据的话
                if (request->write())
                    request->improv = 1;
                else {                                          // 如果写数据失败
                    request->improv = 1;                                // 子线程写数据完毕
                    request->timer_flag = 1;                            // 子线程写数据失败
                }
            }
        }
        else {                              // 如果是 proactor 模式的话
            connectionRAII mysqlcon(&request->mysql, m_connPool);               // 为 tcp 连接申请一个数据库连接
            request->process();                                                 // 调用请求中的处理函数，处理请求
        }
    }
}


#endif
