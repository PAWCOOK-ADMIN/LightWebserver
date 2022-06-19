#include "double_buffer.h"

using namespace std;

#define BUFFERSIZE 150                         // 单个缓冲区缓冲日志的最大数量

double_buffer::double_buffer() {
    cur = new Buffer;
    next = new Buffer;
    new1 = new Buffer;
    new2 = new Buffer;

    stop = false;
}

void double_buffer::init() {
    pthread_t pid;
    pthread_create(&pid, nullptr, thread_work, nullptr);            // 创建后端日志写线程
}

void double_buffer::setfp(FILE *p) {
    fp = p;
}

void *double_buffer::thread_work(void *arg) {
    double_buffer::getinstance().d_buffer_write();
    return nullptr;
}

// 前端写入日志
void double_buffer::push(string s) {
    m_mutex.lock();

    if (cur->size() < BUFFERSIZE)           // cur 缓冲区未满
        cur->push_back(s);
    else {                                  // cur 缓冲区已满
        // 将其放入前端缓冲区数组
        front_buffers.push_back(cur);

        // 切换 next 缓冲区
        if (next != nullptr) {
            cur = next;
            next = nullptr;
            cur->clear();
        }
        else 
            cur = new Buffer;
            
        cur->push_back(s);

        // 通知后端写线程开始写日志到磁盘中
        m_cond.signal();
    }

    m_mutex.unlock();
}

// 后端写入磁盘
void double_buffer::d_buffer_write() {
    while (!stop) {
        m_mutex.lock();

        if (front_buffers.size() == 0)             // 如果前端缓冲区数组为空, 则等待 3 秒
            m_cond.timewait(m_mutex.get(), 3);


        // 把当前使用的缓冲区加入前端缓冲区数组
        front_buffers.push_back(cur);
        cur = new1;
        new1 = nullptr;

        //cout << "frontbuffer: " << front_buffers.size() << endl;
        // 交换前后端缓冲区数组
        back_buffers.swap(front_buffers);

        //cout << "frontbuffer: " << front_buffers.size() << endl;
        //cout << "backbuffer: " << back_buffers.size() << endl;
        
        if (!next) {
            next = new2;
            new2 = nullptr;
        }
        m_mutex.unlock();

        // 前端陷入死循环，拼命发送日志消息，超过后端的处理能力，会造成数据在内存中的堆积
		// 严重时引发性能问题(可用内存不足), 或程序崩溃(分配内存失败)
		if (back_buffers.size() > 25) {
			string s = "Error, too much logs\n";
			fputs(s.c_str(), fp);                  
            
            for (int i=2; i<back_buffers.size(); i++)                            // 释放多余日志占用内存
                delete back_buffers[i];
            
            back_buffers.erase(back_buffers.begin()+2, back_buffers.end());      // 丢掉多余日志，只保留2个buffer(默认4M)
        }

        // 开始将后端缓冲区数组中的日志写入文件磁盘
        for (auto buffer : back_buffers) {
            for (string s: *buffer) {
                fputs(s.c_str(), fp);           // 写入磁盘
                //fflush(fp);
            } 

            buffer->clear();            // 清空缓冲区

            m_mutex.lock();

            if (new1 == nullptr)        // 重新填充 new1 和 new2
                new1 = buffer;
            else if (new1 == nullptr) 
                new2 = buffer;
            else 
                delete buffer;

            m_mutex.unlock();
        }

        back_buffers.clear();           // 清空后端日志缓冲数组
    }
}