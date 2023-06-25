#include <asm-generic/errno-base.h>
#include <csignal>
#include <cstddef>
#include<cstdio>
#include <cstdlib>
#include <stdio.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"lock.h"
#include"threadpool.h"
#include<iostream>
#include<cstring>
#include<signal.h>
#include <unistd.h>
#include"http_conn.h"

#define MAX_FD 10000//最大文件描述符个数
#define MAX_EVENT_NUMBER 10000//最大监听事件数量

//添加信号捕捉
void addsig(int sig,void(*hander)(int))
{
    struct sigaction sa;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler=hander;//捕获信号sig后执行的回调函数
    sigfillset(&sa.sa_mask); //所有信号都阻塞
    sigaction(sig,&sa,NULL);//捕获信号sig
}

//添加文件到epoll中
extern void addfd(int epollfd,int fd,bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);
//从epoll中修改文件描述符
extern void modfd(int epollfd,int fd,int ev);

int main(int argc,char*argv[])
{
    
    if(argc<=1)
    {
        std::cout<<"按照如下格式运行: "<<argv[0]<<" port_number"<<std::endl;
        exit(-1);
    }

    //获取端口
    int port=std::stoi(argv[1]);

    //对SIGPIPE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池，初始化
    ThreadPool<http_conn>*pool=NULL;
    try{
        pool=new ThreadPool<http_conn>;
    }catch(...){
        exit(-1);
    }

    //创建一个数组用于保存客户端信息
    http_conn* users=new http_conn[MAX_FD];



    // 1. 创建监听的套接字
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) // 创建套接字是否成功
    {
        perror("socket error\n");
        return -1;
    }
    printf("监听文件描述符listenfd=%d\n",listenfd);
    //端口复用
    int reuse = 1,ret=0;
    ret=setsockopt(listenfd, SOL_SOCKET,SO_REUSEADDR,  &reuse, sizeof(reuse));


    struct sockaddr_in severaddr; // 监听的套接字地址信息
    severaddr.sin_family = AF_INET;
    severaddr.sin_port = htons(port);//port 设置的端口
    //severaddr.sin_addr.s_addr=INADDR_ANY;
    severaddr.sin_addr.s_addr = inet_addr("192.168.160.131");
    ret=bind(listenfd,(struct sockaddr*)&severaddr,sizeof(severaddr));
    
    if(ret==-1)
    {
        perror("bind error\n");
        return -1;
    }
    
    ret=listen(listenfd,10);//10表示最大同时连接数
    
    if(ret==-1)
    {
        perror("listen error\n");
        return -1;
    }


    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(1);
    printf("创建的epollfd=%d\n",epollfd);
    //添加监听文件描述符到epoll对象
    addfd(epollfd, listenfd, false);//监听描述符listenfd不能注册EPOLLONESHOT，否则只能处理一个客户连接

    http_conn::m_epollfd=epollfd;

    while(true)
    {
        //epoll监测是否有绪的文件描述符
        int num=epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);//函数一直阻塞，直到 epoll 实例中有已就绪的文件描述符之后才解除阻塞
        if((num<0)&&(errno!=EINTR))
        {
            perror("epoll failed\n");
            break;
        }
        printf("epoll 就绪队列中事件个数=%d\n",num);
        for(int i=0;i<num;i++)//遍历就绪文件描述符
        {
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd)
            {
                //有客户端连接
                printf("有客户端连接\n");
                struct sockaddr_in cliaddr;
                socklen_t cliaddrlen=sizeof(cliaddr);
                int connfd=accept(listenfd, (struct sockaddr*)&cliaddr, &cliaddrlen);
                printf("接受连接connd=%d\n",connfd);
                //定义最多连MAX_FD个（跟系统能分配的文件描述符个数有关）
                if(http_conn::m_user_count>MAX_FD)
                {
                    //目前连接满了         (close(sockfd既可以重新使用))
                    //给客户端回显消息
                    close(connfd);
                    continue;
                }
                
                //初始化创建的http_conn对象数组保存连接信息
                users[connfd].init(connfd,cliaddr);
                
            
            }
            else if(events[i].events&(EPOLLHUP|EPOLLRDHUP|EPOLLERR))
            {
                //对方异常断开或者错误事件
                users[sockfd].close_conn();
            }
            else if(events[i].events&EPOLLIN)
            {
                printf("有读事件\n");
                //有读事件
                if(users[sockfd].read())
                {
                    //一次性读完
                    pool->append(&users[sockfd]);//添加任务，任务是通信对象，调用通信对象的process函数
                }
                else 
                {
                users[sockfd].close_conn();
                }
            }
            else if(events[i].events&EPOLLOUT)
            {
                if(!users[sockfd].write())
                {
                    //一次性把数据写完
                    users[sockfd].close_conn();
                }
            }//if(sockfd==listenfd)

        }//for(int i=0;i<num;i++)

    }//while(true)

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}