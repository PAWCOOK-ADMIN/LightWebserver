#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义 http 响应报文中的原因短语
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;          // 保存所有的客户名和密码

//初始化数据库读取表
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);                          

    // 在 user 表中检索 username，passwd 数据，浏览器端输入
    if (mysql_query(mysql, "select name, passward from client"))            // 发送 sql 语句给数据库服务器执行
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);                          // 将执行结果返回至 result

    // 从结果集中获取下一行，将对应的用户名和密码，存入 map 中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }

    // 将信息输出出来，因为调试不方便查看信息
    for (auto user : users) {
        cout << "用户名: " << user.first << "\t\t" << "密码：" << user.second << endl; 
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，默认 LT 模式，选择开启 EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;          // 边缘触发
    else
        event.events = EPOLLIN | EPOLLRDHUP;                    // 水平触发

    if (one_shot)                                               
        event.events |= EPOLLONESHOT;                           // 如果是连接套接字，则启用 EPOLLONESHOT 标志

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为 EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;        // 边缘触发
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;                  // 水平触发，整个套接字上的读写事件只触发一次

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接, 外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或 http 响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;                    // 初始状态为检查请求行
    m_linger = false;                                           // 默认不保持链接  Connection : keep-alive保持连接
    m_method = GET;                                             // 默认请求方式为GET

    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    cgi = 0;                    // 是否启用 post
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, 0, READ_BUFFER_SIZE);
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx)                      // 如果读缓冲区的数据末尾中只有一个 \r，则行数据不完整
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') {           // 如果解析到 \r\n, 则将它们替换为 \0
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;                                            // 如果读缓冲区中只有一个 \r，则行出错
        }
        else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {             // 这种情况会出现吗
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞 ET 工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;

    int bytes_read = 0;

    if (0 == m_TRIGMode) {          // LT 读连接套接字的数据
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
            return false;

        return true;
    }
    else {                          // ET 读连接套接字的数据
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {                     // 读取数据失败
                if (errno == EAGAIN || errno == EWOULDBLOCK)        // 套接字的读缓冲区中的数据读完
                    break;   
                else                                                // recv 调用被中断或者连接 socket 收到了 RST 
                    return false;           
            }
            else if (bytes_read == 0)                                   // 对方关闭了写端，服务端收到 FIN，读到了文件结尾
                return false;

            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");           // 在字符串中搜索 “ \t” 中的某个字符，返回首次出现的指针

    if (!m_url)
        return BAD_REQUEST;  

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';                        // 置位空字符，字符串结束符
    char *method = text;

    if (strcasecmp(method, "GET") == 0)         // 忽略大小写比较
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;                    
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, " \t");              // 在字符串中搜索 “ \t” 中的某个字符，返回首次出现的指针

    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    // 到这一步，url 和 version 都已经处理好了

    if (strlen(m_url) == 1)                 //当 url 为 / 时，显示判断界面 judge.html
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析HTTP请求的一个首部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 遇到空行，表示首部字段解析完毕
    if (text[0] == '\0') {  
        if (m_content_length != 0) {                 // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
            m_check_state = CHECK_STATE_CONTENT;        
            return NO_REQUEST;
        }
        return GET_REQUEST;                          // 否则说明我们已经得到了一个完整的HTTP请求
    }
    // 处理Connection 头部字段  Connection: keep-alive
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
    }
    // 处理Content-Length头部字段
    else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
        LOG_INFO("oop!unknow header: %s", text);            // 如果解析到位置的首部字段
    return NO_REQUEST;
}

