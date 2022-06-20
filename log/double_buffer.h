#ifndef DOUBLE_BUFFER
#define DOUBLE_BUFFER

#include <vector>
#include <string>
#include "../lock/locker.h"
#include <unistd.h>
#include <iostream>

using namespace std;

class double_buffer {
    typedef vector<string> Buffer;
    typedef vector<string>* bufptr;

    Buffer *cur, *next, *new1, *new2;          // 四个异步日志缓冲区

    vector<bufptr> front_buffers;              // 前端缓冲区数组
    vector<bufptr> back_buffers;               // 后端缓冲区数组
    
    bool stop;              // 后端写日志线程运行标志 
    
    locker m_mutex;         // 互斥量
    cond m_cond;            // 条件变量
    FILE *fp;               // 打开的日志文件描述符

public:
    static double_buffer& getinstance() {            // 单例模式
        static double_buffer d_buffer;
        return d_buffer;
    }

    double_buffer();

    void push(string);                          // 将日志写入缓冲区
    static void *thread_work(void *arg);        // 后端日志线程的主线程
    void d_buffer_write();                      // 将缓冲区日志写入磁盘
    void init();

    void setfp(FILE *p);
};




#endif