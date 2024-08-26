#include "fiber.h"
#include <string.h>

static bool debug = false;

namespace sylar {

// 当前线程上的协程控制信息

// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程计数器
static std::atomic<uint64_t> s_fiber_id{0};
// 协程id
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f)
{
	t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{
	if(t_fiber)
	{	
		return t_fiber->shared_from_this();
	}

	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_thread_fiber = main_fiber;
	t_scheduler_fiber = main_fiber.get(); // 除非主动设置 主协程默认为调度协程
	
	assert(t_fiber == main_fiber.get());
	return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f)
{
	t_scheduler_fiber = f;
}

uint64_t Fiber::GetFiberId()
{
	if(t_fiber)
	{
		return t_fiber->getId();
	}
	return (uint64_t)-1;
}

Fiber::Fiber()
{
    SetThis(this);
    m_state = RUNNING;
    cIsShareStack = false;
    stack_sp = nullptr;
    save_size = 0;
    save_buffer = nullptr;
    pending_co = nullptr;
    occupy_co = nullptr;
    stack_mem = nullptr;
    
    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber() failed\n";
        pthread_exit(NULL);
    }
    
    m_id = s_fiber_id++;
    s_fiber_count ++;
    if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler, bool share_stack):
m_cb(cb), m_runInScheduler(run_in_scheduler), cIsShareStack(share_stack)
{
    m_state = READY;

    // 分配协程栈空间
    m_stacksize = stacksize ? stacksize : 128000;
    if (!cIsShareStack) {
        m_stack = malloc(m_stacksize);
    } else {
        // 使用共享栈
        m_stack = nullptr;
        stack_mem = co_alloc_stackmem(m_stacksize);
    }

    if(getcontext(&m_ctx))
    {
        std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
        pthread_exit(NULL);
    }
    
    m_ctx.uc_link = nullptr;
    if (!cIsShareStack) {
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;
    } else {
        m_ctx.uc_stack.ss_sp = stack_mem->stack_buffer;
        m_ctx.uc_stack.ss_size = stack_mem->stack_size;
    }
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    
    m_id = s_fiber_id++;
    s_fiber_count ++;
    if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
	s_fiber_count --;
	if(m_stack)
	{
		free(m_stack);
	}
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}

