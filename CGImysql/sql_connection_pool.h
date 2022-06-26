#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool {
public:
	MYSQL *GetConnection();				 	// 获取数据库连接
	bool ReleaseConnection(MYSQL *conn); 	// 释放连接
	int GetFreeConn();					 	// 获取连接的空闲数
	void DestroyPool();					 	// 销毁所有连接

	// 单例模式返回数据库连接池
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  			// 最大连接数
	int m_CurConn;  			// 当前已使用的连接数
	int m_FreeConn; 			// 当前空闲的连接数
	locker lock;				// 互斥量，同步多个线程访问连接池
	list<MYSQL *> connList; 	// 数据库连接容器（链表）
	sem reserve;				// 信号量，初始化为连接池中的数据库连接个数

public:
	string m_url;			 // 主机地址
	string m_Port;		 	 // 数据库端口号
	string m_User;		     // 登陆数据库用户名
	string m_PassWord;	 	 // 登陆数据库密码
	string m_DatabaseName; 	 // 使用数据库名
	int m_close_log;		 // 日志开关，, 0 表示不关闭（默认）
};

class connectionRAII{
public:
	// 不直接调用获取和释放连接的接口，将其封装起来，通过 RAII 机制进行数据库连接的获取和释放。
	connectionRAII(MYSQL **con, connection_pool *connPool);			// 该类的实例构造的同时，也获取了一个数据库连接
	~connectionRAII();
	
private:
	MYSQL *conRAII;							// RAII 管理数据库连接
	connection_pool *poolRAII;				// 指明管理的数据库连接是从哪个连接池中获取的
};

#endif
