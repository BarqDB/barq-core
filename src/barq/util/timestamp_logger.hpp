
#ifndef BARQ_UTIL_TIMESTAMP_LOGGER_HPP
#define BARQ_UTIL_TIMESTAMP_LOGGER_HPP

#include <barq/util/logger.hpp>
#include <barq/util/timestamp_formatter.hpp>


namespace barq {
namespace util {

class TimestampStderrLogger : public Logger {
public:
    using Precision = TimestampFormatter::Precision;
    using Config = TimestampFormatter::Config;

    explicit TimestampStderrLogger(Config = {}, Level = LogCategory::barq.get_default_level_threshold());

protected:
    void do_log(const LogCategory& category, Logger::Level, const std::string& message) final;

private:
    TimestampFormatter m_formatter;
};


} // namespace util
} // namespace barq

#endif // BARQ_UTIL_TIMESTAMP_LOGGER_HPP
