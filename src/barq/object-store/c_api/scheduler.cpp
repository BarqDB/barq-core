#include <barq/object-store/c_api/util.hpp>
#include <barq/object-store/util/scheduler.hpp>

#if defined(BARQ_USE_UV) && BARQ_USE_UV
#define BARQ_HAS_DEFAULT_SCHEDULER 1
#elif defined(BARQ_USE_CF) && BARQ_USE_CF
#define BARQ_HAS_DEFAULT_SCHEDULER 1
#elif defined(BARQ_USE_ALOOPER) && BARQ_USE_ALOOPER
#define BARQ_HAS_DEFAULT_SCHEDULER 1
#else
#define BARQ_HAS_DEFAULT_SCHEDULER 0
#endif

using namespace barq::util;

// LCOV_EXCL_START

struct barq_work_queue : InvocationQueue {};

namespace barq::c_api {
namespace {

struct CAPIScheduler : Scheduler {
    void* m_userdata = nullptr;
    barq_free_userdata_func_t m_free = nullptr;
    barq_scheduler_notify_func_t m_notify = nullptr;
    barq_scheduler_is_on_thread_func_t m_is_on_thread = nullptr;
    barq_scheduler_is_same_as_func_t m_is_same_as = nullptr;
    barq_scheduler_can_deliver_notifications_func_t m_can_invoke = nullptr;

    barq_work_queue m_queue;

    CAPIScheduler() = default;
    CAPIScheduler(CAPIScheduler&& other)
        : m_userdata(std::exchange(other.m_userdata, nullptr))
        , m_free(std::exchange(other.m_free, nullptr))
        , m_notify(std::exchange(other.m_notify, nullptr))
        , m_is_on_thread(std::exchange(other.m_is_on_thread, nullptr))
        , m_can_invoke(std::exchange(other.m_can_invoke, nullptr))
    {
    }

    ~CAPIScheduler()
    {
        if (m_free)
            m_free(m_userdata);
    }

    void invoke(util::UniqueFunction<void()>&& fn) final
    {
        if (m_notify) {
            m_queue.push(std::move(fn));
            m_notify(m_userdata, &m_queue);
        }
    }

    bool is_on_thread() const noexcept final
    {
        if (m_is_on_thread)
            return m_is_on_thread(m_userdata);
        return false;
    }

    bool is_same_as(const Scheduler* other) const noexcept final
    {
        if (auto rhs = dynamic_cast<const CAPIScheduler*>(other)) {
            bool same_callbacks = m_free == rhs->m_free && m_notify == rhs->m_notify &&
                                  m_is_same_as == rhs->m_is_same_as && m_is_on_thread == rhs->m_is_on_thread &&
                                  m_can_invoke == rhs->m_can_invoke;
            if (same_callbacks && m_userdata == rhs->m_userdata) {
                return true;
            }
            if (same_callbacks && m_is_same_as) {
                return m_is_same_as(m_userdata, rhs->m_userdata);
            }
        }
        return false;
    }

    bool can_invoke() const noexcept final
    {
        if (m_can_invoke)
            return m_can_invoke(m_userdata);
        return false;
    }
};

struct DefaultFactory {
    struct Inner {
        void* m_userdata = nullptr;
        barq_free_userdata_func_t m_free_func = nullptr;
        barq_scheduler_default_factory_func_t m_factory_func = nullptr;

        ~Inner()
        {
            if (m_free_func)
                m_free_func(m_userdata);
        }
    };

    // Indirection because we are wrapping ourselves in an `std::function`,
    // which must be copyable.
    std::shared_ptr<Inner> m_inner;

    DefaultFactory(barq_userdata_t userdata, barq_free_userdata_func_t free_func,
                   barq_scheduler_default_factory_func_t factory_func)
        : m_inner(std::make_shared<Inner>())
    {
        m_inner->m_userdata = userdata;
        m_inner->m_free_func = free_func;
        m_inner->m_factory_func = factory_func;
    }

    std::shared_ptr<Scheduler> operator()()
    {
        if (m_inner->m_factory_func) {
            auto ptr = m_inner->m_factory_func(m_inner->m_userdata);
            std::shared_ptr<Scheduler> scheduler = *ptr;
            barq_release(ptr);
            return scheduler;
        }
        return nullptr;
    }
};

} // namespace

BARQ_API barq_scheduler_t*
barq_scheduler_new(barq_userdata_t userdata, barq_free_userdata_func_t free_func,
                    barq_scheduler_notify_func_t notify_func, barq_scheduler_is_on_thread_func_t is_on_thread_func,
                    barq_scheduler_is_same_as_func_t is_same_as,
                    barq_scheduler_can_deliver_notifications_func_t can_deliver_notifications_func)
{
    return wrap_err([&]() {
        auto capi_scheduler = std::make_shared<CAPIScheduler>();
        capi_scheduler->m_userdata = userdata;
        capi_scheduler->m_free = free_func;
        capi_scheduler->m_notify = notify_func;
        capi_scheduler->m_is_on_thread = is_on_thread_func;
        capi_scheduler->m_is_same_as = is_same_as;
        capi_scheduler->m_can_invoke = can_deliver_notifications_func;
        return new barq_scheduler_t(std::move(capi_scheduler));
    });
}

BARQ_API void barq_scheduler_perform_work(barq_work_queue_t* work_queue)
{
    work_queue->invoke_all();
}

BARQ_API barq_scheduler_t* barq_scheduler_make_default()
{
    return wrap_err([&]() {
        return new barq_scheduler_t{Scheduler::make_default()};
    });
}

BARQ_API const barq_scheduler_t* barq_scheduler_get_frozen()
{
    return wrap_err([&]() {
        // FIXME: Provide a `barq_version_id_t`.
        return static_cast<barq_scheduler_t*>(nullptr);
        // static const barq_scheduler_t* frozen = new barq_scheduler_t{Scheduler::get_frozen()};
        // return frozen;
    });
}

} // namespace barq::c_api

// LCOV_EXCL_STOP
