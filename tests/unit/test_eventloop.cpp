#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"
#include <thread>
#include <chrono>
#include <iostream>

using namespace proxy::network;
using namespace proxy::common;

EventLoop* g_loop;

void timeout() {
    LOG_INFO << "Timeout! Stopping loop.";
    g_loop->Quit();
}

void threadFunc() {
    LOG_INFO << "Thread started.";
    EventLoop loop;
    g_loop = &loop;
    
    // Simulate some work or a timer by queuing a task
    loop.RunInLoop([](){
        LOG_INFO << "Task run in loop immediate";
    });

    // We don't have a TimerQueue yet, so we use a separate thread to quit the loop after some time
    std::thread t([](){
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO << "Calling Quit from another thread";
        g_loop->Quit();
    });
    t.detach();

    loop.Loop();
    LOG_INFO << "Thread loop exited.";
}

int main() {
    Logger::Instance().SetLevel(LogLevel::DEBUG);
    LOG_INFO << "Starting EventLoop test";

    EventLoop loop;
    g_loop = &loop;
    
    std::thread t([]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO << "Quitting main loop from thread";
        g_loop->Quit();
    });

    loop.Loop();
    t.join();
    
    LOG_INFO << "Main loop test passed";
    
    return 0;
}
