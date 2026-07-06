#include <barq/object-store/c_api/util.hpp>
#include <barq/object-store/c_api/types.hpp>

namespace barq::c_api {

inline WrapC* cast_ptr(void* ptr)
{
    auto rptr = static_cast<WrapC*>(ptr);
    BARQ_ASSERT(rptr->cookie == WrapC::s_cookie_value);
    return rptr;
}

inline const WrapC* cast_const_ptr(const void* ptr)
{
    auto rptr = static_cast<const WrapC*>(ptr);
    BARQ_ASSERT(rptr->cookie == WrapC::s_cookie_value);
    return rptr;
}

BARQ_API void barq_free(void* buffer)
{
    if (!buffer)
        return;
    free(buffer);
}

BARQ_API void barq_release(void* ptr)
{
    if (!ptr)
        return;
    delete cast_ptr(ptr);
}

BARQ_API void* barq_clone(const void* ptr)
{
    return wrap_err([=]() {
        return cast_const_ptr(ptr)->clone();
    });
}

BARQ_API bool barq_is_frozen(const void* ptr)
{
    return cast_const_ptr(ptr)->is_frozen();
}

BARQ_API bool barq_equals(const void* a, const void* b)
{
    if (a == b)
        return true;
    if (!a || !b)
        return false;

    auto lhs = static_cast<const WrapC*>(a);
    auto rhs = static_cast<const WrapC*>(b);

    return lhs->equals(*rhs);
}

BARQ_API barq_thread_safe_reference_t* barq_create_thread_safe_reference(const void* ptr)
{
    return wrap_err([=]() {
        auto cptr = static_cast<const WrapC*>(ptr);
        return cptr->get_thread_safe_reference();
    });
}

} // namespace barq::c_api
