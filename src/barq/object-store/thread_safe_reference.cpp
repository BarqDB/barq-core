////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
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

#include <barq/object-store/thread_safe_reference.hpp>

#include <barq/object-store/list.hpp>
#include <barq/object-store/set.hpp>
#include <barq/object-store/dictionary.hpp>
#include <barq/object-store/object.hpp>
#include <barq/object-store/object_schema.hpp>
#include <barq/object-store/results.hpp>
#include <barq/object-store/shared_barq.hpp>

#include "impl/barq_coordinator.hpp"

#include <barq/db.hpp>
#include <barq/keys.hpp>

namespace barq {

using OsDict = object_store::Dictionary;

class ThreadSafeReference::Payload {
public:
    virtual ~Payload() = default;
    Payload(Barq& barq)
        : m_source_version(barq.current_transaction_version())
        , m_created_in_write_transaction(barq.is_in_transaction())
    {
    }

    void refresh_target_barq(Barq&);

private:
    const util::Optional<VersionID> m_source_version;
    const bool m_created_in_write_transaction;
};

void ThreadSafeReference::Payload::refresh_target_barq(Barq& barq)
{
    if (!m_source_version)
        return;
    if (!barq.is_in_read_transaction()) {
        try {
            // If the TSR was created in a write transaction then we want to
            // resolve it at the version created by committing that transaction.
            // That's not possible, so we just use latest.
            if (!m_created_in_write_transaction) {
                Barq::Internal::begin_read(barq, *m_source_version);
                return;
            }
        }
        catch (const DB::BadVersion&) {
            // The TSR's source version was cleaned up, so just use the latest
        }
        barq.read_group();
    }
    else {
        auto version = barq.read_transaction_version();
        if (version < m_source_version || (version == m_source_version && m_created_in_write_transaction))
            barq.refresh();
    }
}

template <typename Collection>
class ThreadSafeReference::CollectionPayload : public ThreadSafeReference::Payload {
public:
    CollectionPayload(object_store::Collection const& collection)
        : Payload(*collection.get_barq())
        , m_key(collection.get_parent_object_key())
        , m_table_key(collection.get_parent_table_key())
        , m_col_key(collection.get_parent_column_key())
    {
    }

    Collection import_into(std::shared_ptr<Barq> const& r)
    {
        Obj obj = r->read_group().get_table(m_table_key)->get_object(m_key);
        return Collection(r, obj, m_col_key);
    }

private:
    ObjKey m_key;
    TableKey m_table_key;
    ColKey m_col_key;
};

template <>
class ThreadSafeReference::PayloadImpl<List> : public ThreadSafeReference::CollectionPayload<List> {
public:
    using ThreadSafeReference::CollectionPayload<List>::CollectionPayload;
};

template <>
class ThreadSafeReference::PayloadImpl<object_store::Set>
    : public ThreadSafeReference::CollectionPayload<object_store::Set> {
public:
    using ThreadSafeReference::CollectionPayload<object_store::Set>::CollectionPayload;
};

template <>
class ThreadSafeReference::PayloadImpl<OsDict> : public ThreadSafeReference::CollectionPayload<OsDict> {
public:
    using ThreadSafeReference::CollectionPayload<OsDict>::CollectionPayload;
};

template <>
class ThreadSafeReference::PayloadImpl<Object> : public ThreadSafeReference::Payload {
public:
    PayloadImpl(Object const& object)
        : Payload(*object.get_barq())
        , m_key(object.get_obj().get_key())
        , m_object_schema_name(object.get_object_schema().name)
    {
    }

    Object import_into(std::shared_ptr<Barq> const& r)
    {
        return Object(r, m_object_schema_name, m_key);
    }

private:
    ObjKey m_key;
    std::string m_object_schema_name;
};

template <>
class ThreadSafeReference::PayloadImpl<Results> : public ThreadSafeReference::Payload {
public:
    PayloadImpl(Results const& r)
        : Payload(*r.get_barq())
        , m_coordinator(Barq::Internal::get_coordinator(*r.get_barq()).shared_from_this())
        , m_ordering(r.get_descriptor_ordering())
    {
        if (r.get_type() != PropertyType::Object) {
            auto list = r.get_collection();
            BARQ_ASSERT(list);
            m_key = list->get_owner_key();
            m_table_key = list->get_table()->get_key();
            // FIXME: Use path for list when supporting notifications for nested collections
            m_col_key = list->get_col_key();
        }
        else {
            Query q(r.get_query());
            m_transaction = r.get_barq()->duplicate();
            m_query = m_transaction->import_copy_of(q, PayloadPolicy::Stay);
            // If the Query is derived from a collection which was created in
            // the current write transaction then the collection cannot be
            // handed over and would just be empty when resolved.
            if (q.view_owner_obj_key() != m_query->view_owner_obj_key()) {
                throw WrongTransactionState(
                    "Cannot create a ThreadSafeReference to Results backed by a collection of objects "
                    "inside the write transaction which created the collection.");
            }
        }
    }

