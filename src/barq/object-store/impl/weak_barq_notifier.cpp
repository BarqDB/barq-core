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

#include <barq/object-store/impl/weak_barq_notifier.hpp>

#include <barq/object-store/shared_barq.hpp>
#include <barq/object-store/util/scheduler.hpp>

using namespace barq;
using namespace barq::_impl;


WeakBarqNotifier::WeakBarqNotifier(const std::shared_ptr<Barq>& barq, bool cache)
    : m_barq(barq)
    , m_barq_key(barq.get())
    , m_cache(cache)
{
    bind_to_scheduler();
}

WeakBarqNotifier::~WeakBarqNotifier() = default;

void WeakBarqNotifier::notify()
{
    if (m_scheduler) {
        m_scheduler->invoke([weak_barq = m_barq] {
            if (auto barq = weak_barq.lock()) {
                barq->notify();
            }
        });
    }
}

void WeakBarqNotifier::bind_to_scheduler()
{
    BARQ_ASSERT(!m_scheduler);
    m_scheduler = barq()->scheduler();
}

bool WeakBarqNotifier::is_cached_for_scheduler(std::shared_ptr<util::Scheduler> scheduler) const
{
    return m_cache && (m_scheduler && scheduler) && (m_scheduler->is_same_as(scheduler.get()));
}

bool WeakBarqNotifier::scheduler_is_on_thread() const
{
    return m_scheduler && m_scheduler->is_on_thread();
}
