#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>     
#include <memory>       
#include <atomic>       
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>
#include <vector>

namespace sylar {

class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
    /**
	* ⼀个共享栈的结构体，每个共享栈的内存所在
	* ⼀个进程或者线程栈的地址，是从⾼位到低位安排数据的，所以stack_bp是栈底，stack_buffer是栈顶
	*/
    struct stStackMem_t {
        Fiber* occupy_co; // 当前正在使用该共享栈的协程
        int stack_size; // 栈的大小
        char* stack_bp; // stack_buffer + stack_size 栈底
        char* stack_buffer; // 栈的内容，也就是栈顶
    };

    /*
	* 所有共享栈的结构体，这⾥的共享栈是个数组，每个元素分别是个共享栈
	*/
    struct stShareStack_t {
        unsigned int alloc_idx; // 当前正在使用的那个共享栈的index
        int stack_size; // 共享栈的大小，这⾥的⼤⼩指的是⼀个stStackMem_t*的⼤⼩
        int count; // 共享栈的个数，共享栈可以为多个，所以以下为共享栈的数组
        std::vector<stStackMem_t*> stack_array; // 栈的内容，这⾥是个数组，元素是stStackMem_t*
    };

	bool cIsShareStack;
	stStackMem_t* stack_mem;

	//save satck buffer while confilct on same stack_buffer;
	char* stack_sp; 
	unsigned int save_size;
	char* save_buffer;

	Fiber* pending_co;
	Fiber* occupy_co;

public:
    
	// 协程状态
    enum State
    {
        READY, 
        RUNNING, 
        TERM 
    };

private:
    // 仅由GetThis()调用 -> 私有 -> 创建主协程  
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true, bool share_stack = 0);
    ~Fiber();

    // 重用一个协程
    void reset(std::function<void()> cb);

    // 任务线程恢复执行
    void resume();
    // 任务线程让出执行权
    void yield();

    uint64_t getId() const {return m_id;}
    State getState() const {return m_state;}

	// 创建共享栈
    stShareStack_t* co_alloc_sharestack(int count, int stack_size);

    // 分配栈内存
    stStackMem_t* co_alloc_stackmem(unsigned int stack_size);

    // 协程切换
    void co_swap(Fiber* curr, Fiber* pending_co);

    // 保存栈内容
    void save_stack_buffer(Fiber* occupy_co);

public:
    // 设置当前运行的协程
    static void SetThis(Fiber *f);

    // 得到当前运行的协程 
    static std::shared_ptr<Fiber> GetThis();

    // 设置调度协程（默认为主协程）
    static void SetSchedulerFiber(Fiber* f);
    
    // 得到当前运行的协程id
    static uint64_t GetFiberId();

    // 协程函数
    static void MainFunc();    

private:
    // id
    uint64_t m_id = 0;
    // 栈大小
    uint32_t m_stacksize = 0;
    // 协程状态
    State m_state = READY;
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈指针
    void* m_stack = nullptr;
    // 协程函数
    std::function<void()> m_cb;
    // 是否让出执行权交给调度协程
    bool m_runInScheduler;

public:
    std::mutex m_mutex;
};

}

#endif