// 解析 http 报文主体
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {         // 如果请求报文并未超过读缓冲区的大小
        text[m_content_length] = '\0';
        m_string = text;                    // POST 请求中最后为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析 HTTP 请求，将请求的目标文件进行内存映射
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 如果当前主状态机的状态是解析报文主体且上一次读取到一个完整的行
    // 如果本次能读取到一个完整的行
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();                              // 获取一行数据
        m_start_line = m_checked_idx;                   // 设置下一行的起始位置
        LOG_INFO("%s", text);                                   // 将从连接套接字的读缓冲上解析到的每一行以日志的形式记录下来 

        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {             // 如果当前正在解析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER: {                  // 如果当前正在解析首部字段
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                    return do_request();                            // 解析完首部字段且报文主体为空，则直接开始处理本次 HTTP 请求 
                break;
            }
            case CHECK_STATE_CONTENT: {                 // 如果当前正在解析报文主体
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();                            // 解析到了报文主体后，开始处理本次 HTTP 请求 
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的 HTTP 请求时，我们就分析请求的目标文件的属性，如果目标文件存在、对所有用户可读，且不是目录，
// 则使用 mmap 将其映射到内存地址 m_file_address 处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/LightWebserver/root"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');

    // 当接受到的是表单提交的情况，即登录或者注册，将发送请求这样的（http://172.30.86.168:10001/3CGISQL.cgi）url
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];                   // m_url: /3CGISQL.cgi 或者 /2CGISQL.cgi

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");                // 添加 '/'
        strcat(m_url_real, m_url + 2);          // `/CGISQL.cgi`
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);       // "/home/nowcoder/LightWebserver/root/CGISQL.cgi"，
        free(m_url_real);

        //将用户名和密码提取出来，形式 user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)                // 取出用户名，以&为分隔符，前面的为用户名
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)     // 取出密码，以&为分隔符，后面的是密码
            password[j] = m_string[i];
        password[j] = '\0';

        // 如果是注册
        if (*(p + 1) == '3') {

            // 构造 sql 插入语句
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO client(name, passward) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 先检测数据库中是否有重名的
            if (users.find(name) == users.end()) {                      // 如果没有重名的，进行增加数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)                                               // 如果注册成功，则返回到登录页面
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else                                                        // 如果有重名的
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        else if (*(p + 1) == '2') {
            if (users.find(name) != users.end() && users[name] == password)        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    // judge.html 有两个按钮，点击注册按钮时会发送一个请求 /0 的请求 
    if (*(p + 1) == '0') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // judge.html 有两个按钮，点击登录按钮时会发送一个请求 /1 的请求
    else if (*(p + 1) == '1') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6') {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 获取m_request_path文件的相关的状态信息，-1 失败，0 成功
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH))       // 判断服务器线程是否有权限访问文件
        return FORBIDDEN_REQUEST;

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    // 创建私有文件映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    // 关闭文件
    close(fd);

    return FILE_REQUEST;
}

// 解除内存映射
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 向客户端 socket 发送 HTTP 响应
bool http_conn::write()
{
    int temp = 0;

    // 如果要发送的字节为 0，则本次响应结束，重置读写缓冲区。
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);      // 集中写

        if (temp < 0) {
            // 如果 TCP 写缓冲没有空间或者被中断，则等待下一轮 EPOLLOUT 事件，虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            else {
                unmap();               
                return false;
            }
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {     // 部分写的情况1，如果集中写的第一个缓冲区已经发送完毕，第二个缓冲区发送了一部分
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else {                                       // 部分写的情况2，集中写的第一个缓冲区发送了一部分数据。
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 如果集中写的数据发送完毕
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);            // 重新向 epoll 注册连接 socket 上的可读事件

            if (m_linger) {                               // 如果设置了保持连接，则重置读写缓冲区等
                init();
                return true;
            }
            else
                return false;
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)            // 如果写缓冲区已满
        return false;

    va_list arg_list;
    va_start(arg_list, format);                     // 确定可变参数的起始地址。
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }

    m_write_idx += len;
    va_end(arg_list);                       // 将指针清零

    LOG_INFO("request:%s", m_write_buf);    // 将要发送的响应报文以日志的形式构造出来

    return true;
}

// 为 HTTP 响应报文添加状态行，status：状态码，title：原因短语
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 为 HTTP 响应报文添加首部字段
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

// 为 HTTP 响应报文添加首部字段 Content-Length
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

// 为 HTTP 响应报文添加首部字段 Content-Type
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 为 HTTP 响应报文添加首部字段 Connection
bool http_conn::add_linger() {
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 为 HTTP 响应报文添加空行
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 为 HTTP 响应报文添加报文主体
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

// 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {          // 基本上不会出现这种情况，因为代码中只有在解析请求报文时，主状态机的状态在三种状态之外才会返回该错误，但是项目中没有将主状态机的状态设置错误的代码
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST: {             // 客户端请求语法错误，如请求行的请求方法不是 get或post，请求行没有版本号
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST: {       // 客户端没有足够的权限访问
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        case FILE_REQUEST: {            // 请求成功，返回目标文件
            add_status_line(200, ok_200_title);             // 添加状态行
            if (m_file_stat.st_size != 0) {                 // 如果请求的目标文件的大小不等于 0
                add_headers(m_file_stat.st_size);                   // 则添加响应报文的首部字段
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else {                                         // 如果请求的目标文件的大小等于 0
                const char *ok_string = "<html><body></body></html>";       // 返回一个空的html
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }

    // 下面是请求失败的情况
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;

    return true;
}

// 由线程池中的工作线程调用，这是处理 HTTP 请求的入口函数
void http_conn::process() {
    // 解析 HTTP 请求
    HTTP_CODE read_ret = process_read();

    // 如果本次请求无效，则重新把连接 socket 上的读事件加入到 epoll 中
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);           // 当请求解析成功后，则把连接 socket 上的写事件加入到 epoll 中
    if (!write_ret)
        close_conn();

    // 向 epoll 实例注册该套接字上的写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
