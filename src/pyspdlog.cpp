#ifndef _WIN32
#define SPDLOG_ENABLE_SYSLOG
#endif

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/function.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/sinks/tcp_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/dup_filter_sink.h>
#include <spdlog/details/null_mutex.h>
#ifndef _WIN32
#include <spdlog/sinks/syslog_sink.h>
#endif

#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <spdlog/details/os.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <signal.h>
#endif

namespace spd = spdlog;
namespace nb = nanobind;

namespace { // Avoid cluttering the global namespace.

class Logger;

bool g_async_mode_on = false;
auto g_async_overflow_policy = spdlog::async_overflow_policy::block;

std::unordered_map<std::string, Logger*> g_loggers;
std::mutex mutex_loggers;

void register_logger(const std::string& name, Logger* logger)
{
    std::lock_guard<std::mutex> lck(mutex_loggers);
    g_loggers[name] = logger;
}

Logger* access_logger(const std::string& name)
{
    std::lock_guard<std::mutex> lck(mutex_loggers);
    return g_loggers[name];
}

void remove_logger(const std::string& name)
{
    std::lock_guard<std::mutex> lck(mutex_loggers);
    g_loggers[name] = nullptr;
    g_loggers.erase(name);
}

void remove_logger_all()
{
    std::lock_guard<std::mutex> lck(mutex_loggers);
    g_loggers.clear();
}

#ifndef _WIN32
struct sigaction g_previous_signal_actions[NSIG];
volatile sig_atomic_t g_has_previous_signal_action[NSIG] = { 0 };
std::once_flag g_install_signal_handlers_once;

void flush_all_loggers_noexcept()
{
    try {
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
            if (logger) {
                logger->flush();
            }
        });
        // Ensure async loggers drain before process exits.
        spdlog::shutdown();
    } catch (...) {
        // best effort
    }
}

void restore_default_and_reraise(int signum)
{
    struct sigaction default_action = {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    default_action.sa_flags = 0;
    sigaction(signum, &default_action, nullptr);
    raise(signum);
}

void flush_and_forward_signal(int signum)
{
    flush_all_loggers_noexcept();

    if (signum > 0 && signum < NSIG && g_has_previous_signal_action[signum]) {
        const struct sigaction& previous = g_previous_signal_actions[signum];
        if (previous.sa_handler == SIG_IGN) {
            return;
        }

        if (previous.sa_handler == SIG_DFL) {
            restore_default_and_reraise(signum);
            return;
        }

        if (previous.sa_flags & SA_SIGINFO) {
            if (previous.sa_sigaction) {
                previous.sa_sigaction(signum, nullptr, nullptr);
            }
        } else if (previous.sa_handler) {
            previous.sa_handler(signum);
        }
        return;
    }

    restore_default_and_reraise(signum);
}

void register_flush_handler_for_signal(int signum)
{
    struct sigaction current_action = {};
    if (sigaction(signum, nullptr, &current_action) != 0) {
        return;
    }

    if (current_action.sa_handler == SIG_IGN) {
        return;
    }

    if (current_action.sa_handler == flush_and_forward_signal) {
        return;
    }

    struct sigaction action = {};
    action.sa_handler = flush_and_forward_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(signum, &action, nullptr) == 0 && signum > 0 && signum < NSIG) {
        g_previous_signal_actions[signum] = current_action;
        g_has_previous_signal_action[signum] = 1;
    }
}

void install_flush_signal_handlers()
{
    std::call_once(g_install_signal_handlers_once, []() {
#ifdef SIGHUP
        register_flush_handler_for_signal(SIGHUP);
#endif
#ifdef SIGINT
        register_flush_handler_for_signal(SIGINT);
#endif
#ifdef SIGQUIT
        register_flush_handler_for_signal(SIGQUIT);
#endif
#ifdef SIGTERM
        register_flush_handler_for_signal(SIGTERM);
#endif
#ifdef SIGABRT
        register_flush_handler_for_signal(SIGABRT);
#endif
#ifdef SIGPIPE
        register_flush_handler_for_signal(SIGPIPE);
#endif
#ifdef SIGALRM
        register_flush_handler_for_signal(SIGALRM);
#endif
#ifdef SIGUSR1
        register_flush_handler_for_signal(SIGUSR1);
#endif
#ifdef SIGUSR2
        register_flush_handler_for_signal(SIGUSR2);
#endif
    });
}
#endif

class LogLevel {
public:
    const static int trace{ (int)spd::level::trace };
    const static int debug{ (int)spd::level::debug };
    const static int info{ (int)spd::level::info };
    const static int warn{ (int)spd::level::warn };
    const static int err{ (int)spd::level::err };
    const static int critical{ (int)spd::level::critical };
    const static int off{ (int)spd::level::off };
};

class Sink {
public:
    Sink() {}
    Sink(const spd::sink_ptr& sink)
        : _sink(sink)
    {
    }
    virtual ~Sink() {}
    virtual void log(const spd::details::log_msg& msg)
    {
        _sink->log(msg);
    }
    bool should_log(int msg_level) const
    {
        return _sink->should_log((spd::level::level_enum)msg_level);
    }
    void set_level(int log_level)
    {
        _sink->set_level((spd::level::level_enum)log_level);
    }
    int level() const
    {
        return (int)_sink->level();
    }

    spd::sink_ptr get_sink() const { return _sink; }

protected:
    spd::sink_ptr _sink{ nullptr };
};

