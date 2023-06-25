#include"http_conn.h"
#include <cstddef>
#include <cstdio>


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/zs/git_space/NiuKe_websever/resourse";


int http_conn::m_epollfd=-1;
int http_conn::m_user_count=0;

//设置文件描述符非阻塞，ET模式必须用的非阻塞IO，因为一次性读完数据需要不停循环
void sefd_nonblock(int fd)
{
    int old_flag=fcntl(fd, F_GETFL);
    int new_flag=old_flag |O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

//添加文件到epoll中
extern void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    //event.events=EPOLLIN|EPOLLRDHUP;//水平模式
    event.events=EPOLLIN|EPOLLRDHUP|EPOLLET;//边沿模式
    if(one_shot) 
    {
        // 防止同一个通信文件描述符被不同的线程处理
        event.events |= EPOLLONESHOT;//同一时刻只能一个线程处理
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    sefd_nonblock(fd);
    
}
//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd,NULL);
    close(fd);
}
// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
extern void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLRDHUP|EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);

}

//初始化新接受的连接
void http_conn::init(int sockfd,struct sockaddr_in& addr_in)
{
    m_sockfd=sockfd;
    m_address=addr_in;

    //添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    //用户数加一
    m_user_count++;
    printf("接受新连接,user_count=%d\n",m_user_count);

    init();
}
//初始化其他信息(上面init会改变m_user_count，有时候重新初始化并不想改变)
void http_conn::init()
{
    m_read_idx=0;
    m_check_state=CHECK_STATE_REQUESTLINE;  // 初始状态为检查请求行;
    m_checked_idx=0;
    m_start_line=0;

    m_method=GET;
    m_url=0;
    m_version=0;
    m_host=NULL;
    m_linger=false;
    m_content_length=0;

    m_write_idx = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_iv_count=0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

}
//关闭连接
void http_conn::close_conn()
{
    if(m_sockfd!=-1)
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--; 
    }
}

//非阻塞读
bool http_conn::read()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
        return false;
    int bytes_read=0;
    while(true)
    {
        bytes_read=recv(m_sockfd, m_read_buf+m_read_idx, 2048,0);//注意是否阻塞
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                //没有数据
                break;
            }
            //出错
            return false;
        }
        else if(bytes_read==0)
        {
            //对方关闭连接
            return false;
        }
        else 
        {
            m_read_idx+=bytes_read;
        }
    }
    printf("读取到了数据：\n%s\n",m_read_buf);
    return true;
}

//处理客户请求,由线程池中工作线程调用，处理http请求
void http_conn::process()
{
    //解析http请求
    printf("解析请求\n");
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN); //修改文件描述符，重置socket上的EPOLLONESHOT事件 
        return;//退出处理
    }
    //生成响应
    printf("生成响应\n");
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);

}

//解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line()//解析行
{
    char tmp;
    for(;m_checked_idx<m_read_idx;m_checked_idx++)
    {
        tmp=m_read_buf[m_checked_idx];
        //如果当前是\r字符，则有可能会读取到完整行
        if(tmp=='\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if((m_checked_idx+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1]=='\n')
            {
                //'\r'置为'\0'
                m_read_buf[m_checked_idx]='\0';
                m_checked_idx++;
                //'\n'置为'\0'
                m_read_buf[m_checked_idx]='\0';
                m_checked_idx++;
                return LINE_OK;
            }
            return LINE_BAD;
        }
        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if(tmp=='\n')//??????????????
        {
            //前一个字符是\r，则接收完整
            if(m_checked_idx>1&&m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx]='\0';
                m_checked_idx++;
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

//主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;

    char* text=NULL;

    while(
        (m_check_state==CHECK_STATE_CONTENT&&line_status==LINE_OK)
        ||
        (line_status=parse_line())==LINE_OK
        )//解析到了一行完整数据
    {
        //获取一行数据
        text=get_line();
        m_start_line=m_checked_idx;
        printf("got 1 http line: %s\n",text);

        switch ( m_check_state ) 
        {
            case CHECK_STATE_REQUESTLINE: 
            {
                ret = parse_request_line(text);
                if ( ret == BAD_REQUEST ) 
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: 
            {
                ret = parse_header( text );
                if ( ret == BAD_REQUEST ) 
                {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) 
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: 
            {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) 
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: 
            {
                return INTERNAL_ERROR;
            }
        }
    }//while

    return NO_REQUEST;
}


//解析http请求行，获得请求方法，请求url，http协议版本
http_conn::HTTP_CODE http_conn::parse_request_line(char*text)
{
    m_url=strpbrk(text," \t");//空格或者\t(未找到返回NULL)
    *m_url='\0';
    m_url++;

    char* method=text;
    if(strcasecmp(method, "GET")==0)
    {
        m_method=GET;
    }
    else 
    {
        return BAD_REQUEST;
    }

    m_version=strpbrk(m_url," \t");
    if(!m_version)//未找到
    {
        return BAD_REQUEST;
    }
    *m_version='\0';
    m_version++;
    
    //解析行的时候将/r/n已经变为了'\0';
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }

    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        m_url=strchr(m_url,'/');
    }

    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }

    m_check_state=CHECK_STATE_HEADER;

    return NO_REQUEST;
}

//解析请求头
http_conn::HTTP_CODE http_conn::parse_header(char*text)
{
    // //解析行的时候将/r/n已经变为了'\0';遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char*text)//解析请求体
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/zs/git_space/NiuKe_websever/resourse"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    //"/home/zs/git_space/NiuKe_websever/resourse"+/index(具体本地资源)
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = NULL;
    }
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            //发送响应状态和响应头，响应体
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }
    //请求有问题只发送响应状态和响应头
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}
// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

//非阻塞写
bool http_conn::write()
{
    printf("一次性写完数据\n");
    
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }

    }

}
