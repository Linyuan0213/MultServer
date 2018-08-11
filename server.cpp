//
// Created by robin on 18-8-10.
//

#include "server.h"
#include "threadpool.h"

CoreServer::CoreServer(int count)
{
    //初始化
    m_thread_num = count;
    m_port = -1;
    m_main_base = new LibeventThread;
    m_threads = new LibeventThread[m_thread_num];
    m_main_base->tid = pthread_self();
    m_main_base->base = event_base_new();

    pool = new ThreadPool(m_thread_num);
    memset(m_signal_events, 0, sizeof(m_signal_events));

    //初始化线程结构体
    for(int i=0; i<m_thread_num; i++)
    { 
        init_thread(&m_threads[i]);
    }

}

CoreServer::~CoreServer()
{
    //停止事件循环
    stop_run(nullptr);


    event_base_free(m_main_base->base);
    for(int i=0; i<m_thread_num; i++)
        event_base_free(m_threads[i].base);

    delete m_main_base;
    delete [] m_threads;
    delete pool;
}

void CoreServer::set_port(int port)
{
    m_port = port;
}

void CoreServer::init_thread(LibeventThread *me)
{
    int res;

    //建立libevent事件处理机制
    me->p_tcp_conn = this;
    me->base = event_base_new();
    assert(me->base != nullptr);

    //在主线程和子线程之间建立管道
    int fds[2];
    res = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if (res == -1)
    {
        perror("socketpair");
        exit(1);
    } 

    me->notify_receive_fd = fds[0];
    me->notify_send_fd    = fds[1];

    //让子线程event base监听管道
    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_process, me);
    event_base_set(me->base, &me->notify_event);
    res = event_add(&me->notify_event, nullptr);
    assert( res == 0 );
}

void *CoreServer::worker_thread(void *arg)
{
    //开启libevent的事件循环，准备处理业务
    auto *me = static_cast<LibeventThread *>(arg);
    me->tid = pthread_self();
    std::cout << "thread " << me->tid << " start..." << std::endl;
    event_base_dispatch(me->base);
    std::cout << "subthread done" << std::endl;

    return nullptr;
}

void CoreServer::start_run()
{
    evconnlistener *listener;

    //如果端口号不是EXIT_CODE，就监听该端口号
    if(EXIT_CODE != m_port)
    {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(static_cast<uint16_t>(m_port));
        std::cout << "start listen..." << std::endl;
        listener = evconnlistener_new_bind(m_main_base->base,
                                           on_listen, (void *) this,
                                           LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
                                           (sockaddr *) &sin, sizeof(sockaddr_in));
        if(nullptr == listener )
        { 
            std::cerr << "listen error " << strerror(errno) << std::endl;
            exit(1);
        }
    } 

    //开启各个子线程
    for(int i=0; i < m_thread_num; i++)
    {
           pool->enqueue(worker_thread, &m_threads[i]);

    } 

    //开启主线程的事件循环
    event_base_dispatch(m_main_base->base);

    //事件循环结果，释放监听者的内存
    if( m_port != EXIT_CODE )
    {
        std::cout << "release listen" << std::endl;
        evconnlistener_free(listener);
    } 


}

void CoreServer::stop_run(timeval *tv)
{
    int contant = EXIT_CODE;
    //向各个子线程的管理中写入EXIT_CODE，通知它们退出
    for(int i = 0; i < m_thread_num; i++)
    { 
        write(m_threads[i].notify_send_fd, &contant, sizeof(int));
    }
    //结果主线程的事件循环
    event_base_loopexit(m_main_base->base, tv);
}

void CoreServer::on_listen(struct evconnlistener *listener,
                           evutil_socket_t fd,
                           struct sockaddr *sa,
                           int socklen,
                           void *user_data)
{
    auto *server = static_cast<CoreServer*>(user_data);

    //随机选择一个子线程，通过管道向其传递socket描述符
    int num = rand() % server->m_thread_num;
    int sendfd = server->m_threads[num].notify_send_fd;
    write(sendfd, &fd, sizeof(evutil_socket_t));
}

void CoreServer::thread_process(int fd, short which, void *arg)
{
    auto *me = static_cast<LibeventThread*>(arg);

    //从管道中读取数据（socket的描述符或操作码）
    int pipefd = me->notify_receive_fd;
    evutil_socket_t confd;
    read(pipefd, &confd, sizeof(evutil_socket_t));
    //如果操作码是EXIT_CODE，则终于事件循环
    if( EXIT_CODE == confd )
    { 
        event_base_loopbreak(me->base);
        return;
    }

    //新建连接
    struct bufferevent *bev;
    bev = bufferevent_socket_new(me->base, confd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev)
    { 
        std::cerr << "Error constructing bufferevent!" << std::endl;
        event_base_loopbreak(me->base);
        return;
    }

    std::cout << "confd " << confd << std::endl;
    //将该链接放入队列
    auto *tmp = new Conn(confd);
    tmp->m_thread = me;

    me->conn.push_front(tmp);
    //准备从socket中读写数据
    std::cout << "conn size " << me->conn.size() << std::endl;
    std::cout << "conn first " << me->conn.front()->m_fd << std::endl;
    bufferevent_setcb(bev, on_read, on_write, on_close, me->conn.front());
    bufferevent_enable(bev, EV_WRITE);
    bufferevent_enable(bev, EV_READ);

    //调用用户自定义的连接事件处理函数
    me->p_tcp_conn->connect_cb(me->conn.front());
}