// template <class sink_type>
// class generic_sink : public Sink {
// public:
//     generic_sink() {
//         _sink = std::make_shared<sink_type>();
//     }
// };

// class stdout_sink_st : public generic_sink<spdlog::sinks::stdout_sink_st> { };
// class stdout_sink_mt : public generic_sink<spdlog::sinks::stdout_sink_mt> { };

class stdout_sink_st : public Sink {
public:
    stdout_sink_st()
    {
        _sink = std::make_shared<spdlog::sinks::stdout_sink_st>();
    }
};

class stdout_sink_mt : public Sink {
public:
    stdout_sink_mt()
    {
        _sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
    }
};

class stdout_color_sink_st : public Sink {
public:
    stdout_color_sink_st()
    {
        _sink = std::make_shared<spdlog::sinks::stdout_color_sink_st>();
    }
};

class stdout_color_sink_mt : public Sink {
public:
    stdout_color_sink_mt()
    {
        _sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }
};

class stderr_sink_st : public Sink {
public:
    stderr_sink_st()
    {
        _sink = std::make_shared<spdlog::sinks::stderr_sink_st>();
    }
};

class stderr_sink_mt : public Sink {
public:
    stderr_sink_mt()
    {
        _sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    }
};

class stderr_color_sink_st : public Sink {
public:
    stderr_color_sink_st()
    {
        _sink = std::make_shared<spdlog::sinks::stderr_color_sink_st>();
    }
};

class stderr_color_sink_mt : public Sink {
public:
    stderr_color_sink_mt()
    {
        _sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    }
};

class basic_file_sink_st : public Sink {
public:
    basic_file_sink_st(const std::string& base_filename, bool truncate)
    {
        _sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(base_filename, truncate);
    }
};

class basic_file_sink_mt : public Sink {
public:
    basic_file_sink_mt(const std::string& base_filename, bool truncate)
    {
        _sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(base_filename, truncate);
    }
};

class daily_file_sink_mt : public Sink {
public:
    daily_file_sink_mt(const std::string& base_filename, int rotation_hour, int rotation_minute)
    {
        _sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(base_filename, rotation_hour, rotation_minute);
    }
};

class daily_file_sink_st : public Sink {
public:
    daily_file_sink_st(const std::string& base_filename, int rotation_hour, int rotation_minute)
    {
        _sink = std::make_shared<spdlog::sinks::daily_file_sink_st>(base_filename, rotation_hour, rotation_minute);
    }
};

class rotating_file_sink_mt : public Sink {
public:
    rotating_file_sink_mt(const std::string& filename, size_t max_file_size, size_t max_files)
    {
        _sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filename, max_file_size, max_files);
    }
};

class rotating_file_sink_st : public Sink {
public:
    rotating_file_sink_st(const std::string& filename, size_t max_file_size, size_t max_files)
    {
        _sink = std::make_shared<spdlog::sinks::rotating_file_sink_st>(filename, max_file_size, max_files);
    }
};

template<typename Mutex>
class dist_sink: public Sink {
public:
    dist_sink() { _sink = std::make_shared<spdlog::sinks::dist_sink<Mutex>>();}
    dist_sink(std::vector<Sink> sinks)
    {
        std::vector<spd::sink_ptr> sink_vec;
        for (uint i =0; i<sinks.size(); i++)
        {
            sink_vec.push_back(sinks.at(i).get_sink());
        }
        _sink = std::make_shared<spdlog::sinks::dist_sink<Mutex>>(sink_vec);
    }
    void add_sink(const Sink& sink)
    {
        std::dynamic_pointer_cast<spdlog::sinks::dist_sink<Mutex>>(_sink)->add_sink(sink.get_sink());
    }

    void remove_sink(const Sink& sink)
    {
        std::dynamic_pointer_cast<spdlog::sinks::dist_sink<Mutex>>(_sink)->remove_sink(sink.get_sink());
    }

    void set_sinks(std::vector<Sink> sinks)
    {
        std::vector<spd::sink_ptr> sink_vec;
        for (uint i =0; i<sinks.size(); i++)
        {
            sink_vec.push_back(sinks.at(i).get_sink());
        }
        std::dynamic_pointer_cast<spdlog::sinks::dist_sink<Mutex>>(_sink)->set_sinks(sink_vec);
    }

    std::vector<spd::sink_ptr> &sinks()
    {
        return std::dynamic_pointer_cast<spdlog::sinks::dist_sink<Mutex>>(_sink)->sinks();
    }  
};

using dist_sink_mt = dist_sink<std::mutex>;
using dist_sink_st = dist_sink<spd::details::null_mutex>;

class dup_filter_sink_mt: public dist_sink_mt {
public:
    dup_filter_sink_mt(float max_skip_duration_sec)
    {
        _sink = std::make_shared<spdlog::sinks::dup_filter_sink_mt>(std::chrono::milliseconds((int)(max_skip_duration_sec*1000.0)));
    }
};

class dup_filter_sink_st: public dist_sink_st {
public:
    dup_filter_sink_st(float max_skip_duration_sec)
    {
        _sink = std::make_shared<spdlog::sinks::dup_filter_sink_st>(std::chrono::milliseconds((int)(max_skip_duration_sec*1000.0)));
    }
};

