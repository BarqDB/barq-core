////////////////////////////////////////////////////////////////////////////
//
// Copyright 2017 Realm Inc.
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

#include <algorithm>

using namespace barq;
using namespace barq::_impl;
using namespace barq::util;

static std::string normalize_barq_path_for_windows_kernel_object_name(std::string barq_path)
{
    // windows named objects names should not contain backslash
    std::replace(barq_path.begin(), barq_path.end(), '\\', '/');

    // always use lowercase for the drive letter as a win32 named objects name
    auto position = barq_path.find(':');
    if (position != std::string::npos && position > 0) {
        barq_path[position - 1] = tolower(barq_path[position - 1]);
    }

    return barq_path;
}

static std::string create_condvar_sharedmemory_name(std::string barq_path)
{
    barq_path = normalize_barq_path_for_windows_kernel_object_name(barq_path);

    std::string name("Local\\Barq_ObjectStore_ExternalCommitHelper_SharedCondVar_");
    name.append(barq_path);
    return name;
}

ExternalCommitHelper::ExternalCommitHelper(BarqCoordinator& parent, const BarqConfig& config)
    : m_parent(parent)
    , m_shared_part(create_condvar_sharedmemory_name(config.path))
{
    auto unneeded = InterprocessMutex::SharedPart();
    m_mutex.set_shared_part(unneeded, normalize_barq_path_for_windows_kernel_object_name(config.path),
                            "ExternalCommitHelper_ControlMutex");

    m_commit_available.set_shared_part(
        m_shared_part->cv, normalize_barq_path_for_windows_kernel_object_name(config.path),
        "ExternalCommitHelper_CommitCondVar",
        normalize_barq_path_for_windows_kernel_object_name(std::filesystem::temp_directory_path().string()));

    {
        std::lock_guard lock(m_mutex);
        m_last_count = m_shared_part->num_signals;
    }

    m_thread = std::thread([this]() {
        listen();
    });
}

ExternalCommitHelper::~ExternalCommitHelper()
{
    {
        std::lock_guard lock(m_mutex);
        m_keep_listening = false;
        m_commit_available.notify_all();
    }
    m_thread.join();

    m_commit_available.release_shared_part();
}

void ExternalCommitHelper::notify_others()
{
    std::lock_guard lock(m_mutex);
    m_shared_part->num_signals++;
    m_commit_available.notify_all();
}

void ExternalCommitHelper::listen()
{
    auto lock = std::unique_lock(m_mutex);
    while (true) {
        m_commit_available.wait(m_mutex, nullptr, [&] {
            return !m_keep_listening || m_shared_part->num_signals != m_last_count;
        });
        m_last_count = m_shared_part->num_signals;

        if (!m_keep_listening)
            return;

        lock.unlock();
        m_parent.on_change();
        lock.lock();
    }
}