void CoreServer::on_read(struct bufferevent *bev, void *data)
{
    auto *conn = static_cast<Conn*>(data);
    conn->m_readbuf = bufferevent_get_input(bev);
    conn->m_writebuf = bufferevent_get_output(bev);

    //调用用户自定义的读取事件处理函数
    conn->m_thread->p_tcp_conn->read_cb(conn);
}

void CoreServer::on_write(struct bufferevent *bev, void *data)
{
    auto *conn = static_cast<Conn*>(data);
    conn->m_readbuf = bufferevent_get_input(bev);
    conn->m_writebuf = bufferevent_get_output(bev);

    //调用用户自定义的写入事件处理函数
    conn->m_thread->p_tcp_conn->write_cb(conn);
}

void CoreServer::on_close(struct bufferevent *bev, short events, void *data)
{
    auto *conn = static_cast<Conn*>(data);
    //调用用户自定义的断开事件处理函数
    conn->m_thread->p_tcp_conn->close_cb(conn, events);
    std::cout << conn->m_thread->conn.front()->m_fd << std::endl;
    delete conn->m_thread->conn.front();
    conn->m_thread->conn.pop_front();
    bufferevent_free(bev);
}

bool CoreServer::add_signal_event(int sig, void (*ptr)(int, short, void *))
{
    if( sig >= MAX_SIGNAL )
        return false;

    //新建一个信号事件
    event *ev = evsignal_new(m_main_base->base, sig, ptr, (void*)this);
    if ( !ev ||
         event_add(ev, nullptr) < 0 )
    { 
        event_del(ev);
        return false;
    }

    //删除旧的信号事件
    if(nullptr != m_signal_events[sig] )
        delete_signal_event(sig);
    m_signal_events[sig] = ev;

    return true;
}

bool CoreServer::delete_signal_event(int sig)
{
    event *ev = m_signal_events[sig];
    if( sig >= MAX_SIGNAL || nullptr == ev )
        return false;

    event_del(ev);
    ev = nullptr;
    return true;
}

event *CoreServer::add_timer_event(void (*ptr)(int, short, void *),
                                   timeval tv, bool once)
{
    int flag = 0;
    if( !once )
        flag = EV_PERSIST;

    //新建定时器信号事件
    auto *ev = new event;
    event_assign(ev, m_main_base->base, -1, flag, ptr, (void*)this);
    if( event_add(ev, &tv) < 0 )
    {
        event_del(ev);
        return nullptr;
    } 
    return ev;
}

bool CoreServer::delete_timer_event(event *ev)
{
    int res = event_del(ev);
    return (0 == res);
}




//Conn
Conn::Conn(int fd):m_fd(fd)
{

}

Conn::~Conn() {

}


LibeventThread *Conn::get_thread()
{
    return m_thread;
}


int Conn::get_fd()
{
    return m_fd;
}

//获取可读数据的长度
int Conn::get_read_buffer_len()
{
    return static_cast<int>(evbuffer_get_length(m_readbuf));
}

//从读缓冲区中取出len个字节的数据，存入buffer中，若不够，则读出所有数据
//返回读出数据的字节数
int Conn::get_read_buffer(char *buffer, int len)
{
    return evbuffer_remove(m_readbuf, buffer, len);
}

//从读缓冲区中复制出len个字节的数据，存入buffer中，若不够，则复制出所有数据
//返回复制出数据的字节数
//执行该操作后，数据还会留在缓冲区中，buffer中的数据只是原数据的副本
int Conn::copy_read_buffer(char *buffer, int len)
{
    return static_cast<int>(evbuffer_copyout(m_readbuf, buffer, len));
}

//获取可写数据的长度
int Conn::get_write_buffer_len()
{
    return static_cast<int>(evbuffer_get_length(m_writebuf));
}

//将数据加入写缓冲区，准备发送
int Conn::add_to_write_buffer(char *buffer, int len)
{
    return evbuffer_add(m_writebuf, buffer, len);
}

//将读缓冲区中的数据移动到写缓冲区
void Conn::move_buffer_data()
{
    evbuffer_add_buffer(m_writebuf, m_readbuf);
}
