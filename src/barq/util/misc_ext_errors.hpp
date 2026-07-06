#ifndef BARQ_UTIL_MISC_EXT_ERRORS_HPP
#define BARQ_UTIL_MISC_EXT_ERRORS_HPP

#include <system_error>

namespace barq {
namespace util {

/// FIXME: The intention is that this enum will be merged into, and subsumed by
/// util::MiscErrors in `<barq/util/misc_errors.hpp>` in the core library.
enum class MiscExtErrors {
    /// End of input.
    end_of_input = 1,

    /// Premature end of input. That is, end of input at an unexpected, or
    /// illegal place in an input stream.
    premature_end_of_input,

    /// Delimiter not found.
    delim_not_found,

    /// Operation not supported
    operation_not_supported,
};

class MiscExtErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override final;
    std::string message(int) const override final;
};

/// The error category associated with MiscErrors. The name of this category is
/// `barq.util.misc_ext`.
extern MiscExtErrorCategory misc_ext_error_category;

inline std::error_code make_error_code(MiscExtErrors err)
{
    return std::error_code(int(err), misc_ext_error_category);
}

} // namespace util
} // namespace barq

namespace std {

template <>
class is_error_code_enum<barq::util::MiscExtErrors> {
public:
    static const bool value = true;
};

} // namespace std

#endif // BARQ_UTIL_MISC_EXT_ERRORS_HPP
