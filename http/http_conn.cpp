#include "http_conn.h"

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy. \n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server. \n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server. \n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file. \n";

//网站根目录
const char* doc_root = "/var/www";

//设置非阻塞fd
static int setnonblocking( int fd ){
    int old_option = fcntl( fd , F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

//将fd加入epoll
void addfd( int epollfd, int fd, bool one_shot ){
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if( one_shot ){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

//移除fd的监听并关闭fd
static void removefd( int epollfd, int fd){
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0);
    close( fd );
}

//更改fd的设置
void modfd( int epollfd, int fd, int ev ){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP/*对端断开连接*/;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event);
}

//在一开始设置两个静态变量为默认值
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

//
void http_conn::close_conn( bool real_close ){
    if( real_close && ( m_sockfd != -1 ) ){
        removefd( m_epollfd, m_sockfd );
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化：将socket加入监听，计数加一
void http_conn::init( int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //下面两行是为了避免TIME_WAIT，仅用于调试，实际使用的时候要关掉
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    addfd( m_epollfd, sockfd, true);
    ++m_user_count;

    init();
}



//简单初始化
void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    doc_root = "/var/www";
    memset( m_read_buf, '\0', READ_BUFFER_SIZE);
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset( m_real_file, '\0', FILENAME_LEN);
}

//从状态机，判断line的完整与否
//每次处理一行（也就是请求行/请求头/消息体中的一种）
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for( ;m_checked_idx < m_read_idx; ++m_checked_idx){
        temp = m_read_buf[ m_checked_idx ];
        if( temp == '\r'){
            if( (m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }else if( m_read_buf[ m_checked_idx + 1] == '\n'){
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if( temp == '\n' ){
            if( m_checked_idx > 1 && m_read_buf[ m_checked_idx - 1] == '\r'){
                //注意m_checked_idx的意义是将要分析的字符位置
                //因此不会将其减少
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读数据直到无数据可读
bool http_conn::read(){
    if( m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }

    int bytes_read = 0;
    while(true){
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if( bytes_read == -1){
            //直到读完
            if( errno == EAGAIN || errno == EWOULDBLOCK ){
                break;
            }
            return false;
        }else if( bytes_read == 0){
            return false;
        }
        //正常情况更新读缓存标志
        m_read_idx += bytes_read;
    }
    return true;
}

//解析http请求行，获得请求方法、url、http版本号
//POST /chapter17/user.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line( char* text ){
    //解析方法
    m_url = strpbrk( text, " \t");//strpbrk返回第一个出现指定字符的位置
    if( !m_url ){
        return BAD_REQUEST;
    }
    //截断后加一方便后面使用
    *m_url++ = '\0';
    char* method = text;
    if( strcasecmp( method, "GET" ) == 0){//strcasecmp比较字符串，相等返回0
        m_method = GET;
    }else if(strcasecmp( method, "POST" ) == 0){
        m_method=POST;
        cgi=1;
    }else{
        return BAD_REQUEST;
    }

    //解析url
    m_url += strspn( m_url, " \t");//strspn返回第一个不是指定字符的下标
    m_version = strpbrk( m_url, " \t");
    if( !m_version ){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';

    //解析版本号
    m_version += strspn( m_version, " \t");
    if( strcasecmp( m_version, "HTTP/1.1") !=0){
        return BAD_REQUEST;
    }

    if( strncasecmp( m_url, "http://", 7) == 0){//strncasecmp比较指定个数字符
        m_url += 7;
        m_url = strchr( m_url, '/');//strchr查找第一个给定字符处
    }
    //增加https情况
    if(strncasecmp(m_url,"https://",8)==0)
    {
        m_url+=8;
        m_url=strchr(m_url,'/');
    }

    if( !m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    //如果url是/转到欢迎界面
    if(strlen(m_url)==1)
        strcat(m_url,"judge.html");

    m_check_state = CHECK_STATE_HEADER;
    //只收到请求行还不够
    return NO_REQUEST;
}

//解析http请求的头部信息
/*请求头
    Host接受请求的服务器地址，ip加端口或者域名
    User-Agent 发送请求的应用程序
    Connection 指定与连接相关的属性，如保持连接
    Connect-Length 消息体长度
*/
http_conn::HTTP_CODE http_conn::parse_headers( char* text ){
    //第一个是空行说明处理完毕，这里面是对最后结果的处理，用于转移状态
    if( text[ 0 ] == '\0' )
    {
        //如果只是HEAD请求就只需要请求行
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }
        //如果消息体有数据，则应将状态转到CHECK_STATE_CONTENT继续进行消息体的处理
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        return GET_REQUEST;
    }
    //处理头部字段Connection
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );//定位到keep-alive的位置
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;//保持连接
        }
    }
    //处理头部字段Connect-Length
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );//字符串转换为longint
    }
    //处理头部字段Host
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    //其他情况
    else
    {
        printf( "oop! unknow header %s\n", text );
    }

    return NO_REQUEST;
}

//处理消息体，这里我们只检查长度是否合法
http_conn::HTTP_CODE http_conn::parse_content( char* text ){
    //读进来的总长度大于已经分析的长度加内容的长度就是合法的
    //因为此时请求头的已经被分析完了，属于checkedidx之前的内容了
    if( m_read_idx >= (m_checked_idx + m_content_length) ){
        text[m_content_length] = '\0';
        //post请求中最后输入的是userpass
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机，用于解析http请求

http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    //要么本行有效且消息体有数据
    //否则就处理新的一行且状态正常
    //这样就是循环处理报文的三行，对每一行有不同的操作
    while( ( (m_check_state ==CHECK_STATE_CONTENT) && (line_status == LINE_OK) ) ||
                ((line_status = parse_line()) == LINE_OK)){
        //得到将要处理的text，也就是startline和checkedidx之间的内容
        //每次按parse_line()，get_line()的顺序调用，idx依次移动
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text);
        
        //注意checkstate一开始的状态是CHECK_STATE_REQUESTLINE
        //也就是会从请求行开始的状态机
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:{
                ret = parse_request_line( text );
                if( ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers( text );
                if( ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret == GET_REQUEST){
                    return do_request();//有可能只有请求头就结束了HEAD
                }
                break;
            }
            case CHECK_STATE_CONTENT:{
                ret = parse_content( text );
                if( ret == GET_REQUEST){
                    return do_request();
                }
                line_status = LINE_OPEN;//todo
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

//如果请求的文件是有效的，就使用mmap映射到m_file_address中（记得munmap）
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy( m_real_file, doc_root);//复制字符串，源必须有‘\0’，目的必须够大且无重叠
    int len = strlen( doc_root );
    //strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //找到m_url中/的位置
    const char *p = strrchr(m_url, '/');

    //cgi登录注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')){
        //根据标志判断是登录检测还是注册检测
        //同步线程登录校验
        //CGI多进程登录校验
    }
    //如果请求资源是/0，跳到注册界面
    if (*(p + 1) == '0'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //拼接
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if(*(p + 1) == '1'){
        //如果是/1跳转到登录界面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //拼接
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else{
        //如果都不是就拼接原本的内容
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    //获取文件状态信息到m_file_stat
    if( stat( m_real_file, &m_file_stat ) < 0){
        return NO_RESOURCE;
    }

    //如果文件的权限是other用户可以读才可以，否则就显示禁止访问
    if( !(m_file_stat.st_mode & S_IROTH ) ){
        return FORBIDDEN_REQUEST;
    }

    //如果是路径说明访问错误
    if( S_ISDIR( m_file_stat.st_mode )){
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY );
    //映射内容和文件内容一起更新，就使用shared，private则是不影响原文件
    //在只读情况下两个都一样
    m_file_address = (char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close( fd );
    return FILE_REQUEST;
}

void http_conn::unmap(){
    if( m_file_address){
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

//写http相应(返回值false就会导致关闭连接)
bool http_conn::write(){
    //发送结果
    int temp = 0;
    
    int newadd = 0;
    //如果没有要法发的就进入下次监听
    if( bytes_to_send == 0){
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1){
        //把响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if(temp > 0){
            bytes_have_send += temp;
            newadd = bytes_have_send - m_write_idx;
        }
        if( temp <= -1){
            //eagain说明写缓冲满了
            if( errno == EAGAIN ){
                //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if (bytes_have_send >= m_iv[0].iov_len){
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
                }else{
                    m_iv[0].iov_base = m_write_buf + bytes_to_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                //等下次epollout事件再写，在此期间无法接到其他请求，但可以保持连接的完整性
                modfd( m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        //bytes_have_send += temp;
        //因为是先进行写操作再进行两个变量的更新的
        //因此to小于等于have就说明刚刚的操作已经都写完了
        if( bytes_to_send <= 0){
            unmap();
            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd,m_sockfd,EPOLLIN);
            //发送成功，根据是否保持连接来确定是否关闭
            if( m_linger ){
                init();
                return true;
            }else{
                return false;
            }
        }
    }
}
//可变参数函数，用于将数据格式化输出到buffer中
//用法和printf相似
bool http_conn::add_response( const char* format, ... ){
    if( m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    //将可变参数按format写入数组中
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= (WRITE_BUFFER_SIZE -1 - m_write_idx) ){
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

//添加响应行
bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

//添加响应头，分三部分：响应体长度/保持连接/空行
bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length( int content_len )
{
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

//添加响应体
bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::process_write( HTTP_CODE ret ){
    switch( ret ){
        case INTERNAL_ERROR:{
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if( !add_content( error_500_form ) ){
                return false;
            }
            break;
        }
        case BAD_REQUEST:{
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if( !add_content( error_400_form ) ){
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if( !add_content( error_404_form ) ){
                return false;
            }
            break; 
        }
        case FORBIDDEN_REQUEST:{
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if( !add_content( error_403_form ) ){
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200, ok_200_title);
            if( m_file_stat.st_size != 0 ){
                add_headers( m_file_stat.st_size );
                //响应头部分，因为所有的add_函数都是写道m_write_buff中的
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //响应体：之前映射的文件，通过内存地址访问
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                //提前结束函数
                return true;
            }
            else{
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if( !add_content( ok_string ) ){
                    return false;
                }
            }
        }
        default:{
            return false;
        }
    }

    //如果不是文件请求，就只用返回m_write_buf
    //如果是文件请求，前面就已经返回了
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//整个连接类的入口
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        //这个函数本来是有参数的，但有初值
        close_conn();
    }

    modfd( m_epollfd, m_sockfd, EPOLLOUT );
    
}


            