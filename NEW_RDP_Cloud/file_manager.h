#pragma once
#include "host.h"
#include "logger.h"

class FileManager {
public:
    // List directory contents as JSON
    static std::string list_dir(const std::string& path) {
        std::ostringstream json;
        json << "{\"cmd\":\"file_list_result\",\"path\":\"" << json_escape(path) << "\",\"items\":[";

        bool first = true;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(path, ec)) {
            if (ec) break;
            try {
                std::string name = entry.path().filename().string();
                bool is_dir = entry.is_directory(ec);
                int64_t size = is_dir ? 0 : (int64_t)entry.file_size(ec);
                auto ftime = entry.last_write_time(ec);
                int64_t mtime = 0;
                if (!ec) {
#if defined(_WIN32) && !defined(__clang__)
                    auto duration = ftime.time_since_epoch();
                    mtime = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
#else
                    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                        std::chrono::file_clock::to_sys(ftime));
                    mtime = sctp.time_since_epoch().count();
#endif
                }
                if (!first) json << ",";
                json << "{\"name\":\"" << json_escape(name) << "\""
                     << ",\"type\":\"" << (is_dir?"dir":"file") << "\""
                     << ",\"size\":" << size
                     << ",\"modified\":" << mtime << "}";
                first = false;
            } catch(...) {}
        }
        json << "]}";
        return json.str();
    }

    // Read a chunk of file (offset, length), returns data
    static std::vector<uint8_t> read_file_chunk(const std::string& path, uint64_t offset, uint32_t length) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        f.seekg((std::streamoff)offset);
        std::vector<uint8_t> buf(length);
        f.read((char*)buf.data(), length);
        size_t n = (size_t)f.gcount();
        buf.resize(n);
        return buf;
    }

    // Read file, return base64 chunks via callback
    static bool read_file_chunks(const std::string& path,
        std::function<void(const uint8_t*, size_t, size_t, size_t)> cb,
        size_t chunk_size = 1*1024*1024)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        size_t total = (size_t)f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(chunk_size);
        size_t offset = 0;
        while (f) {
            f.read((char*)buf.data(), chunk_size);
            size_t n = (size_t)f.gcount();
            if (n == 0) break;
            cb(buf.data(), n, offset, total);
            offset += n;
        }
        return true;
    }

    // Write file chunk
    static bool write_chunk(const std::string& path, const uint8_t* data, size_t len,
                            size_t offset, bool create_new)
    {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        auto mode = std::ios::binary | (create_new ? std::ios::trunc : std::ios::in|std::ios::out);
        std::fstream f(path, mode);
        if (!f && create_new) f.open(path, std::ios::binary|std::ios::out);
        if (!f) return false;
        f.seekp((std::streamoff)offset);
        f.write((const char*)data, len);
        return f.good();
    }

    static bool delete_path(const std::string& path) {
        std::error_code ec;
        fs::remove_all(path, ec);
        return !ec;
    }

    static bool create_directory(const std::string& path) {
        std::error_code ec;
        return fs::create_directories(path, ec);
    }

    static bool rename_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::rename(from, to, ec);
        return !ec;
    }

    static bool copy_path(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::copy(from, to, fs::copy_options::recursive|fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    static std::string read_text_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }

    static bool write_text_file(const std::string& path, const std::string& content) {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path);
        if (!f) return false;
        f << content;
        return f.good();
    }
};
