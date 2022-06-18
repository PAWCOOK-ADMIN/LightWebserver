#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;               // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000;     // epoll 监听的最大文件描述数
const int TIMESLOT = 5;                 // 定时器的超时时间的最小单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();            // 为服务器创建线程池
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    int m_port;             // 服务器的端口
    char *m_root;           // root 文件夹路径
    int m_log_write;        // 日志写入方式（同步还是异步）
    int m_close_log;        // 是否关闭日志系统, 0 表示不关闭（默认）
    int m_actormodel;       // 事件处理模型选择，1 为 reactor，0 为 proactor 

    int m_pipefd[2];                // 信号事件处理机制使用的管道
    int m_epollfd;                  // 服务器中的 epoll 实例
    http_conn *users;               // 服务器中所有的客户端连接    

    //数据库相关
    connection_pool *m_connPool;
    string m_user;                  // 登陆数据库的用户名
    string m_passWord;              // 登陆数据库的密码
    string m_databaseName;          // 登录的数据库名
    int m_sql_num;                  // 数据库连接池数量

    //线程池相关
    threadpool<http_conn> *m_pool;  // 线程池
    int m_thread_num;               // 线程池的数量

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];       // epoll 事件列表

    int m_listenfd;                     // 监听套接字描述符
    int m_OPT_LINGER;                   // 优雅关闭链接
    int m_TRIGMode;                     // 组合触发模式
    int m_LISTENTrigmode;               // 监听套接字的触发模式
    int m_CONNTrigmode;                 // 连接套接字的触发模式

    //定时器相关
    client_data *users_timer;           // 管理所有客户端连接的定时器
    Utils utils;
};
#endif