void Fiber::reset(std::function<void()> cb)
{
    assert(m_stack != nullptr && m_state == TERM);

    m_state = READY;
    m_cb = cb;

    if(getcontext(&m_ctx))
    {
        std::cerr << "reset() failed\n";
        pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
    if (!cIsShareStack) {
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;
    } else {
        m_ctx.uc_stack.ss_sp = stack_mem->stack_buffer;
        m_ctx.uc_stack.ss_size = stack_mem->stack_size;
    }
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

void Fiber::resume()
{
    assert(m_state == READY);
    
    m_state = RUNNING;

    if (cIsShareStack) {
        co_swap(t_fiber, this);
    } else {
        if(m_runInScheduler)
        {
            SetThis(this);
            if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
            {
                std::cerr << "resume() to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
            }		
        }
        else
        {
            SetThis(this);
            if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
            {
                std::cerr << "resume() to t_thread_fiber failed\n";
                pthread_exit(NULL);
            }	
        }
    }
}

void Fiber::yield()
{
    assert(m_state == RUNNING || m_state == TERM);

    if(m_state != TERM)
    {
        m_state = READY;
    }

    if (cIsShareStack) {
        co_swap(this, t_fiber);
    } else {
        if(m_runInScheduler)
        {
            SetThis(t_scheduler_fiber);
            if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
            {
                std::cerr << "yield() to t_scheduler_fiber failed\n";
                pthread_exit(NULL);
            }		
        }
        else
        {
            SetThis(t_thread_fiber.get());
            if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
            {
                std::cerr << "yield() to t_thread_fiber failed\n";
                pthread_exit(NULL);
            }	
        }
    }	
}

void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis();
	assert(curr!=nullptr);

	curr->m_cb(); 
	curr->m_cb = nullptr;
	curr->m_state = TERM;

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get();
	curr.reset(); 
	raw_ptr->yield(); 
}

/**
* 创建⼀个共享栈
* @param count 创建共享栈的个数
* @param stack_size 每个共享栈的⼤⼩
*/
Fiber::stShareStack_t* Fiber::co_alloc_sharestack(int count, int stack_size) {
    stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
    share_stack->alloc_idx = 0;
    share_stack->stack_size = stack_size;
    //alloc stack array
    share_stack->count = count;
    std::vector<stStackMem_t*> stack_array(count);
    for (int i = 0; i < count; i++)
    {
        stack_array[i] = co_alloc_stackmem(stack_size); //co_alloc_stackmem⽤于分配每个共享栈内存, 实现⻅下
    }
    share_stack->stack_array = stack_array;
    return share_stack;
}

/**
* 分配⼀个栈内存
* @param stack_size的⼤⼩
*/
// 分配栈内存
Fiber::stStackMem_t* Fiber::co_alloc_stackmem(unsigned int stack_size) {
    stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
    stack_mem->occupy_co = NULL; //当前没有协程使⽤该共享栈
    stack_mem->stack_size = stack_size;
    stack_mem->stack_buffer = (char*)malloc(stack_size); //栈顶，低内存空间
    stack_mem->stack_bp = stack_mem->stack_buffer + stack_size; //栈底，⾼地址空间
    return stack_mem;
}

/**
* 1. 将当前的运⾏上下⽂保存到curr中
* 2. 将当前的运⾏上下⽂替换为pending_co中的上下⽂
* @param curr
* @param pending_co
*/
void Fiber::co_swap(Fiber* curr, Fiber* pending_co)
{
    Fiber* env = t_fiber;
    //get curr stack sp
    //c变量的作⽤是为了找到⽬前的栈顶，因为c变量是最后⼀个放⼊栈中的内容。
    char c;
    curr->stack_sp = &c;
    if (!pending_co->cIsShareStack)
    {
        // 如果没有采⽤共享栈，清空pending_co和occupy_co
        env->pending_co = NULL;
        env->occupy_co = NULL;
    }
    else
    {
        // 如果采⽤了共享栈
        env->pending_co = pending_co;

        //get last occupy co on the same stack mem
        // occupy_co指的是，和pending_co共同使⽤⼀个共享栈的协程
        // 把它取出来是为了先把occupy_co的内存保存起来
        Fiber* occupy_co = pending_co->stack_mem->occupy_co;
        //set pending co to occupy thest stack mem;
        // 将该共享栈的占⽤者改为pending_co
        pending_co->stack_mem->occupy_co = pending_co;
        env->occupy_co = occupy_co;
        if (occupy_co && occupy_co != pending_co)
        {
            // 如果上⼀个使⽤协程不为空, 则需要把它的栈内容保存起来，⻅下个函数。
            save_stack_buffer(occupy_co);
        }
    }
    // swap context
    swapcontext(&(curr->m_ctx), &(pending_co->m_ctx));
    // 上⼀步coctx_swap会进⼊到pending_co的协程环境中运⾏
    // 到这⼀步，已经yield回此协程了，才会执⾏下⾯的语句
    // ⽽yield回此协程之前，env->pending_co会被上⼀层协程设置为此协程
    // 因此可以顺利执⾏: 将之前保存起来的栈内容，恢复到运⾏栈上
    //stack buffer may be overwrite, so get again;
    Fiber* curr_env = t_fiber;
    Fiber* update_occupy_co = curr_env->occupy_co;
    Fiber* update_pending_co = curr_env->pending_co;
    // 将栈的内容恢复，如果不是共享栈的话，每个协程都有⾃⼰独⽴的栈空间，则不⽤恢复。
    if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
    {
        // resume stack buffer
        if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
        {
            // 将之前保存起来的栈内容，恢复到运⾏栈上
            memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
        }
    }
}

/**
* 将原本占⽤共享栈的协程的内存保存起来。
* @param occupy_co 原本占⽤共享栈的协程
*/
void Fiber::save_stack_buffer(Fiber* occupy_co) {
    ///copy out
    stStackMem_t* stack_mem = occupy_co->stack_mem;
    // 计算出栈的⼤⼩
    int len = stack_mem->stack_bp - occupy_co->stack_sp;
    if (occupy_co->save_buffer)
    {
        free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
    }
    occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
    occupy_co->save_size = len;
    // 将当前运⾏栈的内容，拷⻉到save_buffer中
    memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

}

