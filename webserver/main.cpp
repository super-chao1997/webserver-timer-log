#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"
#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;
// 添加文件描述符到epoll中
extern void addfd( int epollfd, int fd, bool one_shot );
//从epool中删除文件描述符
extern void removefd( int epollfd, int fd );
extern int setnonblocking( int fd );
//添加信号
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( client_data* user_data )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    printf( "close fd %d\n", user_data->sockfd );
}
void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}
//信号处理函数，把信号发送到管道
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}
void addfdtimer( int epollfd, int fd )
{
    epoll_event event; 
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}
void addsigtimer( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));//basename()???????????
        return 1;
    }
    int port = atoi( argv[1] );//获取端口号
    addsig( SIGPIPE, SIG_IGN );//对SIGPIPE信号进行处理
    threadpool< http_conn >* pool = NULL;//创建线程池，初始化
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
        //exit(-1);
    }
    //创建一个数组用于保存所有的连接客户端信息
    http_conn* users = new http_conn[ MAX_FD ];
    //创建监听的套接字，IPV4，TCP
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    // 端口复用,避免2MSL等待
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    //绑定监听
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );
    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    // 监听描述符 添加到 epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;
    // 创建管道，传输信号量
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );//管道写端非阻塞
    addfdtimer( epollfd, pipefd[0] );//监听读端
    // 设置信号处理函数
    addsigtimer( SIGALRM );
    addsigtimer( SIGTERM );
    bool stop_server = false;
    //创建定时器事件用户数组
    client_data* userstimer = new client_data[MAX_FD]; 
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号
    
    while(!stop_server) {//检测事件        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }
        //有事件发生，确定事件的属性，并进行回应
        for ( int i = 0; i < number; i++ ) {            
            int sockfd = events[i].data.fd;           
            if( sockfd == listenfd ) {//有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 ) {//异常处理
                    printf( "errno is: %d\n", errno );
                    continue;
                } 
                if( http_conn::m_user_count >= MAX_FD ) {//连接数已满
                    //目前的连接数已满，（可以给客户端回信息：服务器正忙）
                    close(connfd);
                    continue;
                }
                //将新的客户数据初始化，放到数组中
                users[connfd].init( connfd, client_address);
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                addfd( epollfd, connfd,false );
                userstimer[connfd].address = client_address;
                userstimer[connfd].sockfd = connfd;
                util_timer* timer = new util_timer;
                
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;//定时时间，15秒
                userstimer[connfd].timer = timer;
                timer->user_data = &userstimer[connfd];
                timer_lst.add_timer( timer );//添加此次连接的定时器进链表
            } 
            //对方异常断开或者错误等事件，关闭连接
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                users[sockfd].close_conn();
            } 
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );//读管道数据，一个信号一个字节存储在signals中。
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {//判定何种定时器事件，进行处理
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if(events[i].events & EPOLLIN) {//读事件
                if(users[sockfd].read()) {//一次性读出所有数据
                    pool->append(users + sockfd);//读取完数据，添加到线程池，进行线程处理，回应请求等
                } else {//读取失败
                    users[sockfd].close_conn();
                }
                //超时时间内有数据访问，需要更改定时器
                memset( userstimer[sockfd].buf, '\0', BUFFER_SIZE );
                ret = recv( sockfd, userstimer[sockfd].buf, BUFFER_SIZE-1, 0 );
                printf( "get %d bytes of client data %s from %d\n", ret, userstimer[sockfd].buf, sockfd );
                util_timer* timer = userstimer[sockfd].timer;
                if( ret < 0 )
                {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if( errno != EAGAIN )
                    {
                        cb_func( &userstimer[sockfd] );
                        if( timer )
                        {
                            timer_lst.del_timer( timer );
                        }
                    }
                }
                else if( ret == 0 )
                {
                    // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器。
                    cb_func( &userstimer[sockfd] );
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
                else// 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                {
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );//调整重新定时
                    }
                }
            } 
            else if( events[i].events & EPOLLOUT ) {//写事件
                if( !users[sockfd].write() ) {//一次性写完所有数据
                    users[sockfd].close_conn();//写失败关闭连接
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }
    close( epollfd );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] userstimer;
    delete [] users;
    delete pool;
    return 0;
}