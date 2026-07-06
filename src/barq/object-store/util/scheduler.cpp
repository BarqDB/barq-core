////////////////////////////////////////////////////////////////////////////
//
// Copyright 2020 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <barq/object-store/util/scheduler.hpp>
#include <barq/util/terminate.hpp>
#include <barq/version_id.hpp>

#if BARQ_HAVE_UV
#include <barq/object-store/util/uv/scheduler.hpp>
#endif

#if BARQ_PLATFORM_APPLE
#include <barq/object-store/util/apple/scheduler.hpp>
#endif

// When building Barq within the VNDK in AOSP, __ANDROID__ is defined.
// However, access to libandroid is restricted by VNDK policies.
// As a result, we cannot utilize the built-in ALooper functionality.
// Instead, we require users to provide their own scheduler implementation.

#if BARQ_ANDROID && !defined(BARQ_AOSP_VENDOR)
#define HAS_ANDROID_ALOOPER
#endif

#if defined(HAS_ANDROID_ALOOPER)
#include <barq/object-store/util/android/scheduler.hpp>
#endif

#if defined(__EMSCRIPTEN__)
#include <barq/object-store/util/emscripten/scheduler.hpp>
#endif

#include <barq/object-store/util/generic/scheduler.hpp>

namespace barq::util {
namespace {

std::shared_ptr<Scheduler> (*s_factory)() = Scheduler::make_platform_default;

class FrozenScheduler : public util::Scheduler {
public:
    FrozenScheduler(VersionID version)
        : m_version(version)
    {
    }

    void invoke(UniqueFunction<void()>&&) override {}
    bool is_on_thread() const noexcept override
    {
        return true;
    }
    bool is_same_as(const Scheduler* other) const noexcept override
    {
        auto o = dynamic_cast<const FrozenScheduler*>(other);
        return (o && (o->m_version == m_version));
    }
    bool can_invoke() const noexcept override
    {
        return false;
    }

private:
    VersionID m_version;
};

class DummyScheduler : public barq::util::Scheduler {
public:
    bool is_on_thread() const noexcept override
    {
        return true;
    }
    bool is_same_as(const Scheduler* other) const noexcept override
    {
        auto o = dynamic_cast<const DummyScheduler*>(other);
        return (o != nullptr);
    }
    bool can_invoke() const noexcept override
    {
        return false;
    }
    void invoke(UniqueFunction<void()>&&) override {}
};
} // anonymous namespace

void InvocationQueue::push(util::UniqueFunction<void()>&& fn)
{
    std::lock_guard lock(m_mutex);
    m_functions.push_back(std::move(fn));
}

void InvocationQueue::invoke_all()
{
    std::vector<util::UniqueFunction<void()>> functions;
    {
        std::lock_guard lock(m_mutex);
        functions.swap(m_functions);
    }
    for (auto&& fn : functions) {
        fn();
    }
}

Scheduler::~Scheduler() = default;

void Scheduler::set_default_factory(std::shared_ptr<Scheduler> (*factory)())
{
    s_factory = std::move(factory);
}

std::shared_ptr<Scheduler> Scheduler::make_default()
{
    return s_factory();
}

std::shared_ptr<Scheduler> Scheduler::make_platform_default()
{
#if BARQ_USE_UV
    return make_uv();
#else
#if BARQ_PLATFORM_APPLE
    return make_runloop(nullptr);
#elif defined(HAS_ANDROID_ALOOPER)
    return make_alooper();
#elif defined(__EMSCRIPTEN__)
    return std::make_shared<EmscriptenScheduler>();
#else
    BARQ_TERMINATE("No built-in scheduler implementation for this platform. Register your own with "
                    "Scheduler::set_default_factory()");
#endif
#endif // BARQ_USE_UV
}

std::shared_ptr<Scheduler> Scheduler::make_generic()
{
    return std::make_shared<GenericScheduler>();
}

std::shared_ptr<Scheduler> Scheduler::make_frozen(VersionID version)
{
    return std::make_shared<FrozenScheduler>(version);
}

std::shared_ptr<Scheduler> Scheduler::make_dummy()
{
    return std::make_shared<DummyScheduler>();
}

#if BARQ_PLATFORM_APPLE
std::shared_ptr<Scheduler> Scheduler::make_runloop(CFRunLoopRef run_loop)
{
    if (!run_loop)
        run_loop = CFRunLoopGetCurrent();
    if (run_loop == CFRunLoopGetMain())
        return std::make_shared<MainRunLoopScheduler>();
    return std::make_shared<RunLoopScheduler>(run_loop);
}

std::shared_ptr<Scheduler> Scheduler::make_dispatch(void* queue)
{
    return std::make_shared<DispatchQueueScheduler>(static_cast<dispatch_queue_t>(queue));
}
#endif // BARQ_PLATFORM_APPLE

#if defined(HAS_ANDROID_ALOOPER)
std::shared_ptr<Scheduler> Scheduler::make_alooper()
{
    return std::make_shared<ALooperScheduler>();
}
#endif // HAS_ANDROID_ALOOPER

#if BARQ_HAVE_UV
std::shared_ptr<Scheduler> Scheduler::make_uv()
{
    return std::make_shared<UvMainLoopScheduler>();
}
#endif // BARQ_HAVE_UV

} // namespace barq::util
