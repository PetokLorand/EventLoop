#include <queue>
#include <mutex>
#include <tuple>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <condition_variable>

/****************************************************************/
/*************************** THREAD *****************************/
/****************************************************************/

std::unordered_map<std::thread::id, const char*> g_threadNames;

void setCurrentThreadName(const char* name)
{
    g_threadNames[std::this_thread::get_id()] = name;
}

const char* getCurrentThreadName()
{
    const std::thread::id id = std::this_thread::get_id();
    return g_threadNames.contains(id) ? g_threadNames[id] : "";
}

void sleep(int time)
{
    std::this_thread::sleep_for(std::chrono::seconds(time));
}

/****************************************************************/
/*************************** LOGGER *****************************/
/****************************************************************/

class SpinLock
{
public:
    void lock()
    {
        while (locked.test_and_set(std::memory_order_acquire));
    }

    void unlock()
    {
        locked.clear(std::memory_order_release);
    }
 
private:
    std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

class Logger
{
public:
    Logger(std::ostream& os) : m_os(os) {}

    template <typename... TArgs>
    void info(TArgs&&... args)
    {
        m_os << "[INFO][" << getCurrentThreadName() << "] ";
        (m_os << ... << std::forward<TArgs>(args));
        m_os << '\n';
    }

private:
    std::ostream& m_os;
};

template <typename TLogger>
class SynchronizedLogger
{
public:
    SynchronizedLogger(TLogger logger) : m_logger(logger) {}

    template <typename... TArgs>
    void info(TArgs&&... args)
    {
        std::lock_guard<SpinLock> guard(m_lock);
        m_logger.info(std::forward<TArgs>(args)...);
    }

private:
    TLogger m_logger;
    SpinLock m_lock;
};

Logger logger(std::cout);
SynchronizedLogger<Logger> sl(logger);

/****************************************************************/
/************************* EVENT LOOP ***************************/
/****************************************************************/

template <typename... TArgs>
class EventData
{
public:

private:
    std::pair<std::vector<std::function<void(TArgs...)>>, std::tuple<TArgs...>> m_data;
};

class EventQueue
{
public:
    EventQueue() = default;

    std::function<void()> waitPop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return !m_events.empty(); });

        if (!m_events.empty())
        {
            std::function<void()> event = std::move(m_events.front());
            m_events.pop();
            lock.unlock();
            
            return event;
        }

        return std::function<void()>{ nullptr };
    }

    template <typename TInputIt, typename... TPayload>
    void push(TInputIt first, TInputIt last, const std::tuple<TPayload...>& payload)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        for (auto it = first; it != last; ++it)
        {
            m_events.push([callback = *it, &payload]() { std::apply(callback, payload); });
        }

        lock.unlock();
        m_cv.notify_one();
    }

    void push(std::function<void()> event)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_events.push(event);
        lock.unlock();
        m_cv.notify_one();
    }

    void notify() { m_cv.notify_one(); }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::queue<std::function<void()>> m_events;
};

class EventLoop
{
private:
    template <typename TTag, typename... TPayload>
    requires (!std::disjunction_v<std::is_reference<TPayload>...>)
    friend class Event;

    template <typename TObject>
    friend class Runnable;

public:
    static void start()
    {
        m_stop.store(false, std::memory_order::release);
        while (!m_stop.load(std::memory_order::acquire))
        {
            std::function<void()>&& event = m_eventQueue.waitPop();
            
            if (event != nullptr)
            {
                event();
            }
        }
    }

    static void stop()
    {
        m_stop.store(true, std::memory_order::release);
        m_eventQueue.notify();
    }

private:
    inline static thread_local EventQueue m_eventQueue;
    inline static thread_local std::atomic<bool> m_stop;
};

template <typename... TArgs>
struct function
{
    using type = std::function<void(std::remove_reference_t<TArgs>&...)>;
};

template <>
struct function<void>
{
    using type = std::function<void()>;
};

template <typename... TArgs>
using function_t = typename function<TArgs...>::type;

template <typename TTag, typename... TPayload>
requires (!std::disjunction_v<std::is_reference<TPayload>...>)
class Event
{
private:
    using Function = function_t<TPayload...>;

public:
    Event() = delete;

    static void subscribe(Function callback)
    {
        std::lock_guard<SpinLock> guard(m_lock);
        m_subscriptions[&EventLoop::m_eventQueue].push_back(callback);
    }

