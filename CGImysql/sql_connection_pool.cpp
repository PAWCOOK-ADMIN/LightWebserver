#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool() {
	m_CurConn = 0;				// 当前已使用的连接数
	m_FreeConn = 0;				// 当前空闲的连接数
}

// 单例模式返回数据库连接池
connection_pool *connection_pool::GetInstance()	{
	static connection_pool connPool;		// 创建一个数据库连接池，局部静态变量实现的单例模式
	return &connPool;
}

// 数据库连接池的构造初始化，MaxConn 为连接池的最大连接数，默认为 8
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log) {
	// 初始化数据库信息
	m_url = url;						// localhost
	m_Port = Port;						// mysql 服务器默认使用 3306 端口号
	m_User = User;						// pawcook
	m_PassWord = PassWord;				// 1
	m_DatabaseName = DBName;			// webserver
	m_close_log = close_log;

	//创建 MaxConn 个数据库连接
	for (int i = 0; i < MaxConn; i++) {
		MYSQL *con = nullptr;
		con = mysql_init(con);					// 初始化数据库连接句柄

		if (con == nullptr) {
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, nullptr, 0);		// 连接数据库

		if (con == nullptr) {
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		//更新连接池，空闲连接数量增 1
		connList.push_back(con);
		++m_FreeConn;
	}

	// 将信号量初始化为最大连接次数
	reserve = sem(m_FreeConn);

	// 初始化连接池中的最大连接数
	m_MaxConn = m_FreeConn;
}


// 从数据库连接池的连接链表中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection() {
	MYSQL *con = nullptr;

	if (0 == connList.size())
		return nullptr;

	reserve.wait();				// 等待信号量，即等待连接池有空闲连接
	
	lock.lock();				// 互斥访问连接池。因为被唤醒的线程可能不止一个

	con = connList.front();
	connList.pop_front();

	// 这里的两个变量，并没有用到，非常鸡肋...
	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

// 释放当前使用的连接（将 RAII 管理的连接重新加入连接链表中）
bool connection_pool::ReleaseConnection(MYSQL *con) {
	if (nullptr == con)
		return false;

	lock.lock();				// 互斥访问连接池。因为处于运行态的线程可能不止一个	

	connList.push_back(con);	// 将连接加入链表

	// 更新空闲连接数和已使用连接数
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();				// 信号量增 1
	return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool() {

	lock.lock();
	if (connList.size() > 0) {
		// 先遍历链表，关闭每个连接
		for (MYSQL *conn : connList)
			mysql_close(conn);

		// 再清空链表
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();		
	}

	lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn() {
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

// RAII 管理连接池的构造函数
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();				// 从数据库连接池中返回一个可用连接
	
	conRAII = *SQL;
	poolRAII = connPool;
}

// RAII 机制销毁连接池
connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}