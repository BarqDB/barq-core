#include <barq/util/assert.hpp>
#include <barq/util/misc_ext_errors.hpp>

using namespace barq;

util::MiscExtErrorCategory util::misc_ext_error_category;


const char* util::MiscExtErrorCategory::name() const noexcept
{
    return "barq.util.misc_ext";
}


std::string util::MiscExtErrorCategory::message(int value) const
{
    switch (MiscExtErrors(value)) {
        case MiscExtErrors::end_of_input:
            return "End of input";
        case MiscExtErrors::premature_end_of_input:
            return "Premature end of input";
        case MiscExtErrors::delim_not_found:
            return "Delimiter not found";
        case MiscExtErrors::operation_not_supported:
            return "Operation not supported";
    }
    BARQ_ASSERT(false);
    return {};
}
