#include "rlcd_http_client.h"

#include <esp_crt_bundle.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>

namespace {
constexpr size_t kMaxHeaderBytes = 16 * 1024;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool IsTimeout(int result, int socket_errno) {
    return result == -ESP_ERR_HTTP_EAGAIN || socket_errno == EAGAIN ||
           socket_errno == EWOULDBLOCK || socket_errno == ETIMEDOUT;
}
}  // namespace

RlcdHttpClient::~RlcdHttpClient() { Close(); }
void RlcdHttpClient::SetTimeout(int timeout_ms) { timeout_ms_ = std::max(timeout_ms, 1); }
void RlcdHttpClient::SetHeader(const std::string& key, const std::string& value) {
    request_headers_[Lower(key)] = value;
}
void RlcdHttpClient::SetContent(std::string&& content) { content_ = std::move(content); }
void RlcdHttpClient::SetKeepAlive(bool enable) { keep_alive_ = enable; }

esp_err_t RlcdHttpClient::OnEvent(esp_http_client_event_t* event) {
    auto* self = static_cast<RlcdHttpClient*>(event->user_data);
    if (!self)
        return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        const size_t bytes = std::strlen(event->header_key) + std::strlen(event->header_value);
        if (bytes > kMaxHeaderBytes - self->response_header_bytes_ ||
            self->response_headers_.size() >= 64) {
            self->header_overflow_ = true;
            return ESP_FAIL;
        }
        self->response_header_bytes_ += bytes;
        self->response_headers_[Lower(event->header_key)] = event->header_value;
    } else if (event->event_id == HTTP_EVENT_DISCONNECTED) {
        self->disconnected_ = true;
    }
    return ESP_OK;
}

NetworkResult<> RlcdHttpClient::Open(const std::string& method, const std::string& url) {
    Close();
    response_headers_.clear();
    response_header_bytes_ = received_ = 0;
    content_length_ = -1;
    status_ = 0;
    head_ = complete_ = disconnected_ = header_overflow_ = false;
    last_error_ = {};
    esp_http_client_method_t verb;
    if (method == "GET")
        verb = HTTP_METHOD_GET;
    else if (method == "HEAD")
        verb = HTTP_METHOD_HEAD;
    else if (method == "POST")
        verb = HTTP_METHOD_POST;
    else if (method == "PUT")
        verb = HTTP_METHOD_PUT;
    else
        return Fail(NetworkError::InvalidArgument());
    if ((url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) ||
        (content_ && content_->size() > INT_MAX))
        return Fail(NetworkError::InvalidArgument());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = verb;
    config.timeout_ms = timeout_ms_;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.event_handler = OnEvent;
    config.user_data = this;
    config.is_async = false;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    client_ = esp_http_client_init(&config);
    if (!client_)
        return Fail(NetworkError::NotInitialized());
    for (const auto& header : request_headers_) {
        auto result =
            esp_http_client_set_header(client_, header.first.c_str(), header.second.c_str());
        if (result != ESP_OK) {
            Close();
            return Fail(NetworkError::FromEsp(result));
        }
    }
    if (!request_headers_.count("connection"))
        esp_http_client_set_header(client_, "Connection", keep_alive_ ? "keep-alive" : "close");
    const int write_length = content_ ? static_cast<int>(content_->size()) : 0;
    auto result = esp_http_client_open(client_, write_length);
    if (result != ESP_OK) {
        Close();
        return Fail(NetworkError::FromEsp(result));
    }
    if (content_) {
        size_t written = 0;
        while (written < content_->size()) {
            int count = esp_http_client_write(client_, content_->data() + written,
                                              static_cast<int>(content_->size() - written));
            if (count <= 0) {
                Close();
                return Fail(NetworkError::TransmitFailed(count));
            }
            written += static_cast<size_t>(count);
        }
    }
    // Like the network Http API, settings are consumed by a successful send.
    request_headers_.clear();
    content_.reset();
    errno = 0;
    const int64_t headers = esp_http_client_fetch_headers(client_);
    const int header_errno = errno;
    if (headers < 0 || header_overflow_) {
        auto error = header_overflow_ ? NetworkError::ProtocolError()
                     : IsTimeout(static_cast<int>(headers), header_errno)
                         ? NetworkError::Timeout(static_cast<int>(headers))
                         : NetworkError::ReceiveFailed(static_cast<int>(headers));
        Close();
        return Fail(error);
    }
    status_ = esp_http_client_get_status_code(client_);
    if (status_ < 100 || status_ > 599) {
        Close();
        return Fail(NetworkError::ProtocolError());
    }
    content_length_ = esp_http_client_get_content_length(client_);
    if (content_length_ > 0 &&
        static_cast<uint64_t>(content_length_) > std::numeric_limits<size_t>::max()) {
        Close();
        return Fail(NetworkError::ProtocolError());
    }
    head_ = method == "HEAD" || status_ == 204 || status_ == 304;
    complete_ = head_;
    return {};
}

void RlcdHttpClient::Close() {
    if (!client_)
        return;
    auto client = client_;
    // Cleanup emits only synchronous events while this object is still alive.
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client_ = nullptr;
}

NetworkResult<int> RlcdHttpClient::Read(char* buffer, size_t buffer_size) {
    if (!client_)
        return FailValue<int>(NetworkError::NotInitialized());
    if (complete_)
        return 0;
    if (!buffer || buffer_size == 0)
        return FailValue<int>(NetworkError::InvalidArgument());
    const auto timeout = esp_http_client_set_timeout_ms(client_, timeout_ms_);
    if (timeout != ESP_OK)
        return FailValue<int>(NetworkError::FromEsp(timeout));
    errno = 0;
    const int count = esp_http_client_read(
        client_, buffer, static_cast<int>(std::min(buffer_size, size_t(INT_MAX))));
    const int socket_errno = errno;
    if (count > 0) {
        received_ += static_cast<size_t>(count);
        if (content_length_ >= 0 && received_ >= static_cast<uint64_t>(content_length_))
            complete_ = true;
        return count;
    }
    if (esp_http_client_is_complete_data_received(client_)) {
        complete_ = true;
        return 0;
    }
    if (IsTimeout(count, socket_errno) || (count == 0 && !disconnected_))
        return FailValue<int>(NetworkError::Timeout(count));
    return FailValue<int>(NetworkError::ServerDisconnected(count));
}

NetworkResult<int> RlcdHttpClient::Write(const char*, size_t) {
    // This adapter sends the complete request before fetching response headers.
    // Supply POST/PUT bodies with SetContent before Open.
    return FailValue<int>(NetworkError::InvalidArgument());
}
NetworkResult<int> RlcdHttpClient::GetStatusCode() {
    if (!client_ || !status_)
        return FailValue<int>(NetworkError::NotInitialized());
    return status_;
}
std::string RlcdHttpClient::GetResponseHeader(const std::string& key) const {
    const auto found = response_headers_.find(Lower(key));
    return found == response_headers_.end() ? "" : found->second;
}
size_t RlcdHttpClient::GetBodyLength() {
    return content_length_ > 0 ? static_cast<size_t>(content_length_) : 0;
}
std::string RlcdHttpClient::ReadAll() {
    std::string body, buffer(1024, '\0');
    while (true) {
        auto count = Read(buffer.data(), buffer.size());
        if (!count)
            return "";
        if (*count == 0)
            return body;
        body.append(buffer.data(), static_cast<size_t>(*count));
    }
}