class null_sink_st : public Sink {
public:
    null_sink_st()
    {
        _sink = std::make_shared<spdlog::sinks::null_sink_st>();
    }
};

class null_sink_mt : public Sink {
public:
    null_sink_mt()
    {
        _sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    }
};

class tcp_sink_st : public Sink {
public:
    tcp_sink_st(std::string server_host, int server_port, bool lazy_connect)
    {
        struct spdlog::sinks::tcp_sink_config tcp_config(server_host, server_port);
        tcp_config.lazy_connect = lazy_connect;

        _sink = std::make_shared<spdlog::sinks::tcp_sink_st>(tcp_config);
    }
};

class tcp_sink_mt : public Sink {
public:
    tcp_sink_mt(std::string server_host, int server_port, bool lazy_connect)
    {
        struct spdlog::sinks::tcp_sink_config tcp_config(server_host, server_port);
        tcp_config.lazy_connect = lazy_connect;

        _sink = std::make_shared<spdlog::sinks::tcp_sink_mt>(tcp_config);
    }
};

#ifdef SPDLOG_ENABLE_SYSLOG
class syslog_sink_st : public Sink {
public:
    syslog_sink_st(const std::string& ident = "", int syslog_option = 0, int syslog_facility = (1 << 3), bool enable_formatting = true)
    {
        _sink = std::make_shared<spdlog::sinks::syslog_sink_st>(ident, syslog_option, syslog_facility, enable_formatting);
    }
};

class syslog_sink_mt : public Sink {
public:
    syslog_sink_mt(const std::string& ident = "", int syslog_option = 0, int syslog_facility = (1 << 3), bool enable_formatting = true)
    {
        _sink = std::make_shared<spdlog::sinks::syslog_sink_mt>(ident, syslog_option, syslog_facility, enable_formatting);
    }
};
#endif

// "%H:%M:%S.%e,%v" 전용 컴파일드 포맷터.
// 일반 pattern_formatter는 패턴 항목(H,:,M,:,S,.,e,,,v)마다 가상호출을 하지만,
// 이 포맷터는 초가 바뀔 때만 "HH:MM:SS." 접두사를 재계산해 캐시하고
// 매 메시지에는 ms 3자리 + ',' + payload + '\n'만 기록한다. 출력은 완전 동일.
class csv_time_formatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        using namespace std::chrono;
        const auto tp = msg.time.time_since_epoch();
        const auto secs = duration_cast<seconds>(tp);
        if (secs != cached_secs_) {
            const std::time_t tt = system_clock::to_time_t(
                time_point<system_clock, system_clock::duration>(
                    duration_cast<system_clock::duration>(tp)));
            const std::tm tm = spdlog::details::os::localtime(tt);
            prefix_[0] = static_cast<char>('0' + tm.tm_hour / 10);
            prefix_[1] = static_cast<char>('0' + tm.tm_hour % 10);
            prefix_[2] = ':';
            prefix_[3] = static_cast<char>('0' + tm.tm_min / 10);
            prefix_[4] = static_cast<char>('0' + tm.tm_min % 10);
            prefix_[5] = ':';
            prefix_[6] = static_cast<char>('0' + tm.tm_sec / 10);
            prefix_[7] = static_cast<char>('0' + tm.tm_sec % 10);
            prefix_[8] = '.';
            cached_secs_ = secs;
        }
        dest.append(prefix_, prefix_ + 9);
        const auto ms = duration_cast<milliseconds>(tp - secs).count();
        char tail[4] = {static_cast<char>('0' + ms / 100),
                        static_cast<char>('0' + (ms / 10) % 10),
                        static_cast<char>('0' + ms % 10), ','};
        dest.append(tail, tail + 4);
        dest.append(msg.payload.begin(), msg.payload.end());
        dest.push_back('\n');
    }
    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<csv_time_formatter>();
    }
private:
    std::chrono::seconds cached_secs_{-1};
    char prefix_[9] = {};
};

class Logger {
public:
    using async_factory_nb = spdlog::async_factory_impl<spdlog::async_overflow_policy::overrun_oldest>;

    Logger(const std::string& name, bool async_mode)
        : _name(name)
        , _async(async_mode)
    {
        register_logger(name, this);
    }

    virtual ~Logger() {}
    std::string name() const
    {
        if (_logger)
            return _logger->name();
        else
            return "NULL";
    }
    // hot path: std::string_view 인자는 nanobind가 PyUnicode_AsUTF8AndSize로 파이썬 str의
    // 내부(캐시된) UTF-8 버퍼를 직접 참조하므로 호출당 힙 할당/복사가 없다.
    // (std::string 인자는 매 호출 malloc+memcpy를 유발)
    static spd::string_view_t sv(std::string_view msg) { return spd::string_view_t(msg.data(), msg.size()); }
    void log(int level, std::string_view msg) const { this->_logger->log((spd::level::level_enum)level, sv(msg)); }
    void trace(std::string_view msg) const { this->_logger->trace(sv(msg)); }
    void debug(std::string_view msg) const { this->_logger->debug(sv(msg)); }
    void info(std::string_view msg) const { this->_logger->info(sv(msg)); }
    void warn(std::string_view msg) const { this->_logger->warn(sv(msg)); }
    void error(std::string_view msg) const { this->_logger->error(sv(msg)); }
    void critical(std::string_view msg) const { this->_logger->critical(sv(msg)); }

