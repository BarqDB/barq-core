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

#ifndef BARQ_OS_COLLECTION_HPP
#define BARQ_OS_COLLECTION_HPP

#include <barq/collection.hpp>
#include <barq/object-store/property.hpp>
#include <barq/object-store/object.hpp>
#include <barq/object-store/util/copyable_atomic.hpp>
#include <barq/object-store/collection_notifications.hpp>
#include <barq/object-store/impl/collection_notifier.hpp>

namespace barq {
class Barq;
class Results;
class ObjectSchema;
class List;

namespace _impl {
class ListNotifier;
}

namespace object_store {
class Dictionary;
class Collection {
public:
    Collection(PropertyType type) noexcept;
    Collection(const Object& parent_obj, const Property* prop);
    Collection(std::shared_ptr<Barq> r, const Obj& parent_obj, ColKey col);
    Collection(std::shared_ptr<Barq> r, const CollectionBase& coll);
    Collection(std::shared_ptr<Barq> r, CollectionBasePtr coll);

    const std::shared_ptr<Barq>& get_barq() const
    {
        return m_barq;
    }
    // Get the type of the values contained in this List
    PropertyType get_type() const
    {
        return m_type;
    }

    virtual ~Collection();

    virtual Mixed get_any(size_t list_ndx) const = 0;
    virtual size_t find_any(Mixed value) const = 0;

    // Get the ObjectSchema of the values in this List
    // Only valid if get_type() returns PropertyType::Object
    const ObjectSchema& get_object_schema() const;

    ColKey get_parent_column_key() const;
    ObjKey get_parent_object_key() const;
    TableKey get_parent_table_key() const;

    size_t size() const;
    bool is_valid() const;
    void verify_attached() const;
    void verify_in_transaction() const;

    // Returns whether or not this Collection is frozen.
    bool is_frozen() const noexcept;

    // Return a Results representing a live view of this Collection.
    Results as_results() const;

    // Return a Results representing a snapshot of this Collection.
    Results snapshot() const;

    Results sort(SortDescriptor order) const;
    Results sort(std::vector<std::pair<std::string, bool>> const& keypaths) const;

    // Get the min/max/average/sum of the given column
    // All but sum() returns none when collection is empty, and sum() returns 0
    // Throws UnsupportedColumnTypeException for sum/average on timestamp or non-numeric column
    // Throws OutOfBoundsIndexException for an out-of-bounds column
    util::Optional<Mixed> max(ColKey column = {}) const;
    util::Optional<Mixed> min(ColKey column = {}) const;
    util::Optional<Mixed> average(ColKey column = {}) const;
    Mixed sum(ColKey column = {}) const;

    /**
     * Adds a `CollectionChangeCallback` to this `Collection`. The `CollectionChangeCallback` is exectuted when
     * insertions, modifications or deletions happen on this `Collection`.
     *
     * @param callback The function to execute when a insertions, modification or deletion in this `Collection` was
     * detected.
     * @param key_path_array A filter that can be applied to make sure the `CollectionChangeCallback` is only executed
     * when the property in the filter is changed but not otherwise.
     *
     * @return A `NotificationToken` that is used to identify this callback.
     */
    NotificationToken add_notification_callback(CollectionChangeCallback callback,
                                                std::optional<KeyPathArray> key_path_array = std::nullopt) &;

    const CollectionBase& get_impl() const
    {
        return *m_coll_base;
    }

    // nested collections
    void insert_collection(const PathElement&, CollectionType);
    void set_collection(const PathElement&, CollectionType);
    List get_list(const PathElement&) const;
    Dictionary get_dictionary(const PathElement&) const;

protected:
    std::shared_ptr<Barq> m_barq;
    PropertyType m_type;
    std::shared_ptr<CollectionBase> m_coll_base;
    mutable util::CopyableAtomic<const ObjectSchema*> m_object_schema = nullptr;
    _impl::CollectionNotifier::Handle<_impl::ListNotifier> m_notifier;
    bool m_is_embedded = false;

    Collection(const Collection&);
    Collection& operator=(const Collection&);
    Collection(Collection&&);
    Collection& operator=(Collection&&);

    void validate(const Obj&) const;

    template <typename T, typename Context>
    void validate_embedded(Context& ctx, T&& value, CreatePolicy policy) const;

    size_t hash() const noexcept;

    void record_audit_read(const Obj& obj) const;
    void record_audit_read(const Mixed& obj) const;

private:
    Collection(std::shared_ptr<Barq>&& r, CollectionBasePtr&& coll, PropertyType type);

    virtual const char* type_name() const noexcept = 0;
};

template <typename T, typename Context>
void Collection::validate_embedded(Context& ctx, T&& value, CreatePolicy policy) const
{
    if (!policy.copy && ctx.template unbox<Obj>(value, CreatePolicy::Skip).is_valid())
        throw IllegalOperation(util::format("Cannot add an existing managed embedded object to a %1.", type_name()));
}

} // namespace object_store
} // namespace barq

#endif /* BARQ_OS_COLLECTION_HPP */
