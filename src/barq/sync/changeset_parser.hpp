
#ifndef BARQ_SYNC_CHANGESET_PARSER_HPP
#define BARQ_SYNC_CHANGESET_PARSER_HPP

#include <barq/mixed.hpp>
#include <barq/sync/changeset.hpp>
#include <barq/util/input_stream.hpp>

namespace barq::sync {
void parse_changeset(util::InputStream&, Changeset& out_log);

// The server may send us primary keys of objects in json-encoded error messages as base64-encoded changeset payloads.
// This function takes such a base64-encoded payload and returns it parsed as an owned Mixed value. If it cannot
// be decoded, this throws a BadChangeset exception.
OwnedMixed parse_base64_encoded_primary_key(std::string_view str);
} // namespace barq::sync

#endif // BARQ_SYNC_CHANGESET_PARSER_HPP