    // ---- CSV 직렬화 핫패스 (파이썬 f-string 생성 비용을 C++로 내림) ----
    // set_csv_schema("i,i,f3,f5")처럼 필드 타입을 1회 등록해두면, log_csv(*args)가
    // 파이썬 f-string(f"{a},{b},{c:.3f},{d:.5f}")과 바이트 단위로 동일한 라인을 만든다.
    // 토큰: "i" = 정수, "f<N>" = 고정소수점 N자리 (파이썬 {x:.Nf}와 동일).
    // 반올림 동일성: std::to_chars(fixed)와 파이썬 포맷 모두 이진 double의 정확
    // 반올림(correctly rounded)이라 모든 값에서 일치. NaN만 파이썬이 부호를 무시한
    // "nan"을 쓰므로 명시 처리한다. "i" 필드에 int가 아닌 값이 오면 str(obj) 폴백.
    struct CsvField { char kind; int prec; };
    std::vector<CsvField> _csv_schema;

    void set_csv_schema(const std::string& spec)
    {
        std::vector<CsvField> schema;
        size_t pos = 0;
        while (pos <= spec.size()) {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos) end = spec.size();
            const std::string tok = spec.substr(pos, end - pos);
            if (tok == "i") {
                schema.push_back({'i', 0});
            } else if (tok.size() >= 2 && tok[0] == 'f') {
                int prec = 0;
                auto r = std::from_chars(tok.data() + 1, tok.data() + tok.size(), prec);
                if (r.ec != std::errc() || r.ptr != tok.data() + tok.size() || prec < 0 || prec > 17)
                    throw std::invalid_argument("set_csv_schema: bad token: " + tok);
                schema.push_back({'f', prec});
            } else {
                throw std::invalid_argument("set_csv_schema: bad token: " + tok);
            }
            pos = end + 1;
        }
        _csv_schema = std::move(schema);
    }

    size_t csv_serialize(PyObject* tup, char* buf, char* bufend) const
    {
        const size_t n = _csv_schema.size();
        if (n == 0)
            throw std::runtime_error("log_csv: call set_csv_schema() first");
        if ((size_t)PyTuple_GET_SIZE(tup) != n)
            throw std::invalid_argument("log_csv: argument count does not match schema");
        char* p = buf;
        for (size_t i = 0; i < n; ++i) {
            if (i) *p++ = ',';
            PyObject* obj = PyTuple_GET_ITEM(tup, i);
            const CsvField f = _csv_schema[i];
            if (f.kind == 'f') {
                const double d = PyFloat_AsDouble(obj);
                if (d == -1.0 && PyErr_Occurred())
                    throw nb::python_error();
                if (std::isnan(d)) {  // 파이썬은 -nan도 "nan"
                    std::memcpy(p, "nan", 3);
                    p += 3;
                } else {
                    const auto r = std::to_chars(p, bufend, d, std::chars_format::fixed, f.prec);
                    if (r.ec != std::errc())
                        throw std::runtime_error("log_csv: line buffer overflow");
                    p = r.ptr;
                }
            } else {
                bool done = false;
                if (PyLong_CheckExact(obj)) {
                    int overflow = 0;
                    const long long v = PyLong_AsLongLongAndOverflow(obj, &overflow);
                    if (overflow == 0) {
                        if (v == -1 && PyErr_Occurred())
                            throw nb::python_error();
                        const auto r = std::to_chars(p, bufend, v);
                        if (r.ec != std::errc())
                            throw std::runtime_error("log_csv: line buffer overflow");
                        p = r.ptr;
                        done = true;
                    }
                    // overflow: int64 초과 정수는 아래 str() 폴백으로 (f-string과 동일 출력)
                }
                if (!done) {
                // 드문 경로: int64 정수가 아니면 str(obj)로 f-string의 {obj}와 바이트 동일 보장
                PyObject* s = PyObject_Str(obj);
                if (!s)
                    throw nb::python_error();
                Py_ssize_t len = 0;
                const char* u = PyUnicode_AsUTF8AndSize(s, &len);
                if (!u) {
                    Py_DECREF(s);
                    throw nb::python_error();
                }
                if (p + len > bufend) {
                    Py_DECREF(s);
                    throw std::runtime_error("log_csv: line buffer overflow");
                }
                std::memcpy(p, u, (size_t)len);
                p += len;
                Py_DECREF(s);
                }
            }
        }
        return (size_t)(p - buf);
    }

    void log_csv(nb::args args) const
    {
        char buf[4096];  // f17 x 최대 필드수에도 충분 (double fixed 최장 ~335자)
        const size_t len = csv_serialize(args.ptr(), buf, buf + sizeof buf);
        this->_logger->info(spd::string_view_t(buf, len));
    }

    std::string format_csv(nb::args args) const
    {
        // 검증용: log_csv가 출력할 본문(%v)을 로깅 없이 반환
        char buf[4096];
        const size_t len = csv_serialize(args.ptr(), buf, buf + sizeof buf);
        return std::string(buf, len);
    }

    bool should_log(int level) const
    {
        return _logger->should_log((spd::level::level_enum)level);
    }

    void set_level(int level)
    {
        _logger->set_level((spd::level::level_enum)level);
    }

    int level() const
    {
        return (int)_logger->level();
    }

    void set_pattern(const std::string& pattern, spd::pattern_time_type type = spd::pattern_time_type::local)
    {
        // 매매 시스템의 실전 패턴은 전용 포맷터로 자동 대체 (출력 동일, 포맷팅 비용 절감)
        if (pattern == "%H:%M:%S.%e,%v" && type == spd::pattern_time_type::local) {
            _logger->set_formatter(std::make_unique<csv_time_formatter>());
        } else {
            _logger->set_pattern(pattern, type);
        }
    }

    // automatically call flush() if message level >= log_level
    void flush_on(int log_level)
    {
        _logger->flush_on((spd::level::level_enum)log_level);
    }

    void flush()
    {
        _logger->flush();
    }

    bool async()
    {
        return _async;
    }

    void close()
    {
        remove_logger(_name);
        _logger = nullptr;
        spdlog::drop(_name);
    }

    std::vector<Sink> sinks() const
    {
        std::vector<Sink> snks;
        for (const spd::sink_ptr& sink : _logger->sinks()) {
            snks.push_back(Sink(sink));
        }
        return snks;
    }

    void set_error_handler(spd::err_handler handler)
    {
        _logger->set_error_handler(handler);
    }

    std::shared_ptr<spdlog::logger> get_underlying_logger() {
        return _logger;
    }

