#include "rym_crosscheck/storage/files.hpp"
#include <fstream>
#include <random>

namespace rym {
std::string read_file(const std::filesystem::path &path, std::size_t maximum) {
    auto size = std::filesystem::file_size(path);
    if (size > maximum)
        throw Error("File exceeds size limit: " + path_text(path));
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw Error("Cannot read " + path_text(path));
    std::string result(static_cast<std::size_t>(size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(size));
    if (!input && size)
        throw Error("Cannot read complete file: " + path_text(path));
    return result;
}
void atomic_write(const std::filesystem::path &path, std::string_view value) {
    auto absolute = std::filesystem::absolute(path);
    std::filesystem::create_directories(absolute.parent_path());
    static std::atomic<std::uint64_t> sequence{};
    auto temporary = absolute;
    temporary += L".rym-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1)) + L".tmp";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() noexcept {
            try {
                std::error_code error;
                std::filesystem::remove(path, error);
            } catch (...) {
            }
        }
    } cleanup{temporary};
    Handle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle)
        throw Error("Cannot create report/cache temporary file.");
    std::size_t offset{};
    while (offset < value.size()) {
        DWORD written{};
        auto length = static_cast<DWORD>(std::min<std::size_t>(value.size() - offset, 1024 * 1024));
        if (!WriteFile(handle.get(), value.data() + offset, length, &written, nullptr) || !written)
            throw Error("Cannot write output.");
        offset += written;
    }
    if (!FlushFileBuffers(handle.get()))
        throw Error("Cannot flush output.");
    handle = Handle{};
    if (!MoveFileExW(temporary.c_str(), absolute.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error("Cannot atomically replace " + path_text(path));
}
boost::json::value read_json(const std::filesystem::path &path) {
    auto text = read_file(path);
    return boost::json::parse(text);
}
void write_json(const std::filesystem::path &path, const boost::json::value &value) {
    atomic_write(path, boost::json::serialize(value));
}
std::int64_t modified_time(const std::filesystem::path &path) {
    return std::filesystem::last_write_time(path).time_since_epoch().count();
}
}