    Results import_into(std::shared_ptr<Barq> const& r)
    {
        if (m_key) {
            CollectionBasePtr collection;
            auto table = r->read_group().get_table(m_table_key);
            try {
                collection = table->get_object(m_key).get_collection_ptr(m_col_key);
            }
            catch (KeyNotFound const&) {
                // Create a detached list of the appropriate type so that we
                // return an invalid Results rather than an Empty Results, to
                // match what happens for other types of handover where the
                // object doesn't exist.
                if (m_col_key.is_dictionary()) {
                    collection = std::make_unique<Dictionary>();
                }
                else {
                    switch_on_type(ObjectSchema::from_core_type(m_col_key), [&](auto* t) -> void {
                        if (m_col_key.is_list()) {
                            collection = std::make_unique<Lst<NonObjTypeT<decltype(*t)>>>();
                        }
                        else if (m_col_key.is_set()) {
                            collection = std::make_unique<Set<NonObjTypeT<decltype(*t)>>>();
                        }
                    });
                }
            }
            return Results(r, std::move(collection), m_ordering);
        }
        auto q = r->import_copy_of(*m_query, PayloadPolicy::Stay);
        return Results(std::move(r), std::move(*q), m_ordering);
    }

private:
    const std::shared_ptr<_impl::BarqCoordinator> m_coordinator;
    TransactionRef m_transaction;
    DescriptorOrdering m_ordering;
    std::unique_ptr<Query> m_query;
    ObjKey m_key;
    TableKey m_table_key;
    ColKey m_col_key;
};

template <>
class ThreadSafeReference::PayloadImpl<std::shared_ptr<Barq>> : public ThreadSafeReference::Payload {
public:
    PayloadImpl(std::shared_ptr<Barq> const& barq)
        : Payload(*barq)
        , m_barq(barq)
    {
    }

    std::shared_ptr<Barq> get_barq()
    {
        return std::move(m_barq);
    }

private:
    std::shared_ptr<Barq> m_barq;
};

ThreadSafeReference::ThreadSafeReference() noexcept = default;
ThreadSafeReference::~ThreadSafeReference() = default;
ThreadSafeReference::ThreadSafeReference(ThreadSafeReference&&) noexcept = default;
ThreadSafeReference& ThreadSafeReference::operator=(ThreadSafeReference&&) noexcept = default;

template <typename T>
ThreadSafeReference::ThreadSafeReference(T const& value)
{
    auto barq = value.get_barq();
    barq->verify_thread();
    m_payload.reset(new PayloadImpl<T>(value));
}

template <>
ThreadSafeReference::ThreadSafeReference(std::shared_ptr<Barq> const& value)
{
    m_payload.reset(new PayloadImpl<std::shared_ptr<Barq>>(value));
}

template ThreadSafeReference::ThreadSafeReference(List const&);
template ThreadSafeReference::ThreadSafeReference(object_store::Set const&);
template ThreadSafeReference::ThreadSafeReference(OsDict const&);
template ThreadSafeReference::ThreadSafeReference(Results const&);
template ThreadSafeReference::ThreadSafeReference(Object const&);

template <typename T>
T ThreadSafeReference::resolve(std::shared_ptr<Barq> const& barq)
{
    BARQ_ASSERT(barq);
    barq->verify_thread();

    BARQ_ASSERT(m_payload);
    auto& payload = static_cast<PayloadImpl<T>&>(*m_payload);
    BARQ_ASSERT(typeid(payload) == typeid(PayloadImpl<T>));

    m_payload->refresh_target_barq(*barq);
    try {
        return payload.import_into(barq);
    }
    catch (KeyNotFound const&) {
        // Object was deleted in a version after when the TSR was created
        return {};
    }
}

template <>
std::shared_ptr<Barq> ThreadSafeReference::resolve<std::shared_ptr<Barq>>(std::shared_ptr<Barq> const&)
{
    BARQ_ASSERT(m_payload);
    auto& payload = static_cast<PayloadImpl<std::shared_ptr<Barq>>&>(*m_payload);
    BARQ_ASSERT(typeid(payload) == typeid(PayloadImpl<std::shared_ptr<Barq>>));

    return payload.get_barq();
}

template <typename T>
bool ThreadSafeReference::is() const
{
    return dynamic_cast<PayloadImpl<T>*>(m_payload.get()) != nullptr;
}

template Results ThreadSafeReference::resolve<Results>(std::shared_ptr<Barq> const&);
template List ThreadSafeReference::resolve<List>(std::shared_ptr<Barq> const&);
template object_store::Set ThreadSafeReference::resolve<object_store::Set>(std::shared_ptr<Barq> const&);
template OsDict ThreadSafeReference::resolve<OsDict>(std::shared_ptr<Barq> const&);
template Object ThreadSafeReference::resolve<Object>(std::shared_ptr<Barq> const&);

template bool ThreadSafeReference::is<std::shared_ptr<Barq>>() const;
template bool ThreadSafeReference::is<Results>() const;
template bool ThreadSafeReference::is<List>() const;
template bool ThreadSafeReference::is<object_store::Set>() const;
template bool ThreadSafeReference::is<OsDict>() const;
template bool ThreadSafeReference::is<Object>() const;

} // namespace barq
