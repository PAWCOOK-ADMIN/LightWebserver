#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/tw_timer.h"
#include "../log/log.h"

class http_conn {
public:
    static const int FILENAME_LEN = 200;            // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;       // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 2048;      // 写缓冲区大小

    // HTTP 请求方法，支持 GET 和 POST
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH};

    // 解析客户端请求时，主状态机的状态
    // - CHECK_STATE_REQUESTLINE：当前正在分析请求行
    // - CHECK_STATE_HEADER：当前正在分析首部字段
    // - CHECK_STATE_CONTENT：当前正在解析报文主体
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};

    // 服务器处理 HTTP 请求的可能结果，报文解析的结果
    // - NO_REQUEST：请求不完整，需要继续读取客户数据
    // - GET_REQUEST：表示获得了一个完整的客户请求
    // - BAD_REQUEST：表示客户请求语法错误
    // - NO_RESOURCE：表示服务器没有资源
    // - FORBIDDEN_REQUEST：表示客户对资源没有足够的访问权限
    // - FILE_REQUEST：文件请求，获取文件成功
    // - INTERNAL_ERROR：表示服务器内部错误
    // - CLOSED_CONNECTION：表示客户端已经关闭连接了
    enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};

    // 从状态机的三种可能状态，代表行的读取状态
    // - LINE_OK  ：读取到一个完整的行
    // - LINE_BAD ：行出错
    // - LINE_OPEN：行数据不完整
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);   // 初始化服务器新接受的连接
    void close_conn(bool real_close = true);        // 关闭连接
    void process();                                 // 任务的处理逻辑。这里是处理客户端请求，解析 http 请求报文并做出响应
    bool read_once();
    bool write();                                   // 非阻塞写

    // 返回 TCP 连接的客户端 socket 地址
    sockaddr_in *get_address(){
        return &m_address;
    }

    void initmysql_result(connection_pool *connPool);           // 读取数据库中的所有用户名和密码至 m_users（只在初始化时执行一次）

    // 以下两个变量用于标识在 reactor 模式下子线程的 I/O 是否出错
    int timer_flag;                         // 用来判定子线程读写任务是否成功。
    int improv;                             // 用来保持主线程和子线程的同步，子线程读数据完毕后被置为 1  


private:
    void init();                            // 初始化连接其余的信息
    HTTP_CODE process_read();               // 解析 http 请求
    bool process_write(HTTP_CODE ret);      // 填充 HTTP 应答

    // 下面这一组函数被 process_read 调用以分析 HTTP 请求报文
    HTTP_CODE parse_request_line(char *text);       // 解析 http 请求首行
    HTTP_CODE parse_headers(char *text);            // 解析 http 请求头
    HTTP_CODE parse_content(char *text);            // 解析 http 请求体
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();               // 解析具体的一行

    // 这一组函数被 process_write 调用以填充 HTTP 应答
    void unmap();                                           // 对内存映射区执行 munmap 操作
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;           // epoll 实例。所有的 http_conn 共用一个 epoll 实例
    static int m_user_count;        // 统计 TCP 连接数量
    MYSQL *mysql;                   // 数据库连接句柄
    int m_state;                    // 表明连接池中的线程在 reactor 模式下，该连接套接字的需要进行的是读还是写，读为 0, 写为 1

private:
    int m_sockfd;               // 连接的客户端 socket 句柄
    sockaddr_in m_address;      // 连接的客户端 socket 地址

    char m_read_buf[READ_BUFFER_SIZE];         // 读缓冲
    int m_read_idx;             // 游标，指明读缓冲的第一个空闲的下标
    int m_checked_idx;          // 当前正在分析的字符在读缓冲区的位置
    int m_start_line;           // 当前正在解析的行的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];      // 写缓冲区
    int m_write_idx;                            // 写缓冲区中待发送的字节数
    int bytes_to_send;              // 写缓冲区中还需要发送的字节数
    int bytes_have_send;            // 写缓冲区中已经发送的字节数

    CHECK_STATE m_check_state;          // 主状态机当前所处的状态
    METHOD m_method;                    // 请求方法

    char m_real_file[FILENAME_LEN];         // 请求处理后，应该访问的文件名

    char *m_url;                            // 客户请求的目标文件的文件名
    char *m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    char *m_host;                           // 主机名
    int m_content_length;                   // HTTP 请求报文的报文主体的长度
    bool m_linger;                          // HTTP 请求是否要求保持连接

    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 保存请求的文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 将多个缓冲区的数据集中写到写缓冲区中，第 0 个参数保存响应报文的报文首部，第 1 个参数保存响应报文的报文主体（如果有的话，内存映射）
    int m_iv_count;                         // 表示集中写中的缓冲区的个数

    int cgi;                // 是否启用的 POST
    char *m_string;         // 存储请求报文主体中的数据

    char *doc_root;         // 网站根目录

    map<string, string> m_users;            // 用户名和密码(貌似没有被用到)
    int m_TRIGMode;                         // 连接套接字的触发模式，默认 LT
    int m_close_log;                        // 是否启用日志，, 0 表示不关闭（默认）

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
