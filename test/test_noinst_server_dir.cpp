#include "test.hpp"

#include <barq/util/file.hpp>
#include <barq/sync/noinst/server/server_dir.hpp>

using namespace barq;
using namespace barq::_impl;


TEST(ServerDir_InvalidVirtualPath)
{
    std::string root_path = "/root";

    std::string virt_paths[] = {
        "",
        "/",
        "//",
        "/.",
        "/..",
        "/abc/.",
        "/def/...",
        "/abc/.def",
        "/abc/",
        "/abc/",
        "/abc/+",
        "?abc",
        "/abc//def",
        "/abc/.def",
        "/abc+",
        "/db.barq",
        "/abc/db.barq.lock",
        "/abc/db.barq.management",
        " ",
        "/ abc",
        "/abc/*",
    };

    for (const std::string& virt_path : virt_paths) {
        VirtualPathComponents components = parse_virtual_path(root_path, virt_path);
        CHECK(!components.is_valid);
    }
}

#ifndef _WIN32
TEST(ServerDir_FullSyncPath)
{
    std::string root_paths[] = {
        "/root", "/root/123", "/abc/def/ghi123", "/root/",
        //"/root//"
        //"/root////"
    };

    std::string virt_paths[] = {"/a", "/a/b", "/a_-..", "/abc/123456789/0..../______/_/-/--/-.",
                                "/__.../__partial./__partial0"};

    for (const std::string& root_path : root_paths) {
        for (const std::string& virt_path : virt_paths) {
            VirtualPathComponents components = parse_virtual_path(root_path, virt_path);
            CHECK(components.is_valid);
            const std::string expected_real_path = util::File::resolve(virt_path.substr(1) + ".barq", root_path);
            CHECK_EQUAL(components.real_barq_path, expected_real_path);
            CHECK(!components.is_partial_view);
        }
    }
}
#endif