protected:
    const std::string _name;
    bool _async;
    std::shared_ptr<spdlog::logger> _logger{ nullptr };
};

class ConsoleLogger : public Logger {
public:
    ConsoleLogger(const std::string& logger_name, bool multithreaded, bool standard_out, bool colored, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (standard_out) {
            if (multithreaded) {
                if (colored) {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stdout_color_mt<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stdout_color_mt<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stdout_color_mt(logger_name);
                    }
                } else {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stdout_logger_mt<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stdout_logger_mt<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stdout_logger_mt(logger_name);
                    }
                }
            } else {
                if (colored) {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stdout_color_st<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stdout_color_st<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stdout_color_st(logger_name);
                    }
                } else {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stdout_logger_st<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stdout_logger_st<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stdout_logger_st(logger_name);
                    }
                }
            }

        } else {
            if (multithreaded) {
                if (colored) {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stderr_color_mt<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stderr_color_mt<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stderr_color_mt(logger_name);
                    }
                } else {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stderr_logger_mt<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stderr_logger_mt<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stderr_logger_mt(logger_name);
                    }
                }
            } else {
                if (colored) {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stderr_color_st<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stderr_color_st<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stderr_color_st(logger_name);
                    }
                } else {
                    if (async_mode) {
                        if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                            _logger = spd::stderr_logger_st<async_factory_nb>(logger_name);
                        } else {
                            _logger = spd::stderr_logger_st<spdlog::async_factory>(logger_name);
                        }
                    } else {
                        _logger = spd::stderr_logger_st(logger_name);
                    }
                }
            }
        }
    }
};

class FileLogger : public Logger {
public:
    FileLogger(const std::string& logger_name, const std::string& filename, bool multithreaded, bool truncate = false, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (multithreaded) {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::basic_logger_mt<async_factory_nb>(logger_name, filename, truncate);
                } else {
                    _logger = spd::basic_logger_mt<spdlog::async_factory>(logger_name, filename, truncate);
                }
            } else {
                _logger = spd::basic_logger_mt(logger_name, filename, truncate);
            }
        } else {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::basic_logger_st<async_factory_nb>(logger_name, filename, truncate);
                } else {
                    _logger = spd::basic_logger_st<spdlog::async_factory>(logger_name, filename, truncate);
                }
            } else {
                _logger = spd::basic_logger_st(logger_name, filename, truncate);
            }
        }
    }
};

class RotatingLogger : public Logger {
public:
    RotatingLogger(const std::string& logger_name, const std::string& filename, bool multithreaded, size_t max_file_size, size_t max_files, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (multithreaded) {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::rotating_logger_mt<async_factory_nb>(logger_name, filename, max_file_size, max_files);
                } else {
                    _logger = spd::rotating_logger_mt<spdlog::async_factory>(logger_name, filename, max_file_size, max_files);
                }
            } else {
                _logger = spd::rotating_logger_mt(logger_name, filename, max_file_size, max_files);
            }
        } else {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::rotating_logger_st<async_factory_nb>(logger_name, filename, max_file_size, max_files);
                } else {
                    _logger = spd::rotating_logger_st<spdlog::async_factory>(logger_name, filename, max_file_size, max_files);
                }
            } else {
                _logger = spd::rotating_logger_st(logger_name, filename, max_file_size, max_files);
            }
        }
    }
};

class DailyLogger : public Logger {
public:
    DailyLogger(const std::string& logger_name, const std::string& filename, bool multithreaded = false, int hour = 0, int minute = 0, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (multithreaded) {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::daily_logger_mt<async_factory_nb>(logger_name, filename, hour, minute);
                } else {
                    _logger = spd::daily_logger_mt<spdlog::async_factory>(logger_name, filename, hour, minute);
                }
            } else {
                _logger = spd::daily_logger_mt(logger_name, filename, hour, minute);
            }
        } else {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::daily_logger_st<async_factory_nb>(logger_name, filename, hour, minute);
                } else {
                    _logger = spd::daily_logger_st<spdlog::async_factory>(logger_name, filename, hour, minute);
                }
            } else {
                _logger = spd::daily_logger_st(logger_name, filename, hour, minute);
            }
        }
    }
};

