#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./locker/locker.h"
#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );

//添加信号和回调函数,先把每个信号都屏蔽。
void addsig( int sig, void( handler )(int), bool restart = true){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ));
    sa.sa_handler = handler;
    if( restart ){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL) != -1);
}

//向socket写入错误信息
void show_error( int connfd, const char* info ){
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}

int main( int argc, char* argv[] ){
    if( argc <= 2 ){
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return true;
    }
    const char* ip = argv[1];//获取ip地址
    int port = atoi( argv[2]);//端口转换成数字

    //忽略SIGPIPE信号
    addsig( SIGPIPE, SIG_IGN );

    //创建线程池
    threadpool< http_conn >* pool = NULL;
    try{
        pool = new threadpool< http_conn >;
    }catch( ... ){
        return 1;
    }

    //预先分配http_conn对象给每个用户
    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    int user_count = 0;

    //创建监听socket
    int listenfd = socket( PF_INET, SOCK_STREAM, 0);
    assert( listenfd >= 0 );

    //设定close的时候的行为
    //当onoff不为0 且linger为0, close将立即返回, TCP将丢弃发送缓冲区的残留数据, 同时发送一个复位报文段
    struct linger tmp = {1, 0};
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ));

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;//address family
    inet_pton( AF_INET, ip, &address.sin_addr );//ip转为网络字节序
    address.sin_port = htons( port );//将port转换为网络字节序

    //sockaddr和sockaddr_in大小是一样的，都是16字节
    ret = bind( listenfd, (struct sockaddr* )&address, sizeof( address ));
    assert( ret >= 0 );

    ret = listen( listenfd, 5);
    assert( ret >= 0);

    epoll_event events[ MAX_EVENT_NUMBER ];

    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false);
    //所有连接共用的static成员
    http_conn::m_epollfd = epollfd;

    while(true){
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1);
        if( ( number < 0 ) && ( errno != EINTR ) ){
            printf( "epoll failure ");
            break;
        }

        for( int i = 0; i < number; ++i){
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd){
                //用来接收客户端socket的addr
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                //接收连接socket并填充addr
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                //放入数组中并根据socket/addr初始化
                users[connfd].init( connfd, client_address );
                //这里不用将连接加入epoll，后面也不用在主函数中处理
                //因为加入users数组后根据来到的信息分配给线程池
                //实现半反应堆效果，线程之间竞争任务队列
            }else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR )){
                //对方挂断/socket挂断/错误都会导致关闭连接
                users[sockfd].close_conn();
            }else if( events[i].events & EPOLLIN ){
                if( users[sockfd].read()){
                    //如果读取数据成功，就将此http连接加入pool
                    pool -> append( users + sockfd );
                }else{
                    users[sockfd].close_conn();
                }
            }else if( events[i].events & EPOLLOUT){
                if( !users[sockfd].write() ){
                    users[sockfd].close_conn();
                }
            }else{

            }
        }
    }
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}