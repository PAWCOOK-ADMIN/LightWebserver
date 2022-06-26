#include "config.h"

Config::Config(){

    // 端口号, 默认10000
    PORT = 10001;

    // 日志写入方式，默认异步
    LOGWrite = 1;

    // 触发组合模式, 默认 listenfd LT + connfd LT
    TRIGMode = 0;

    // listenfd 触发模式，默认 LT
    LISTENTrigmode = 0;

    // connfd触发模式，默认 LT，水平触发
    CONNTrigmode = 0;

    // 优雅地关闭链接，默认不使用
    OPT_LINGER = 0;

    // 数据库连接池数量, 默认 8
    sql_num = 8;

    // 线程池内的线程数量, 默认 8
    thread_num = 8;

    // 关闭日志,默认不关闭
    close_log = 0;

    // 并发模型,默认是 proactor
    actor_model = 0;
}

// 解析命令行参数
void Config::parse_arg(int argc, char*argv[]){

    int opt;                                            // 命令行参数返回的选项字符
    const char *str = "p:l:m:o:s:t:c:a:";               // 选项字符串

    while ((opt = getopt(argc, argv, str)) != -1) {
        switch (opt) {
            case 'p': {
                PORT = stoi(optarg);
                break;
            }
            case 'l': {
                LOGWrite = stoi(optarg);
                break;
            }
            case 'm': {
                TRIGMode = stoi(optarg);
                break;
            }
            case 'o': {
                OPT_LINGER = stoi(optarg);
                break;
            }
            case 's': {
                sql_num = stoi(optarg);
                break;
            }
            case 't': {
                thread_num = stoi(optarg);
                break;
            }
            case 'c': {
                close_log = stoi(optarg);
                break;
            }
            case 'a': {
                actor_model = stoi(optarg);
                break;
            }

            default:
                break;
        }
    }
}