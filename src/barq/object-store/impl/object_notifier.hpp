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

#ifndef BARQ_OS_OBJECT_NOTIFIER_HPP
#define BARQ_OS_OBJECT_NOTIFIER_HPP

#include <barq/object-store/impl/collection_notifier.hpp>

#include <barq/keys.hpp>
#include <barq/table.hpp>

namespace barq::_impl {
class ObjectNotifier : public CollectionNotifier {
public:
    ObjectNotifier(std::shared_ptr<Barq> barq, const Obj&);

private:
    TableRef m_table;
    ObjKey m_obj_key;
    TransactionChangeInfo* m_info = nullptr;

    void run() override REQUIRES(!m_callback_mutex);
    void reattach() override;
    bool do_add_required_change_info(TransactionChangeInfo& info) override;
};
} // namespace barq::_impl

#endif // BARQ_OS_OBJECT_NOTIFIER_HPP
