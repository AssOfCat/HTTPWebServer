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
#include "locker.h"
//m_read_buf-------m_start_line------m_checked_idx------m_read_idx

class http_conn
{
public:
    //文件名最大长度
    static const int FILENAME_LEN = 200;
    //读缓冲区大小
    static const int READ_BUFFER_SIZE = 2048;
    //写缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //http请求方法
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };
    //解析客户请求的时候，主机所处状态
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    //处理http请求的可能结果
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    //行的读取状态
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn(){}
    ~http_conn(){}

public:
    //初始化新接受的连接
    void init( int sockfd, const sockaddr_in& addr );
    //关闭连接
    void close_conn( bool real_close = true );
    //处理客户请求
    void process();
    //非阻塞读
    bool read();
    //非阻塞写
    bool write();

private:
    //初始化连接
    void init();
    //解析http请求
    HTTP_CODE process_read();
    //填充http应答
    bool process_write( HTTP_CODE ret );

    //下面的函数被process_read调用以分析http请求
    HTTP_CODE parse_request_line( char* text );
    HTTP_CODE parse_headers( char* text );
    HTTP_CODE parse_content( char* text );
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();

    //下面的函数被process_write调用填充http应答
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

public:
    //所有socket事件都被注册到一个epoll中，所以设置static
    static int m_epollfd;
    //统计用户数量也是static
    static int m_user_count;

private:
    //负责连接对方的socket
    int m_sockfd;
    //对方的addr
    sockaddr_in m_address;

    //读缓冲区
    char m_read_buf[ READ_BUFFER_SIZE ];
    //读缓冲区已经读入的客户数据的最后一个字节的下一个字节
    int m_read_idx;
    //当前正在分析的字符在读缓冲区中的位置(也就是还未分析)
    int m_checked_idx;
    //正在解析的行的起始位置
    int m_start_line;
    //写缓冲区
    char m_write_buf[ WRITE_BUFFER_SIZE ];
    //写缓冲区待发送字节数，也就是要发送的最后一个后一个字节位置
    int m_write_idx;

    //主状态机所处的状态
    CHECK_STATE m_check_state;
    //请求方法，见头文件定义
    METHOD m_method;

    //客户请求的目标文件完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
    char m_real_file[ FILENAME_LEN ];
    //客户请求的目标文件文件名
    char* m_url;
    //http协议版本号
    char* m_version;
    //主机名
    char* m_host;
    //http请求消息体的长度
    int m_content_length;
    //http请求是否要保持连接
    bool m_linger;

    //客户请求的目标文件被mmap到内存中的起始位置
    char* m_file_address;
    //目标文件的状态，通过stat可以获得文件是否存在、是否为目录、是否可读，获取文件大小
    struct stat m_file_stat;
    //使用writev()执行写操作，也就是散布写，第一行是内存块，第二行是块数量
    struct iovec m_iv[2];
    int m_iv_count;
};

#endif
