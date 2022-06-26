#include "tw_timer.h"
#include "../http/http_conn.h"



// 删除定时器
void time_wheel::del_timer(tw_timer* timer)
{
    if(!timer)
        return;

    int ts = timer->time_slot;

    if(timer == slots[ts]) {            // 如果定时器是链表头
        slots[ts] = slots[ts]->next;
        if(slots[ts])
            slots[ts]->prev = NULL;
    }
    else {                              // 如果被删除的定时器不是表头
        timer->prev->next = timer->next;
        if(timer->next)
            timer->next->prev = timer->prev;
    }

    delete timer;
    LOG_INFO("the timer of %d has removed", timer->user_data->sockfd);
    len--;
}

// 删除当前槽上过期的定时器, 转动时间轮
void time_wheel::tick()
{
    LOG_INFO("time tick, the number of client is: %d", len);
    tw_timer* p = slots[cur_slot];
    
    while(p) {

        if(p->rotation > 0) {         // 如果定时器还没到期，则定时器的轮次减一
            p->rotation--;
            p = p->next;
        }
        else {                        // 如果定时器到期了
            LOG_INFO("The unactive conn of %d has expired", p->user_data->sockfd);

            p->cb_func(p->user_data);           // 执行定时器超时处理函数
            
            tw_timer* tmp = p->next;
            del_timer(p);
            p = tmp;
        }
    }
    cur_slot = ++cur_slot % N;        // 转动时间轮
}

// 调整定时器的到期时间
void time_wheel::adjust_timer(tw_timer* timer)
{
    // 1、将定时器从时间轮中摘除
    int ts = timer->time_slot;

    if(timer == slots[ts]) {            // 如果定时器是链表头
        slots[ts] = slots[ts]->next;
        if(slots[ts])
            slots[ts]->prev = NULL;
    }
    else {                              // 如果定时器不是链表头
        timer->prev->next = timer->next;
        if(timer->next)
            timer->next->prev = timer->prev;
    }

    // 2、重新设置超时时间
    int ticks = 3 * TIMESLOT / TI;
    int rotation = ticks / N;                       // 超时轮次
    ts = (cur_slot + (ticks % N)) % N;          // 定时器应插入的槽

    timer->rotation = rotation;
    timer->time_slot = ts;
    // 这里有个 bug，找了好久, 即摘除的的定时器，其前后指针一定要重新设置为 nullpter
    timer->prev = nullptr;
    timer->next = nullptr;

    // 3、重新添加进时间轮
    if(!slots[ts])                  // 如果槽目前是空的
        slots[ts] = timer;
    else {                          // 如果槽目前不是空的
        timer->next = slots[ts];            // 头插法
        slots[ts]->prev = timer;
        slots[ts] = timer;
    }

    LOG_INFO("adjust the timer of conn: %d, sum: %d", timer->user_data->sockfd, len);

}

// 添加定时器
tw_timer* time_wheel::add_timer(int timeout)
{
    if(timeout < 0)
        return NULL;

    int ticks;
    if(timeout < TI)            // 如果时间少于 3 秒，则槽间隔为 3 秒
        ticks = 1;
    else                        // 否则槽间隔为 timeout / TI
        ticks = timeout / TI;

    int rotation = ticks / N;                       // 超时轮次
    int ts = (cur_slot + (ticks % N)) % N;          // 定时器应插入的槽
    tw_timer* timer = new tw_timer(rotation, ts);

    if(!slots[ts])                  // 如果槽目前是空的
        slots[ts] = timer;
    else {                          // 如果槽目前不是空的
        timer->next = slots[ts];            // 头插法
        slots[ts]->prev = timer;
        slots[ts] = timer;
    }

    len++;
    LOG_INFO("add a timer: %d", len);
    return timer;
}


// 初始化 SIGALARM 下一次到来的时间
void Utils::init(int timeslot, int close_log) {
    m_TIMESLOT = timeslot;
    m_time_wheel.set_timeslot(m_TIMESLOT);
    m_time_wheel.set_log(close_log);
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)          
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;          // 边缘触发
    else
        event.events = EPOLLIN | EPOLLRDHUP;                    // 水平触发

    if (one_shot)
        event.events |= EPOLLONESHOT;                           // EPOLLONESHOT 标志

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);                                         // 设置为非阻塞。
}

// 信号处理函数，处理 SIGALARM 和 SIGTERM
void Utils::sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的 errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);                  // 将收到的信号发送至管道             
    errno = save_errno;
}

// 设置信号函数
// restart 表示是否自动重启由信号处理器程序中断的系统调用，比如 read
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;                    // 信号处理函数  

    if (restart)
        sa.sa_flags |= SA_RESTART;

    sigfillset(&sa.sa_mask);                    // 临时信号掩码
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 如果服务器的客户连接数以达到最大，则返回一个错误信息给客户端，并关闭连接
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() 
{
    m_time_wheel.tick();                 // 清除不活跃连接
    alarm(m_TIMESLOT);                   // 重新设定下一次的定时器闹钟
}

int *Utils::u_pipefd = 0;               // m_pipefd[1]: 写端        m_pipefd[0]：读端
int Utils::u_epollfd = 0;

class Utils;

// 定时器超时处理函数
void cb_func(client_data *user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);           // 从 epoll 实例中删除监听的文件描述符
    
    assert(user_data);
    
    close(user_data->sockfd);                         // 关闭客户端连接
    http_conn::m_user_count--;
}