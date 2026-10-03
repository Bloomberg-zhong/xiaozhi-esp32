#pragma once
#include <cstddef>
#include <optional>
#include <string>

enum class NetworkErrc {
    Unknown,
    InvalidArgument,
    NotInitialized,
    Timeout,
    ProtocolError,
    ReceiveFailed,
    TransmitFailed,
    ServerDisconnected
};
struct NetworkError {
    NetworkErrc code = NetworkErrc::Unknown;
    int native = 0;
    static NetworkError InvalidArgument(int n = 0) { return {NetworkErrc::InvalidArgument, n}; }
    static NetworkError NotInitialized(int n = 0) { return {NetworkErrc::NotInitialized, n}; }
    static NetworkError Timeout(int n = 0) { return {NetworkErrc::Timeout, n}; }
    static NetworkError ProtocolError(int n = 0) { return {NetworkErrc::ProtocolError, n}; }
    static NetworkError ReceiveFailed(int n = 0) { return {NetworkErrc::ReceiveFailed, n}; }
    static NetworkError TransmitFailed(int n = 0) { return {NetworkErrc::TransmitFailed, n}; }
    static NetworkError ServerDisconnected(int n = 0) {
        return {NetworkErrc::ServerDisconnected, n};
    }
    static NetworkError FromEsp(int n) { return {NetworkErrc::Unknown, n}; }
};
template <typename T = void>
class NetworkResult {
public:
    NetworkResult(T value) : value_(value) {}
    NetworkResult(NetworkError error) : error_(error) {}
    explicit operator bool() const { return value_.has_value(); }
    T operator*() const { return *value_; }
    const NetworkError& error() const { return error_; }

private:
    std::optional<T> value_;
    NetworkError error_;
};
template <>
class NetworkResult<void> {
public:
    NetworkResult() = default;
    NetworkResult(NetworkError error) : good_(false), error_(error) {}
    explicit operator bool() const { return good_; }
    const NetworkError& error() const { return error_; }

private:
    bool good_ = true;
    NetworkError error_;
};
class Http {
public:
    virtual ~Http() = default;
    virtual void SetTimeout(int) = 0;
    virtual void SetHeader(const std::string&, const std::string&) = 0;
    virtual void SetContent(std::string&&) = 0;
    virtual void SetKeepAlive(bool) = 0;
    virtual NetworkResult<> Open(const std::string&, const std::string&) = 0;
    virtual void Close() = 0;
    virtual NetworkResult<int> Read(char*, size_t) = 0;
    virtual NetworkResult<int> Write(const char*, size_t) = 0;
    virtual NetworkResult<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(const std::string&) const = 0;
    virtual size_t GetBodyLength() = 0;
    virtual std::string ReadAll() = 0;

protected:
    NetworkResult<> Fail(NetworkError error) {
        last_error_ = error;
        return error;
    }
    template <typename T>
    NetworkResult<T> FailValue(NetworkError error) {
        last_error_ = error;
        return error;
    }
    NetworkError last_error_;
};