#ifdef SPDLOG_ENABLE_SYSLOG
class SyslogLogger : public Logger {
public:
    SyslogLogger(const std::string& logger_name, bool multithreaded = false, const std::string& ident = "", int syslog_option = 0, int syslog_facilty = (1 << 3), bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (multithreaded) {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::syslog_logger_mt<async_factory_nb>(logger_name, ident, syslog_option, syslog_facilty);
                } else {
                    _logger = spd::syslog_logger_mt<spdlog::async_factory>(logger_name, ident, syslog_option, syslog_facilty);
                }
            } else {
                _logger = spd::syslog_logger_mt(logger_name, ident, syslog_option, syslog_facilty);
            }
        } else {
            if (async_mode) {
                if (g_async_overflow_policy == spdlog::async_overflow_policy::overrun_oldest) {
                    _logger = spd::syslog_logger_st<async_factory_nb>(logger_name, ident, syslog_option, syslog_facilty);
                } else {
                    _logger = spd::syslog_logger_st<spdlog::async_factory>(logger_name, ident, syslog_option, syslog_facilty);
                }
            } else {
                _logger = spd::syslog_logger_st(logger_name, ident, syslog_option, syslog_facilty);
            }
        }
    }
};
#endif

class AsyncOverflowPolicy {
public:
    const static int block{ (int)spd::async_overflow_policy::block };
    const static int overrun_oldest{ (int)spd::async_overflow_policy::overrun_oldest };
};

void set_async_mode(size_t queue_size = spdlog::details::default_async_q_size, size_t thread_count = 1, int async_overflow_policy = AsyncOverflowPolicy::block) {
    // Initialize/replace the global spdlog thread pool.
    auto& registry = spdlog::details::registry::instance();
    std::lock_guard<std::recursive_mutex> tp_lck(registry.tp_mutex());
    auto tp = std::make_shared<spd::details::thread_pool>(queue_size, thread_count);
    registry.set_tp(tp);

    g_async_overflow_policy = static_cast<spd::async_overflow_policy>(async_overflow_policy);
    g_async_mode_on = true;
}

std::shared_ptr<spdlog::details::thread_pool> thread_pool() {
    auto& registry = spdlog::details::registry::instance();
    std::lock_guard<std::recursive_mutex> tp_lck(registry.tp_mutex());
    auto tp = registry.get_tp();
    if(tp == nullptr) {
        set_async_mode();
        auto tp = registry.get_tp();
    }

    return tp;
}

class SinkLogger : public Logger {
public:
    SinkLogger(const std::string& logger_name, const Sink& sink, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        if (async_mode) {
            _logger = std::shared_ptr<spd::async_logger>(new spd::async_logger(logger_name, sink.get_sink(), thread_pool(), g_async_overflow_policy));
        } else {
            _logger = std::shared_ptr<spd::logger>(new spd::logger(logger_name, sink.get_sink()));
        }
    }
    SinkLogger(const std::string& logger_name, const std::vector<Sink>& sink_list, bool async_mode = g_async_mode_on)
        : Logger(logger_name, async_mode)
    {
        std::vector<spd::sink_ptr> sinks;
        for (auto sink : sink_list)
            sinks.push_back(sink.get_sink());

        if (async_mode) {
            _logger = std::shared_ptr<spd::async_logger>(new spd::async_logger(logger_name, sinks.begin(), sinks.end(), thread_pool(), g_async_overflow_policy));
        } else {
            _logger = std::shared_ptr<spd::logger>(new spd::logger(logger_name, sinks.begin(), sinks.end()));
        }
    }
};

Logger get(const std::string& name)
{
    Logger* logger = access_logger(name);
    if (logger)
        return *logger;
    else
        throw std::runtime_error(std::string("Logger name: " + name + " could not be found"));
}

void drop(const std::string& name)
{
    remove_logger(name);
    spdlog::drop(name);
}

void drop_all()
{
    remove_logger_all();
    spdlog::drop_all();
}

}

