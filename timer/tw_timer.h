#ifndef TIME_WHEEL_TIMER
#define TIME_WHEEL_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

#define BUFFER_SIZE 64

class tw_timer;

// 客户端数据
struct client_data
{
    sockaddr_in address;            // 客户端 IP 地址
    int sockfd;                     // 连接 socket 对应的文件描述符
    tw_timer* timer;                // 定时器
};

// 链表节点（定时器）
class tw_timer
{
public:
    tw_timer(int rot, int ts): next(NULL), prev(NULL), rotation(rot), time_slot(ts){}

public:
    int rotation;                       // 定时器超时的轮次，即时间轮转多少圈后，当前定时器会超时
    int time_slot;                      // 定时器应插入的时间轮的槽
    void (*cb_func)(client_data*);      // 超时处理函数
    client_data* user_data;             // 该定时器中保存的客户端数据
    tw_timer* next;                     // 后一个结点
    tw_timer* prev;                     // 前一个结点
};

class time_wheel
{
public:
    // 时间轮的构造函数
    time_wheel() : cur_slot(0) 
    {
        for(int i = 0; i < N; ++i)
            slots[i] = NULL;
        
        len = 0;
    }

    ~time_wheel() 
    {
        for(int i = 0; i < N; ++i) {
            tw_timer* tmp = slots[i];
            while(tmp) {
                slots[i] = tmp->next;
                delete tmp;
                tmp = slots[i];
            }
        }
    }

    void set_timeslot(int n) { TIMESLOT = n; }
    void set_log(int n) {m_close_log = n;}

    // 添加定时器
    tw_timer* add_timer(int timeout);

    // 删除定时器
    void del_timer(tw_timer* timer);
    
    // 删除当前槽上过期的定时器, 转动时间轮
    void tick();

    // 调整定时器的到期时间
    void adjust_timer(tw_timer* timer);
    

private:
    static const int N = 20;        // 槽的个数
    static const int TI = 3;        // 定时时间的基本单位  
    tw_timer* slots[N];             // 时间轮的槽
    int cur_slot;                   // 当前槽

    int len;
    int TIMESLOT;                  // SIGALARM 到来的间隔时间 
    int m_close_log;                // 是否开启日志
};

// 工具类，提供一些常用的功能函数
class Utils {
public:
    Utils() {}

    ~Utils() {}

    void init(int timeslot, int log);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;           // 管道
    time_wheel m_time_wheel;        // 时间轮
    static int u_epollfd;           // epoll 实例
    int m_TIMESLOT;                 // 下一次 SIGALARM 到来的时间
};

void cb_func(client_data *user_data);


#endif
