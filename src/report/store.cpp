#include "rym_crosscheck/report/report.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <objbase.h>

namespace rym {
PortablePaths PortablePaths::from_directory(const std::filesystem::path &directory) {
    PortablePaths result;
    result.application = std::filesystem::absolute(directory).lexically_normal();
    result.directory = result.application / "data";
    result.report = result.directory / "missing.json";
    result.whitelist = result.directory / "whitelist.toml";
    result.viewer = result.application / "missing-report.exe";
    result.cli = result.application / "bin/rym-crosscheck.exe";
    result.settings = result.directory / "settings.toml";
    result.cache = result.directory / "cache";
    return result;
}
std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size == path.size())
        throw Error("Cannot locate the executable directory.");
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}
std::wstring installation_id(const PortablePaths &paths) {
    auto path = std::filesystem::absolute(paths.directory).lexically_normal().native();
    auto length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(),
                                static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!length)
        throw Error("Cannot identify the installation directory.");
    std::wstring folded(static_cast<std::size_t>(length), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(),
                  static_cast<int>(path.size()), folded.data(), length, nullptr, nullptr, 0);
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto c : folded) {
        hash ^= static_cast<std::uint16_t>(c);
        hash *= 1099511628211ULL;
    }
    return std::to_wstring(hash);
}
ReportLock::ReportLock(const PortablePaths &paths) {
    auto name = L"Local\\rym-crosscheck-store-" + installation_id(paths);
    mutex_ = Handle(CreateMutexW(nullptr, FALSE, name.c_str()));
    if (!mutex_)
        throw Error("Cannot create the report-store lock.");
    auto result = WaitForSingleObject(mutex_.get(), 5000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        throw Error("The report store is busy. Try again shortly.");
}
ReportLock::~ReportLock() noexcept {
    ReleaseMutex(mutex_.get());
}
RunLock::RunLock(const PortablePaths &paths, DWORD timeout) {
    auto name = L"Local\\rym-crosscheck-run-" + installation_id(paths);
    mutex_ = Handle(CreateMutexW(nullptr, FALSE, name.c_str()));
    if (!mutex_)
        throw Error("Cannot create the comparison lock.");
    auto result = WaitForSingleObject(mutex_.get(), timeout);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        throw Error("Another comparison or data update is active in this installation.");
}
RunLock::~RunLock() noexcept {
    ReleaseMutex(mutex_.get());
}
std::filesystem::path cli_installation_directory() {
    auto directory = executable_directory();
    return _wcsicmp(directory.filename().c_str(), L"bin") == 0 ? directory.parent_path()
                                                               : directory;
}
void ReportStore::initialize(bool create_whitelist) {
    ReportLock lock(paths_);
    std::filesystem::create_directories(paths_.directory);
    auto attributes = GetFileAttributesW(paths_.directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("The data folder must be a normal directory in the installation.");
    if (create_whitelist) {
        if (!std::filesystem::exists(paths_.whitelist))
            atomic_write(paths_.whitelist, Whitelist{}.toml());
        Whitelist validated(paths_.whitelist, true);
    }
}
ReviewDocument ReportStore::load() const {
    ReportLock lock(paths_);
    ReviewDocument result;
    result.whitelist = Whitelist(paths_.whitelist, false);
    if (std::filesystem::exists(paths_.report))
        result.report = parse_report(read_json(paths_.report));
    result.revision = revision();
    return result;
}
SaveResult ReportStore::available() const {
    ReportLock lock(paths_);
    SaveResult result;
    try {
        result.document.whitelist = Whitelist(paths_.whitelist, false);
    } catch (const std::exception &failure) {
        result.document.whitelist_valid = false;
        result.warning = "Whitelist could not be loaded: " + std::string(failure.what()) + ". ";
    }
    try {
        if (std::filesystem::exists(paths_.report))
            result.document.report = parse_report(read_json(paths_.report));
    } catch (const std::exception &failure) {
        result.warning += "Report could not be loaded: " + std::string(failure.what());
    }
    result.document.revision = revision();
    return result;
}
std::pair<std::int64_t, std::int64_t> ReportStore::revision() const noexcept {
    try {
        return {std::filesystem::exists(paths_.report) ? modified_time(paths_.report) : 0,
                std::filesystem::exists(paths_.whitelist) ? modified_time(paths_.whitelist) : 0};
    } catch (...) {
        return {-1, -1};
    }
}
Report ReportStore::publish(Report report) const {
    ReportLock lock(paths_);
    report.whitelist = Whitelist(paths_.whitelist, false).entries();
    write_json(paths_.report, report_json(report));
    return report;
}
Whitelist proposed_whitelist(const ReviewDocument &baseline, const std::vector<std::string> &add,
                             const std::vector<std::string> &remove) {
    if (!baseline.whitelist_valid)
        throw Error("Load a valid whitelist before editing; the unreadable file is preserved.");
    boost::unordered_flat_map<std::string, const Song *> allowed;
    if (!add.empty() && baseline.report) {
        allowed.reserve(baseline.report->songs.size());
        for (const auto &row : baseline.report->songs)
            if (!row.owned && !baseline.whitelist.contains(row.song))
                allowed.emplace(row.song.url, &row.song);
    }
    boost::unordered_flat_set<std::string> removable, removed;
    removable.reserve(baseline.whitelist.entries().size());
    removed.reserve(remove.size());
    for (const auto &entry : baseline.whitelist.entries())
        removable.insert(entry.key);
    for (const auto &key : remove) {
        if (!removable.contains(key))
            throw Error("Unknown whitelist removal selection.");
        removed.insert(key);
    }
    for (const auto &url : add)
        if (!allowed.contains(url) || song_url(url) != url)
            throw Error("Unknown song addition selection.");
    std::vector<WhitelistEntry> entries;
    entries.reserve(baseline.whitelist.entries().size() + add.size());
    for (const auto &entry : baseline.whitelist.entries())
        if (!removed.contains(entry.key))
            entries.push_back(entry);
    Whitelist retained(entries);
    for (const auto &url : add)
        if (!retained.contains(*allowed.at(url)))
            entries.push_back({{}, url, {}, {}});
    return Whitelist(std::move(entries));
}
bool ReportStore::differs(const Whitelist &proposed) const {
    ReportLock lock(paths_);
    Whitelist current(paths_.whitelist, false);
    boost::unordered_flat_set<std::string> keys;
    keys.reserve(current.entries().size());
    for (const auto &entry : current.entries())
        keys.insert(entry.key);
    return proposed.entries().size() != keys.size() ||
           std::ranges::any_of(proposed.entries(),
                               [&](const auto &entry) { return !keys.contains(entry.key); });
}
SaveResult ReportStore::replace(const Whitelist &updated) const {
    RunLock run(paths_, 5000);
    ReportLock lock(paths_);
    SaveResult result;
    result.document.whitelist = updated;
    atomic_write(paths_.whitelist, updated.toml());
    try {
        if (std::filesystem::exists(paths_.report)) {
            auto active = parse_report(read_json(paths_.report));
            active.whitelist = updated.entries();
            result.document.report = std::move(active);
            write_json(paths_.report, report_json(*result.document.report));
        }
    } catch (const std::exception &error) {
        result.warning = "Whitelist saved, but the active report could not be refreshed: " +
                         std::string(error.what());
    }
    result.document.revision = revision();
    return result;
}
SaveResult ReportStore::apply(const ReviewDocument &baseline, const std::vector<std::string> &add,
                              const std::vector<std::string> &remove) const {
    if (!baseline.whitelist_valid)
        throw Error("Load a valid whitelist before saving; the unreadable file is preserved.");
    RunLock run(paths_, 5000);
    ReportLock lock(paths_);
    Whitelist current(paths_.whitelist, false);
    return replace(proposed_whitelist(baseline, add, remove));
}
Report ReportStore::import_report(Report report) const {
    RunLock run(paths_, 5000);
    ReportLock lock(paths_);
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw Error("Cannot assign an imported report generation.");
    wchar_t value[40]{};
    StringFromGUID2(guid, value, 40);
    report.generation = utf8(value);
    try {
        report.whitelist = Whitelist(paths_.whitelist, false).entries();
    } catch (const std::exception &) {
        report.whitelist.clear();
    }
    write_json(paths_.report, report_json(report));
    return report;
}
ImportData inspect_import(const PortablePaths &paths, const std::filesystem::path &source,
                          bool report) {
    ImportData result;
    result.source = std::filesystem::absolute(source).lexically_normal();
    std::error_code error;
    result.no_op =
        std::filesystem::equivalent(result.source, report ? paths.report : paths.whitelist, error);
    if (result.no_op)
        return result;
    if (report)
        result.report = parse_report(read_json(result.source));
    else
        result.whitelist = Whitelist(result.source, true);
    return result;
}
SaveResult ReportStore::import_file(const ImportData &input) const {
    if (input.no_op)
        return available();
    if (input.report) {
        import_report(*input.report);
        return available();
    }
    if (input.whitelist)
        return replace(*input.whitelist);
    throw Error("No validated import data.");
}
}
