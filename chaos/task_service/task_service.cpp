/*
 * Copyright 2012, Yunjie Lu.  All rights reserved.
 * https://github.com/lyjdamzwf/chaos
 *
 * Use of this source code is governed by a BSD-style
 * license that can be found in the License file.
 */

#include <assert.h>

#include "task_service.h"

/*! 
 *  @file           task_service.cpp
 *  @author         yunjie.lu
 *  @email          lyjdamzwf@gmail.com
 *  @weibo          http://weibo.com/crazyprogramerlyj
 *  @date           2012.3.24
 *  @brief          task service
 *  @last changed
 *
 */

namespace chaos
{

namespace task_service
{

task_service_t::task_service_t(const string& service_name_)
    :   m_started(false),
        m_service_name(service_name_),
        m_stop_signal(false),
        m_thread_num(0),
        m_fetch_num_per_loop(0xffffffff)
{
}

task_service_t::~task_service_t()
{
    stop();
}

int task_service_t::start(int32_t thread_num_)
{
    if (m_started)
    {
        LOGWARN((TASK_SERVICE_MODULE, "task_service_t::start service(%s) has started, return.", m_service_name.c_str()));

        return -1;
    }

    m_thread_num = thread_num_;

    for (int i = 0; i < m_thread_num; ++i)
    {
        thread_t* tmp_thread = new thread_t;
        if (NULL == tmp_thread)
        {
            LOGERROR((TASK_SERVICE_MODULE, "task_service_t::start service(%s) new thread_t failed, return.", m_service_name.c_str()));
            return -1;
        }

        m_thread_group.add(tmp_thread);
    }

    //! yunjie: 任务队列必须加锁保护
    m_task_queue.initialize(true);

    //! yunjie: 如果是单线程驱动task_service的话, timer内部就不需要进行加锁
    bool lock_flag = (m_thread_num <= 1) ? false : true;
    m_timer_manager.initialize(lock_flag); 
    m_io_handler.initialize(lock_flag); 

    start_thread_t start_thread(*this);
    m_thread_group.exec_all(start_thread);

    m_started = true;

    return 0;
}

int task_service_t::stop()
{
    if (!m_started)
    {
        LOGWARN((TASK_SERVICE_MODULE, "task_service_t::stop service(%s) has stopped, return.", m_service_name.c_str()));

        return -1;
    }

    //! yunjie: 投递异步消息, 设置线程停止执行标志
    post_async_stop_signal();

    //! yunjie: thread* 将会在join_all()中被delete
    m_thread_group.join_all();
    m_thread_num = 0;
    m_fetch_num_per_loop = 0xffffffff;

    assert(0 == m_timer_manager.size());
    assert(0 == m_task_queue.size());

    m_started = false;

    return 0;
}

int task_service_t::post_async_stop_signal()
{
    this->post(async_method_t::bind_memfunc(this, &task_service_t::set_stop_signal, true));

    return 0;
}

bool task_service_t::is_run_on_service()
{
    return m_thread_group.check_is_self(thread_t::get_current_thread_id());
}

void task_service_t::exec_task(thread_t* thd_)
{
    deque<async_method_t> tasks;
    uint32_t all_task_num = 0;
    uint32_t thread_num = m_thread_group.size();

    if (NULL == thd_)
    {
        LOGWARN((TASK_SERVICE_MODULE, "task_service_t::exec_task service(%s) thread is NULL, return", m_service_name.c_str()));
        return;
    }

    if (!thread_num)
    {
        LOGWARN((TASK_SERVICE_MODULE, "task_service_t::exec_task service(%s) thread_num equals zero, return", m_service_name.c_str()));
        return;
    }

    while (!m_stop_signal || !m_task_queue.empty())
    {
        struct timeval cached_now;
        //! yunjie: prepare the task buffer
        tasks.clear();

        while (1)
        {
            //! yunjie: 处理定时事件
            m_timer_manager.flush_time();
            cached_now = m_timer_manager.get_cached_time();

            //! yunjie: 如果有timer时间超时, 将会在函数内部执行callback
            m_timer_manager.exec();

            //! yunjie; 进行网络IO检测, 并会调用相应 读/写 回调
            int wake_num = m_io_handler.wait_io_notification();

            //! yunjie: 本来是采用动态调整m_fetch_num_per_loop来平衡每个线程的任务抓取量, 发现并没有太大意义, 改为一次性全部获取所有任务
            m_task_queue.fetch_task(tasks, all_task_num, m_fetch_num_per_loop);
            if (wake_num || tasks.size() || m_stop_signal)      //! yunjie: 需要判断m_stop_signal, 否则外部投递stop信号号会发生join永远阻塞
            {
                break;
            }

#if USE_COND_VAR
            thd_->cond_wait(cached_now, 0, TIMEDOUT_US);
#else
            thread_t::usleep(TIMEDOUT_US);
#endif
        }

        //! yunjie: calc the fetch_num_per_loop
        if (thread_num != 1)
        {
            m_fetch_num_per_loop = all_task_num / thread_num;
            if (!m_fetch_num_per_loop)
            {
                m_fetch_num_per_loop = MIN_TASK_FETCH_NUM;
            }

            if (m_fetch_num_per_loop > MAX_TASK_FETCH_NUM)
            {
                m_fetch_num_per_loop = MAX_TASK_FETCH_NUM;
            }
        }

        //! yunjie: 处理异步请求
        for (deque<async_method_t>::iterator it = tasks.begin(); it != tasks.end(); ++it)
        {
            (*it)();
            it->release();
        }
    }

    m_timer_manager.clear();

    assert(0 == m_task_queue.size());

}

void task_service_t::set_stop_signal(bool signal_)
{
    m_stop_signal = signal_;

}

int task_service_t::post(const async_method_t& async_method_, void* ext_data_, task_prior_e prior_, bool exec_local_)
{
    //! yunjie: 如果投递消息的是本线程组中的线程, 那么就直接执行, 不进行多余的入队列操作.
    if (exec_local_ && is_run_on_service())
    {
        async_method_t& exec_local_method = const_cast<async_method_t&>(async_method_);
        exec_local_method();
        exec_local_method.release();

        return 0;
    }

    m_task_queue.push(async_method_);
    
#if USE_COND_VAR
    send_cond_signal_t send_cond_signal;
    m_thread_group.exec_all(send_cond_signal);
#endif

    return 0;
}

int task_service_t::clear_all_task()
{
    m_task_queue.clear_all_queue();

    return 0;
}

void task_service_t::register_timer(uint32_t                        interval_,
                                    const time_event_callback_t&    callback_,
                                    bool                            persist_,
                                    time_t                          start_time_
                                   )
{
    //! yunjie: 因为one loop mutilthread内部容器会加锁, 如果在callback中本地上下文执行可能会死锁
    bool is_allow_exec_local = (m_thread_num > 1 ? false : true);

    this->post(async_method_t::bind_memfunc(&m_timer_manager, &timer_manager_t::register_timer, interval_, callback_, persist_, start_time_), NULL, TASK_PRIOR_NORMAL, is_allow_exec_local);
}

void task_service_t::register_io_event(
                                        fd_t                    fd_,
                                        int                     event_type_flag_,
                                        callback_on_event_t     callback_,
                                        void*                   cb_arg_,
                                        bool                    is_persist_
                                     )
{
    //! yunjie: 因为one loop mutilthread内部容器会加锁, 如果在callback中本地上下文执行可能会死锁
    bool is_allow_exec_local = (m_thread_num > 1 ? false : true);

    this->post(async_method_t::bind_memfunc(&m_io_handler, &io_multiplex_handler_t::register_io_event, fd_, event_type_flag_, callback_, cb_arg_, is_persist_), NULL, TASK_PRIOR_NORMAL, is_allow_exec_local);
}

void task_service_t::remove_fd_from_epoll(fd_t fd_)
{
    //! yunjie: 因为one loop mutilthread内部容器会加锁, 如果在callback中本地上下文执行可能会死锁
    bool is_allow_exec_local = (m_thread_num > 1 ? false : true);

    this->post(async_method_t::bind_memfunc(&m_io_handler, &io_multiplex_handler_t::remove_fd_from_epoll, fd_), NULL, TASK_PRIOR_NORMAL, is_allow_exec_local);
}

}

}

