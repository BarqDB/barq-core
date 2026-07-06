/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#pragma once

#include <barq/object-store/c_api/util.hpp>
#include <barq/object-store/binding_context.hpp>

namespace barq::c_api {

class CBindingContext : public BindingContext {
public:
    static CBindingContext& get(SharedBarq barq);

    CBindingContext() = default;
    CBindingContext(SharedBarq barq)
        : BindingContext()
    {
        this->barq = barq;
    }

    CallbackRegistry<>& barq_changed_callbacks()
    {
        return m_barq_changed_callbacks;
    }

    CallbackRegistryWithVersion<>& barq_pending_refresh_callbacks()
    {
        return m_barq_pending_refresh_callbacks;
    }

    CallbackRegistry<const Schema&>& schema_changed_callbacks()
    {
        return m_schema_changed_callbacks;
    }

protected:
    void did_change(std::vector<ObserverState> const&, std::vector<void*> const&, bool) final;

    void schema_did_change(const Schema& schema) final
    {
        m_schema_changed_callbacks.invoke(schema);
    }

private:
    CallbackRegistry<> m_barq_changed_callbacks;
    CallbackRegistryWithVersion<> m_barq_pending_refresh_callbacks;
    CallbackRegistry<const Schema&> m_schema_changed_callbacks;
};

} // namespace barq::c_api
