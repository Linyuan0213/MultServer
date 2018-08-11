#include <iostream>
#include "server.h"
#include <vector>
class TestServer : public CoreServer
{

private:
    std::vector<Conn *> vec;
protected:
    //重载各个处理业务的虚函数
    void read_cb(Conn *conn) override ;
    void write_cb(Conn *conn) override ;
    void connect_cb(Conn *conn) override ;
    void close_cb(Conn *conn, short events) override ;

public:
    TestServer(int count);;
    ~TestServer();

    //退出事件，响应Ctrl+C
    static void quit_cb(int sig, short events, void *data);
    //定时器事件，每10秒向所有客户端发一句hello, world
    static void timeout_cb(int id, int short events, void *data);
};

void TestServer::read_cb(Conn *conn)
{
    conn->move_buffer_data();
}

void TestServer::write_cb(Conn *conn)
{

}

void TestServer::connect_cb(Conn *conn)
{

    TestServer *me = dynamic_cast<TestServer *>(conn->get_thread()->p_tcp_conn);
    std::cout << "new connection " << conn->get_fd() << std::endl;
    me->vec.push_back(conn);
}

void TestServer::close_cb(Conn *conn, short events)
{
    TestServer *me = dynamic_cast<TestServer *>(conn->get_thread()->p_tcp_conn);
    me->vec.pop_back();
    if (events & BEV_EVENT_EOF)
        std::cout << "connection closed " << conn->get_fd() <<std::endl;
    else if (events & BEV_EVENT_ERROR)
        std::cerr << "some other error" <<std::endl;
}

TestServer::TestServer(int count) : CoreServer(count) {}

TestServer::~TestServer()
{

}

void TestServer::quit_cb(int sig, short events, void *data)
{
    std::cout << "Catch the SIGINT signal, quit in one second" << std::endl;
    TestServer *me = (TestServer*)data;
    timeval tv = {1, 0};
    me->stop_run(&tv);
}

void TestServer::timeout_cb(int id, int short events, void *data)
{
    TestServer *me = (TestServer*)data;
    char temp[33] = "hello, world\n";

    for(int i=0; i<me->vec.size(); i++)
        //std::cout << "vec fd" << me->vec[i]->get_fd() << std::endl;
        me->vec[i]->add_to_write_buffer(temp, strlen(temp));
}

int main()
{

    std::cout << "pid: " << getpid() << std::endl;
    TestServer server(3);
    server.add_signal_event(SIGINT, TestServer::quit_cb);
    timeval tv = {1, 0};
    server.add_timer_event(TestServer::timeout_cb, tv, false);
    server.set_port(9999);
    server.start_run();
    std::cout << "done" << std::endl;
    return 0;
}