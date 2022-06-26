#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;     // 异步写标志关闭
}

Log::~Log()
{
    if (m_fp != NULL)
        fclose(m_fp);
}

// file_name：./ServerLog
// log_buf_size：日志缓冲区大小，2000
// split_lines：日志最大行数，800000
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int m_log_write)
{  
    m_close_log = close_log;                    // 是否关闭日志
    m_log_buf_size = log_buf_size;              // 单条日志的缓冲区大小
    m_buf = new char[m_log_buf_size];           // 初始化日志缓冲区
    memset(m_buf, 0, m_log_buf_size);

    m_split_lines = split_lines;                // 日志文件最大行数

    time_t t = time(NULL);                      // 获取当前日历时间
    struct tm *sys_tm = localtime(&t);          // 使用 time_t 来填充 tm 结构
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');    // 搜索 '/' 最后一次出现的位置
    char log_full_name[256] = {0};              // 完整日志文件名

    // 构造完整的日志文件名
    if (p == NULL)
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    else {
        strcpy(log_name, p + 1);                // server
        strncpy(dir_name, file_name, p - file_name + 1);        // 设置目录名（./）
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);       // ./2022_06_16_ServerLog
    }

    // 记录当前时间
    m_today = my_tm.tm_mday;               
    
    // 打开日志文件，如果不存在则创建
    m_fp = fopen(log_full_name, "a");       
    if (m_fp == NULL)
        return false;

    // 如果日志的写入方式为异步，则初始化双缓冲系统
    if (m_log_write == 1) {
        m_is_async = true;                                      // 日志系统中的异步标志设为 true
        
        double_buffer &tmp = double_buffer::getinstance();                           // 获取双缓冲系统实例
        tmp.setfp(m_fp);                                   // 设置双缓冲系统的目标日志文件
        tmp.init();                                        // 为双缓冲系统启动后端写日志线程
    }

    return true;
}

// 写日志
// level：日志级别
void Log::write_log(int level, const char *format, ...)
{
    // 获取当前时间
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);

    time_t t = now.tv_sec;                  // 当前日历秒
    struct tm *sys_tm = localtime(&t);      // 将日历时间转换成年月日
    struct tm my_tm = *sys_tm;

    char s[16] = {0};
    switch (level) {
        case 0:
            strcpy(s, "[debug]:");          // 日志级别
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }

    // 写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;          // 行数增1

    // 如果日志系统保存的日期落后于当日，或者日志文件已经写的行数已达最大
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {       
        char new_log[256] = {0};
        fflush(m_fp);               // 刷新日志系统当前写的日志文件
        fclose(m_fp);               // 关闭当前写的日志文件
        char tail[16] = {0};
       
        // 创建新的日志文件
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday) {         // 如果时间落后的话
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else                                    // 如果日志已达最大的话
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        
        // 打开新的日志文件
        m_fp = fopen(new_log, "a");
        if (m_is_async)
            double_buffer::getinstance().setfp(m_fp);
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);            // 将 valst 指向可变参数的第一个位置

    string log_str;
    m_mutex.lock();

    // 构造一条日志
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ", my_tm.tm_year + 1900, my_tm.tm_mon + 1, 
                                        my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async)                 // 异步写
        double_buffer::getinstance().push(log_str);
    else {                          // 同步写
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    fflush(m_fp);               // 刷新 FILE 结构体中的缓冲区到内核缓冲区
    m_mutex.unlock();
}
