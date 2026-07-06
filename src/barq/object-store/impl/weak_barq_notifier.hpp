////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
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

#ifndef BARQ_WEAK_BARQ_NOTIFIER_HPP
#define BARQ_WEAK_BARQ_NOTIFIER_HPP

#include <memory>
#include <thread>

namespace barq {
class Barq;

namespace util {
class Scheduler;
}

namespace _impl {
// WeakBarqNotifier stores a weak reference to a Barq instance, along with all of
// the information about a Barq that needs to be accessed from other threads.
// This is needed to avoid forming strong references to the Barq instances on
// other threads, which can produce deadlocks when the last strong reference to
// a Barq instance is released from within a function holding the cache lock.
class WeakBarqNotifier {
public:
    WeakBarqNotifier(const std::shared_ptr<Barq>& barq, bool cache);
    ~WeakBarqNotifier();

    // Get a strong reference to the cached barq
    std::shared_ptr<Barq> barq() const
    {
        return m_barq.lock();
    }

    // Has the Barq instance been destroyed?
    bool expired() const
    {
        return m_barq.expired();
    }

    // Is this a WeakBarqNotifier for the given Barq instance?
    bool is_for_barq(Barq* barq) const
    {
        return barq == m_barq_key;
    }
    bool is_cached_for_scheduler(std::shared_ptr<util::Scheduler> scheduler) const;
    bool scheduler_is_on_thread() const;

    // Invoke m_barq.notify() on the Barq's thread via the scheduler.
    void notify();

    // Bind this notifier to the Barq's scheduler.
    void bind_to_scheduler();

private:
    std::weak_ptr<Barq> m_barq;
    void* m_barq_key;
    bool m_cache = false;
    std::shared_ptr<util::Scheduler> m_scheduler;
};

} // namespace _impl
} // namespace barq

#endif // BARQ_WEAK_BARQ_NOTIFIER_HPP
