#pragma once
#include "host.h"
#include <iostream>
#include <ctime>
#include <iomanip>

class Logger {
public:
    enum Level { DEBUG_L=0, INFO_L=1, WARN_L=2, ERR_L=3 };
    static Logger& get() { static Logger l; return l; }

    void set_level(const std::string& lvl) {
        if (lvl=="DEBUG") min_level_=DEBUG_L;
        else if (lvl=="WARN") min_level_=WARN_L;
        else if (lvl=="ERROR") min_level_=ERR_L;
        else min_level_=INFO_L;
    }

    void set_file(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) file_.close();
        file_.open(path, std::ios::app);
    }

    void log(Level lvl, const std::string& msg) {
        if (lvl < min_level_) return;
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
        static const char* names[] = {"DBG","INF","WRN","ERR"};
        std::string line = std::string("[") + ts + "][" + names[lvl] + "] " + msg;
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << line << "\n";
        if (file_.is_open()) file_ << line << "\n" << std::flush;
    }

    void info (const std::string& m){ log(INFO_L, m); }
    void warn (const std::string& m){ log(WARN_L, m); }
    void error(const std::string& m){ log(ERR_L,  m); }
    void debug(const std::string& m){ log(DEBUG_L,m); }

private:
    Logger() = default;
    std::mutex mu_;
    std::ofstream file_;
    Level min_level_ = INFO_L;
};

#define LOG_INFO(m)  Logger::get().info(m)
#define LOG_WARN(m)  Logger::get().warn(m)
#define LOG_ERROR(m) Logger::get().error(m)
#define LOG_DEBUG(m) Logger::get().debug(m)
