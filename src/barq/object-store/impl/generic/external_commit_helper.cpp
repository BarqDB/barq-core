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

#include <barq/object-store/impl/external_commit_helper.hpp>

#include <barq/object-store/impl/barq_coordinator.hpp>

#include <barq/history.hpp>
#include <barq/replication.hpp>

using namespace barq;
using namespace barq::_impl;

ExternalCommitHelper::ExternalCommitHelper(BarqCoordinator& parent, const BarqConfig& config)
    : m_parent(parent)
    , m_sg(DB::create(barq::make_in_barq_history(), config.path,
                      DBOptions(parent.is_in_memory() ? DBOptions::Durability::MemOnly : DBOptions::Durability::Full,
                                parent.get_encryption_key().data())))
    , m_thread(std::async(std::launch::async, [=] {
        auto tr = m_sg->start_read();
        while (m_sg->wait_for_change(tr)) {
            tr->end_read();
            tr = m_sg->start_read();
            m_parent.on_change();
        }
    }))
{
}

ExternalCommitHelper::~ExternalCommitHelper()
{
    m_sg->wait_for_change_release();
    m_thread.wait(); // Wait for the thread to exit
}

void ExternalCommitHelper::notify_others() {}
