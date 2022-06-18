#include "config.h"

int main(int argc, char *argv[])
{
    // 数据库的用户名、密码和数据库名
    string user = "pawcook";
    string passwd = "1";
    string databasename = "webserver";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化 webserver 服务器
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, config.OPT_LINGER, config.TRIGMode,  
                config.sql_num,  config.thread_num, config.close_log, config.actor_model);
    
    // 创建日志系统，并初始化
    server.log_write();

    // 创建并初始化数据库连接池
    server.sql_pool();

    // 初始化线程池
    server.thread_pool();

    // 设置套接字的触发模式
    server.trig_mode();

    // 监听
    server.eventListen();

    // 开始运行服务器
    server.eventLoop();

    return 0;
}