NB_MODULE(spdlog_swyang, m)
{
#ifndef _WIN32
    install_flush_signal_handlers();
#endif

    m.doc() = R"pbdoc(
        spdlog_swyang module
        -----------------------

        .. currentmodule:: spdlog_swyang

        .. autosummary::
           :toctree: _generate

           LogLevel
           Logger
    )pbdoc";

    // nanobind는 holder 템플릿 인자가 없음 — shared_ptr는 stl/shared_ptr.h 캐스터가 처리
    nb::class_<spd::logger>(m, "_spd_logger");

    m.def("set_async_mode", set_async_mode,
        nb::arg("queue_size") = 1 << 16,
        nb::arg("thread_count") = 1,
        nb::arg("overflow_policy") = 0);

    nb::class_<Sink>(m, "Sink")
        .def(nb::init<>())
        .def("set_level", &Sink::set_level);

    nb::class_<stdout_sink_st, Sink>(m, "stdout_sink_st")
        .def(nb::init<>());

    nb::class_<stdout_sink_mt, Sink>(m, "stdout_sink_mt")
        .def(nb::init<>());

    nb::class_<stdout_color_sink_st, Sink>(m, "stdout_color_sink_st")
        .def(nb::init<>());

    nb::class_<stdout_color_sink_mt, Sink>(m, "stdout_color_sink_mt")
        .def(nb::init<>());

    nb::class_<stderr_sink_st, Sink>(m, "stderr_sink_st")
        .def(nb::init<>());

    nb::class_<stderr_sink_mt, Sink>(m, "stderr_sink_mt")
        .def(nb::init<>());

    nb::class_<stderr_color_sink_st, Sink>(m, "stderr_color_sink_st")
        .def(nb::init<>());

    nb::class_<stderr_color_sink_mt, Sink>(m, "stderr_color_sink_mt")
        .def(nb::init<>());

    nb::class_<basic_file_sink_st, Sink>(m, "basic_file_sink_st")
        .def(nb::init<std::string, bool>(), nb::arg("filename"), nb::arg("truncate") = false);

    nb::class_<basic_file_sink_mt, Sink>(m, "basic_file_sink_mt")
        .def(nb::init<std::string, bool>(), nb::arg("filename"), nb::arg("truncate") = false);

    nb::class_<daily_file_sink_st, Sink>(m, "daily_file_sink_st")
        .def(nb::init<std::string, int, int>(), nb::arg("filename"),
            nb::arg("rotation_hour"),
            nb::arg("rotation_minute"));

    nb::class_<daily_file_sink_mt, Sink>(m, "daily_file_sink_mt")
        .def(nb::init<std::string, int, int>(), nb::arg("filename"),
            nb::arg("rotation_hour"),
            nb::arg("rotation_minute"));

    nb::class_<rotating_file_sink_st, Sink>(m, "rotating_file_sink_st")
        .def(nb::init<std::string, int, int>(), nb::arg("filename"),
            nb::arg("max_size"),
            nb::arg("max_files"));

    nb::class_<rotating_file_sink_mt, Sink>(m, "rotating_file_sink_mt")
        .def(nb::init<std::string, int, int>(), nb::arg("filename"),
            nb::arg("max_size"),
            nb::arg("max_files"));

    nb::class_<dist_sink_mt, Sink>(m, "dist_sink_mt")
        .def(nb::init<>())
        .def(nb::init<std::vector<Sink>>(), nb::arg("sinks"))
        .def("add_sink", &dist_sink_mt::add_sink, nb::arg("sink"))
        .def("remove_sink", &dist_sink_mt::remove_sink, nb::arg("sink"))
        .def("set_sinks", &dist_sink_mt::set_sinks, nb::arg("sinks"))
        .def("sinks", &dist_sink_mt::sinks);

    nb::class_<dist_sink_st, Sink>(m, "dist_sink_st")
        .def(nb::init<>())
        .def(nb::init<std::vector<Sink>>(), nb::arg("sinks"))
        .def("add_sink", &dist_sink_st::add_sink, nb::arg("sink"))
        .def("remove_sink", &dist_sink_st::remove_sink, nb::arg("sink"))
        .def("set_sinks", &dist_sink_st::set_sinks, nb::arg("sinks"))
        .def("sinks", &dist_sink_st::sinks);

    nb::class_<dup_filter_sink_st, dist_sink_st>(m, "dup_filter_sink_st")
        .def(nb::init<float>(), nb::arg("max_skip_duration_seconds"));

    nb::class_<dup_filter_sink_mt, dist_sink_mt>(m, "dup_filter_sink_mt")
        .def(nb::init<float>(), nb::arg("max_skip_duration_seconds"));

    nb::class_<null_sink_st, Sink>(m, "null_sink_st")
        .def(nb::init<>());

    nb::class_<null_sink_mt, Sink>(m, "null_sink_mt")
        .def(nb::init<>());

    nb::class_<tcp_sink_st, Sink>(m, "tcp_sink_st")
        .def(nb::init<std::string, int, bool>(),
             nb::arg("server_host"),
             nb::arg("server_port"),
             nb::arg("lazy_connect"));

    nb::class_<tcp_sink_mt, Sink>(m, "tcp_sink_mt")
        .def(nb::init<std::string, int, bool>(),
             nb::arg("server_host"),
             nb::arg("server_port"),
             nb::arg("lazy_connect"));

    nb::class_<LogLevel>(m, "LogLevel")
        .def_prop_ro_static("TRACE", [](nb::object) { return LogLevel::trace; })
        .def_prop_ro_static("DEBUG", [](nb::object) { return LogLevel::debug; })
        .def_prop_ro_static("INFO", [](nb::object) { return LogLevel::info; })
        .def_prop_ro_static("WARN", [](nb::object) { return LogLevel::warn; })
        .def_prop_ro_static("ERR", [](nb::object) { return LogLevel::err; })
        .def_prop_ro_static("CRITICAL", [](nb::object) { return LogLevel::critical; })
        .def_prop_ro_static("OFF", [](nb::object) { return LogLevel::off; });

    nb::class_<AsyncOverflowPolicy>(m, "AsyncOverflowPolicy")
        .def_prop_ro_static("BLOCK", [](nb::object) { return AsyncOverflowPolicy::block; })
        .def_prop_ro_static("OVERRUN_OLDEST", [](nb::object) { return AsyncOverflowPolicy::overrun_oldest; });

    nb::enum_<spdlog::pattern_time_type>(m, "PatternTimeType")
        .value("local", spdlog::pattern_time_type::local)
        .value("utc", spdlog::pattern_time_type::utc)
        .export_values();

    nb::class_<Logger>(m, "Logger")
        .def("log", &Logger::log)
        .def("trace", &Logger::trace)
        .def("debug", &Logger::debug)
        .def("info", &Logger::info)
        .def("warn", &Logger::warn)
        .def("error", &Logger::error)
        .def("critical", &Logger::critical)
        .def("name", &Logger::name)
        .def("set_csv_schema", &Logger::set_csv_schema,
            "CSV 필드 스키마 등록. 예: \"i,i,f3,f5\" (i=정수, fN=고정소수 N자리)")
        .def("log_csv", &Logger::log_csv,
            "스키마대로 인자들을 C++에서 CSV 직렬화해 INFO로 기록 (f-string과 바이트 동일)")
        .def("format_csv", &Logger::format_csv,
            "log_csv가 출력할 본문을 로깅 없이 반환 (검증용)")
        .def("should_log", &Logger::should_log)
        .def("set_level", &Logger::set_level)
        .def("level", &Logger::level)
        .def("set_pattern", &Logger::set_pattern,
            nb::arg("pattern"), nb::arg("type") = spd::pattern_time_type::local, "type refers to time format and takes 'local' or 'utc'")
        .def("flush_on", &Logger::flush_on)
        .def("flush", &Logger::flush)
        .def("close", &Logger::close)
        .def("async_mode", &Logger::async)
        .def("sinks", &Logger::sinks)
        .def("set_error_handler", &Logger::set_error_handler)
        .def("get_underlying_logger", &Logger::get_underlying_logger);

    nb::class_<SinkLogger, Logger>(m, "SinkLogger")
    .def(nb::init<const std::string&, const std::vector<Sink>&>(),
        nb::arg("name"),
        nb::arg("sinks"))
    .def(nb::init<const std::string&, const std::vector<Sink>&, bool>(),
        nb::arg("name"),
        nb::arg("sinks"),
        nb::arg("async_mode"));

