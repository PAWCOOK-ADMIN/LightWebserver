#include "webserver.h"

// 服务器初始化
WebServer::WebServer() {
    //http_conn类对象
    users = new http_conn[MAX_FD];

    // 设置 webserver 提供资源服务的文件夹路径
    char server_path[200];
    getcwd(server_path, 200);           // 获取当前工作目录
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;              // 监听套接字和连接套接字的触发模式
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    // LT + LT  （水平触发）
    if (0 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }

    //LT + ET
    else if (1 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }

    //ET + LT
    else if (2 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }

    //ET + ET   （边缘触发）
    else if (3 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 创建日志系统，并初始化
void WebServer::log_write() {
    if (0 == m_close_log) 
        Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, m_log_write);
}

// 创建并初始化数据库连接池
void WebServer::sql_pool() {
    m_connPool = connection_pool::GetInstance();                // 获取数据库连接池单例
    m_connPool->init("127.0.0.1", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 获取数据库中 client 表的用户名和密码信息
    users->initmysql_result(m_connPool);
}

// 为服务器创建线程池
void WebServer::thread_pool() {
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen() {

    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER) {
        struct linger tmp = {0, 1};         // 缺省 close() 的行为
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER) {
        struct linger tmp = {1, 1};         // close 时，通过发送 RST 分组来关闭该连接
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    // 设置监听套接字端口复用
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 为监听套接字绑定socket地址
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));

    assert(ret >= 0);

    // 将 socket 标记为被动，表示可以开始接受连接请求
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // 初始化定时器的超时时间
    utils.init(TIMESLOT, m_close_log);

    // 创建 epoll 实例
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 把 listenfd 的文件描述符加入 epoll 实例的内核事件表中
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);            // 创建一个 unix 域的 socket 套接字对，使用方式和 pipe 类似。
    assert(ret != -1);

    utils.setnonblocking(m_pipefd[1]);                              // 写端非阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);                  // 读端加入 epoll 事件表

    utils.addsig(SIGPIPE, SIG_IGN);                                 // 向一个没有读端的管道写，会产生一个 SIGPIPE 信号
    utils.addsig(SIGALRM, utils.sig_handler, false);                // 实时定时器过期，会产生一个 SIGALRM 信号
    utils.addsig(SIGTERM, utils.sig_handler, false);                // 收到 SIGTERM 终止进程信号时的信号处理函数。

    alarm(TIMESLOT);                    // 开始定时器倒计时

    // 工具类, 信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 通过连接套接字创建一个 htppconn 对象，同时为客户端连接创建一个定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);            // 初始化客户端连接

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    tw_timer *timer = utils.m_time_wheel.add_timer(3 * TIMESLOT);
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;                       // 初始化定时器超时处理函数
    
    users_timer[connfd].timer = timer;

}

// 若有数据传输，则将定时器往后延迟 3 个单位
// 并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(tw_timer *timer) {
    utils.m_time_wheel.adjust_timer(timer);
}

// 删除定时器
void WebServer::deal_timer(tw_timer *timer, int sockfd) {
    timer->cb_func(&users_timer[sockfd]);                       // 先调用超时处理函数（作用关闭 TCP 连接）
    if (timer)
        utils.m_time_wheel.del_timer(timer);                     // 再从定时器链表中删除定时器

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 处理监听套接字上的可读数据
bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    if (0 == m_LISTENTrigmode) {            // 如果监听套接字是水平触发模式
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }

        timer(connfd, client_address);          // 通过连接套接字创建一个 htppconn 对象，同时为客户端连接创建一个定时器
    }

    else {                                      // 如果监听套接字是边缘触发模式, 则必须一直读监听套接字，直到没有数据可读
        while (1) {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 处理系统产生的信号
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);           // 从管道中接受信号，进行处理

    if (ret == -1)
        return false;
    else if (ret == 0)
        return false;
    else {
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                case SIGTERM: {             // 如果产生了 SIGTERM 信号，则停止服务器
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd) {
    tw_timer *timer = users_timer[sockfd].timer;

    if (1 == m_actormodel) {                // reactor，事件处理模型，主线程不读连接套接字，只是把连接套接字包装成任务，插入线程池中的任务队列中
        if (timer)
            adjust_timer(timer);

        // 若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);          // 读为 0

        while (true) {
            if (1 == users[sockfd].improv) {        // 子线程读数据完毕       
                if (1 == users[sockfd].timer_flag) {            // 子线程读数据失败
                    deal_timer(timer, sockfd);                  // 删除定时器
                    users[sockfd].timer_flag = 0;   
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {                                  // 模拟的 proactor，事件处理模型，主线程读连接套接字，再把数据和连接套接字包装成任务，插入线程池中的任务队列中
        if (users[sockfd].read_once()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));         // 开始读连接套接字

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
                adjust_timer(timer);
        }
        else
            deal_timer(timer, sockfd);              // 删除定时器，并关闭 TCP 连接
    }
}

void WebServer::dealwithwrite(int sockfd){
    tw_timer *timer = users_timer[sockfd].timer;
    

    if (1 == m_actormodel) {        // reactor 事件处理模式
        if (timer)
            adjust_timer(timer);

        m_pool->append(users + sockfd, 1);

        while (true){
            if (1 == users[sockfd].improv) {            // 子线程写数据完毕       
                if (1 == users[sockfd].timer_flag) {            // 子线程写数据失败
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else {                          // proactor 事件处理模式
        if (users[sockfd].write()) {            // 写数据成功
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
            if (timer)
                adjust_timer(timer);                    // 重置连接套接字定时器
        }
        else                                    // 写数据失败
            deal_timer(timer, sockfd);                  // 删除连接套接字定时器
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;                   // 标记上一次产生的 SIGALARM 是否被处理
    bool stop_server = false;               // 标记是否停止服务器的运行

    while (!stop_server) {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);           // 等待 epoll 内核事件
        
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 循环处理 epoll 内核事件
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;                 // 获取事件对应的文件描述符

            if (sockfd == m_listenfd) {                                                     // 监听套接字可读，处理新到的客户连接
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {               // EPOLLHUP：挂断    EPOLLRDHUP：对端套接字关闭    EPOLLERR：有错误发生 
                tw_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {             // 管道文件描述符可读，说明有信号等待处理
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            else if (events[i].events & EPOLLIN)                                            // 连接套接字可读
                dealwithread(sockfd);
            else if (events[i].events & EPOLLOUT)                                           // 连接套接字可写
                dealwithwrite(sockfd);
        }

        if (timeout) {                  // 只有在上一次的 SIGALARM 被捕捉后，才开始新一轮的倒计时，同时处理不活跃连接
            utils.timer_handler();
            timeout = false;
        }
    }

}