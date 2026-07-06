////////////////////////////////////////////////////////////////////////////
//
// Copyright 2021 Realm Inc.
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

#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>

namespace barq::c_api {
namespace {
using namespace barq::networking;

static_assert(barq_http_request_method_e(HttpMethod::get) == BARQ_HTTP_REQUEST_METHOD_GET);
static_assert(barq_http_request_method_e(HttpMethod::post) == BARQ_HTTP_REQUEST_METHOD_POST);
static_assert(barq_http_request_method_e(HttpMethod::patch) == BARQ_HTTP_REQUEST_METHOD_PATCH);
static_assert(barq_http_request_method_e(HttpMethod::put) == BARQ_HTTP_REQUEST_METHOD_PUT);
static_assert(barq_http_request_method_e(HttpMethod::del) == BARQ_HTTP_REQUEST_METHOD_DELETE);

class CNetworkTransport final : public GenericNetworkTransport {
public:
    CNetworkTransport(UserdataPtr userdata, barq_http_request_func_t request_executor)
        : m_userdata(std::move(userdata))
        , m_request_executor(request_executor)
    {
    }

    static void on_response_completed(void* request_context, const barq_http_response_t* response) noexcept
    {
        std::unique_ptr<util::UniqueFunction<void(const Response&)>> completion(
            static_cast<util::UniqueFunction<void(const Response&)>*>(request_context));

        HttpHeaders headers;
        for (size_t i = 0; i < response->num_headers; i++) {
            headers.emplace(response->headers[i].name, response->headers[i].value);
        }

        (*completion)({response->status_code, response->custom_status_code, std::move(headers),
                       std::string(response->body, response->body_size)});
    }

private:
    void send_request_to_server(const Request& request,
                                util::UniqueFunction<void(const Response&)>&& completion_block) final
    {
        auto completion_data =
            std::make_unique<util::UniqueFunction<void(const Response&)>>(std::move(completion_block));

        std::vector<barq_http_header_t> c_headers;
        c_headers.reserve(request.headers.size());
        for (auto&& header : request.headers) {
            c_headers.push_back({header.first.c_str(), header.second.c_str()});
        }

        barq_http_request_t c_request{barq_http_request_method_e(request.method),
                                       request.url.c_str(),
                                       request.timeout_ms,
                                       c_headers.data(),
                                       c_headers.size(),
                                       request.body.data(),
                                       request.body.size()};
        m_request_executor(m_userdata.get(), c_request, completion_data.release());
    }

    UserdataPtr m_userdata;
    barq_http_request_func_t m_request_executor;
};
} // namespace
} // namespace barq::c_api

BARQ_API barq_http_transport_t* barq_http_transport_new(barq_http_request_func_t request_executor,
                                                         barq_userdata_t userdata, barq_free_userdata_func_t free)
{
    auto transport = std::make_shared<barq::c_api::CNetworkTransport>(barq::c_api::UserdataPtr{userdata, free},
                                                                       request_executor);
    return new barq_http_transport_t(std::move(transport));
}

BARQ_API void barq_http_transport_complete_request(void* completion_data, const barq_http_response_t* response)
{
    barq::c_api::CNetworkTransport::on_response_completed(completion_data, response);
}
