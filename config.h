#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"

using namespace std;

// 配置类
class Config {
public:
    Config();
    ~Config(){};

    // 解析命令行参数
    void parse_arg(int argc, char*argv[]);

    // 端口号
    int PORT;

    // 日志写入方式（同步和异步）
    int LOGWrite;

    // 触发组合模式
    int TRIGMode;

    // listenfd 触发模式
    int LISTENTrigmode;

    // connfd 触发模式
    int CONNTrigmode;

    // 优雅关闭链接
    int OPT_LINGER;

    // 数据库连接池数量
    int sql_num;

    // 线程池内的线程数量
    int thread_num;

    // 是否关闭日志
    int close_log;

    // 事件处理模型选择, 默认是 proactor
    int actor_model;
};

#endif