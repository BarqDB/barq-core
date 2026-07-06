#include <cstdlib>
#include <memory>

#include <barq/util/features.h>
#include <barq/util/assert.hpp>
#include <barq/util/demangle.hpp>
#include <barq/util/backtrace.hpp>

#if BARQ_HAVE_AT_LEAST_GCC(3, 2)
#define BARQ_HAVE_CXXABI_DEMANGLE
#include <cxxabi.h>
#endif


// See http://gcc.gnu.org/onlinedocs/libstdc++/latest-doxygen/namespaceabi.html
//
// FIXME: Could use the Autoconf macro 'ax_cxx_gcc_abi_demangle'. See
// http://autoconf-archive.cryp.to.
std::string barq::util::demangle(const std::string& mangled_name)
{
#ifdef BARQ_HAVE_CXXABI_DEMANGLE
    int status = 0;
    char* unmangled_name = abi::__cxa_demangle(mangled_name.c_str(), nullptr, nullptr, &status);
    switch (status) {
        case 0:
            BARQ_ASSERT(unmangled_name);
            goto demangled;
        case -1:
            BARQ_ASSERT(!unmangled_name);
            throw util::bad_alloc{};
    }
    BARQ_ASSERT(!unmangled_name);
    return mangled_name; // Throws
demangled:
    class Free {
    public:
        void operator()(char* p) const
        {
            std::free(p);
        }
    };
    std::unique_ptr<char[], Free> owner{unmangled_name};
    std::string demangled_name_2{unmangled_name}; // Throws
    return demangled_name_2;
#else
    return mangled_name; // Throws
#endif
}