    template <typename... TArgs> 
    requires std::conjunction_v<std::is_same<std::decay_t<TArgs>, std::decay_t<TPayload>>...>
    static void publish(TArgs&&... payload)
    {
        std::lock_guard<SpinLock> guard(m_lock);

        auto sharedPayload = std::make_shared<std::tuple<TPayload...>>(std::forward<TArgs>(payload)...);

        for (auto& [eventQueue, callbacks] : m_subscriptions)
        {
            for (auto callback : callbacks)
            {
                eventQueue->push([callback, sharedPayload]() { std::apply(callback, *sharedPayload); });
            }
        }
    }

private:
    inline static SpinLock m_lock;
    inline static std::unordered_map<EventQueue*, std::vector<Function>> m_subscriptions;
};

template <typename TObject>
class Runnable
{
public:
    Runnable(const char* threadName = "", bool deferredStart = false) : m_threadName(threadName) { if (!deferredStart) { start(); }}

    ~Runnable()
    {
        stop();
    }

    template <typename... TArgs>
    void start(TArgs&&... args)
    {        
        if (isStarted())
        {
            return;
        }

        m_started.store(true, std::memory_order::release);

        m_thread = std::thread([this, objArgs = std::tuple<TArgs...>(std::forward<TArgs>(args)...)]()
        {
            setCurrentThreadName(m_threadName);
            m_eventQueue = &EventLoop::m_eventQueue;
            m_running.store(true, std::memory_order::release);
            [[maybe_unused]] TObject object = std::make_from_tuple<TObject>(objArgs);
            EventLoop::start();
        });
    }

    void stop()
    {
        if (!isStarted())
        {
            return;
        }

        while (!isRunning());

        m_eventQueue->push([](){ EventLoop::stop(); });
        m_thread.join();
        m_running.store(false, std::memory_order::release);
        m_started.store(false, std::memory_order::release);
        
    }

    bool isRunning() const { return m_running.load(std::memory_order::acquire); }
    bool isStarted() const { return m_started.load(std::memory_order::acquire); }

private:
    std::thread m_thread;
    const char* m_threadName { nullptr };
    EventQueue* m_eventQueue { nullptr };
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_started { false };
};

/****************************************************************/
/*************************** TESTING ****************************/
/****************************************************************/

#include <chrono>

class Argument
{
public:
    Argument(int value) : m_value(value) { print("Ctor"); }
    ~Argument() { sl.info("Dtor", m_deleted ? " - Deleted!" : ""); m_deleted = true; }
    Argument(const Argument& rhs) { initFrom(rhs); print("Copy Ctor"); }
    Argument(Argument&& rhs) { initFrom(rhs); print("Move Ctor"); rhs.m_moved = true; }
    Argument& operator=(const Argument& rhs) { initFrom(rhs); print("Copy Assign Op"); return *this; }    
    Argument& operator=(Argument&& rhs) { initFrom(rhs); print("Move Assign Op"); rhs.m_moved = true; return *this; }
    int value() const { print("Getter"); return m_value; }

private:
    void print(const char* msg) const { sl.info(msg, m_moved ? " - Moved!" : "", m_deleted ? " - Deleted!" : ""); }; 

    void initFrom(const Argument& rhs)
    {
        m_value = rhs.m_value;
        m_moved = rhs.m_moved.load();
        m_deleted = rhs.m_deleted.load();
    }

    int m_value { 0 };
    std::atomic<bool> m_moved { false };
    std::atomic<bool> m_deleted { false };
};

class SystemEventTag;
using SystemEvent = Event<SystemEventTag, Argument>;

struct Subscriber
{
    Subscriber()
    {
        sl.info("CHECK");
        SystemEvent::subscribe([](Argument const& arg) { sl.info("Handler One: ", arg.value()); });
        SystemEvent::subscribe([](Argument const& arg) { sl.info("Handler Two: ", arg.value()); });
    }
};

int main()
{    
    setCurrentThreadName("MAIN");

    Runnable<Subscriber> foo("FOO ");
    Runnable<Subscriber> bar("BAR ");

    Argument lvalue { 21 };
    Argument xvalue { 42 };

    SystemEvent::publish(lvalue);
    SystemEvent::publish(std::move(xvalue));
    sl.info("CHECK");

    return 0;
}