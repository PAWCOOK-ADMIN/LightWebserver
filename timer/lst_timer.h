#ifndef LST_TIMER
#define LST_TIMER

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

class util_timer;

// 客户端数据
struct client_data {
    sockaddr_in address;            // 客户端 IP 地址
    int sockfd;                     // 连接 socket 对应的文件描述符
    util_timer *timer;              // 定时器
};

// 定时器类，链表结点
class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                      // 连接存活时间，绝对时间
    
    void (* cb_func)(client_data *);    // 超时处理函数

    client_data *user_data;             // 该定时器中保存的客户端数据
    util_timer *prev;                   // 前一个结点
    util_timer *next;                   // 后一个结点
};

// 定时器链表，升序、双向。带有头结点和尾节点
class sort_timer_lst {
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);                  // 增加一个定时器
    void adjust_timer(util_timer *timer);               // 调整一个定时器
    void del_timer(util_timer *timer);                  // 删除一个定时器
    void tick();                                        // SIGALARM 信号每次被触发时，都在其信号处理函数中调用一次 tick

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;               // 头结点
    util_timer *tail;               // 尾结点
};

// 工具类，提供一些常用的功能函数
class Utils {
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;           // 管道
    sort_timer_lst m_timer_lst;
    static int u_epollfd;           // epoll 实例
    int m_TIMESLOT;                 // 定时器的超时时间
};

void cb_func(client_data *user_data);

#endif