nb::class_<ConsoleLogger, Logger>(m, "ConsoleLogger")
    .def(nb::init<std::string, bool, bool, bool>(),
        nb::arg("name"),
        nb::arg("multithreaded") = false,
        nb::arg("stdout") = true,
        nb::arg("colored") = true)
    .def(nb::init<std::string, bool, bool, bool, bool>(),
        nb::arg("name"),
        nb::arg("multithreaded") = false,
        nb::arg("stdout") = true,
        nb::arg("colored") = true,
        nb::arg("async_mode"));

nb::class_<FileLogger, Logger>(m, "FileLogger")
    .def(nb::init<std::string, std::string, bool, bool>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded") = false,
        nb::arg("truncate") = false)
    .def(nb::init<std::string, std::string, bool, bool, bool>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded") = false,
        nb::arg("truncate") = false,
        nb::arg("async_mode"));
nb::class_<RotatingLogger, Logger>(m, "RotatingLogger")
    .def(nb::init<std::string, std::string, bool, int, int>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded"),
        nb::arg("max_file_size"),
        nb::arg("max_files"))
    .def(nb::init<std::string, std::string, bool, int, int, bool>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded"),
        nb::arg("max_file_size"),
        nb::arg("max_files"),
        nb::arg("async_mode"));
nb::class_<DailyLogger, Logger>(m, "DailyLogger")
    .def(nb::init<std::string, std::string, bool, int, int>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded") = false,
        nb::arg("hour") = 0,
        nb::arg("minute") = 0)
    .def(nb::init<std::string, std::string, bool, int, int, bool>(),
        nb::arg("name"),
        nb::arg("filename"),
        nb::arg("multithreaded") = false,
        nb::arg("hour") = 0,
        nb::arg("minute") = 0,
        nb::arg("async_mode"));

//SyslogLogger(const std::string& logger_name, const std::string& ident = "", int syslog_option = 0, int syslog_facilty = (1<<3))
#ifdef SPDLOG_ENABLE_SYSLOG
nb::class_<syslog_sink_st, Sink>(m, "syslog_sink_st")
    .def(nb::init<std::string, int, int, bool>(),
        nb::arg("ident") = "",
        nb::arg("syslog_option") = 0,
        nb::arg("syslog_facility") = (1 << 3),
        nb::arg("enable_formatting") = true);
nb::class_<syslog_sink_mt, Sink>(m, "syslog_sink_mt")
    .def(nb::init<std::string, int, int, bool>(),
        nb::arg("ident") = "",
        nb::arg("syslog_option") = 0,
        nb::arg("syslog_facility") = (1 << 3),
        nb::arg("enable_formatting") = true);
    nb::class_<SyslogLogger, Logger>(m, "SyslogLogger")
        .def(nb::init<std::string, bool, std::string, int, int>(),
            nb::arg("name"),
            nb::arg("multithreaded") = false,
            nb::arg("ident") = "",
            nb::arg("syslog_option") = 0,
            nb::arg("syslog_facility") = (1 << 3))
        .def(nb::init<std::string, bool, std::string, int, int, bool>(),
            nb::arg("name"),
            nb::arg("multithreaded") = false,
            nb::arg("ident") = "",
            nb::arg("syslog_option") = 0,
            nb::arg("syslog_facility") = (1 << 3),
            nb::arg("async_mode"));
#endif
    m.def("get", get, nb::arg("name"), nb::rv_policy::copy);
    m.def("drop", drop, nb::arg("name"));
    m.def("drop_all", drop_all);

#ifdef VERSION_INFO
    m.attr("__version__") = VERSION_INFO;
#else
    m.attr("__version__") = "dev";
#endif
}
