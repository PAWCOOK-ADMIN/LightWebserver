#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
#include "double_buffer.h"

using namespace std;

class Log {
public:

    // 单例模式创建日志系统
    static Log *get_instance() {
        static Log instance;
        return &instance;
    }

    // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    // 写入消息
    void write_log(int level, const char *format, ...);

    // 刷新缓冲区
    void flush(void);

private:
    Log();
    virtual ~Log();

private:
    char dir_name[128];         // 目录名
    char log_name[128];         // log文件名

    int m_split_lines;          // 日志最大行数
    int m_log_buf_size;         // 日志缓冲区大小
    char *m_buf;                // 构造单条日志时的缓冲区

    long long m_count;          // 日志行数记录
    int m_today;                // 因为按天分类, 记录当前时间是那一天
    FILE *m_fp;                 // 打开log的文件指针

    bool m_is_async;            // 是否是异步写入日志
    locker m_mutex;             // 同步日志缓冲队列的互斥量
    int m_close_log;            // 关闭日志，, 0 表示不关闭（默认）

};

//使用宏定义，便于调用， 使用##__VA_ARGS__，支持format后面可有0到多个参数
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}


#endif
