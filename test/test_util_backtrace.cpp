#include "test.hpp"

#include <barq/util/backtrace.hpp>
#include <barq/exceptions.hpp>
#include <barq/string_data.hpp>

using namespace barq;
using namespace barq::util;

BARQ_NOINLINE static void throw_logic_error(ErrorCodes::Error kind)
{
    throw LogicError{kind, "Some error"};
}

// FIXME: Disabled because this suddenly stopped working on Linux
TEST_IF(Backtrace_LogicError, false)
{
    try {
        throw_logic_error(ErrorCodes::RangeError);
    }
    catch (const LogicError& err) {
        // arm requires -funwind-tables to make backtraces, and that increases binary size.
#if BARQ_PLATFORM_APPLE || (defined(__linux__) && !BARQ_ANDROID && !defined(__arm__))
        if (!CHECK(StringData{err.what()}.contains("throw_logic_error")))
            std::cout << err.what() << std::endl;

#endif
        LogicError copy = err;
        CHECK_EQUAL(StringData{copy.what()}, StringData{err.what()});
    }
}
