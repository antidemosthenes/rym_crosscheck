#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include "rym_crosscheck/matching/normalize.hpp"
#include "rym_crosscheck/storage/files.hpp"
#include "rym_crosscheck/storage/whitelist.hpp"
#include <optional>

namespace rym {
struct ReportCandidate {
    std::string title;
    std::vector<std::string> artists;
    std::string path;
    Fraction artist, title_score;
    bool credit_gap{};
    bool version_gap{};
    bool artist_gap{};
    bool section_gap{};
    bool compound_gap{};
    bool annotation_gap{};
    bool diacritic_gap{};
};
struct ReportSong {
    Song song;
    bool owned{};
    boost::container::static_vector<ReportCandidate, 3> candidates;
};
struct Report {
    std::string generation, source{"https://rateyourmusic.com/charts/top/song/all-time/"};
    std::int64_t generated{}, captured{};
    bool offline{};
    std::size_t identified{}, reused{};
    std::vector<std::string> warnings;
    std::vector<ReportSong> songs;
    std::vector<WhitelistEntry> whitelist;
};
Report compare_library(const Snapshot &snapshot, const Scan &scan,
                       const std::filesystem::path &root, bool offline, Fraction artist_cutoff,
                       Fraction title_cutoff);
boost::json::object report_json(const Report &report);
Report parse_report(const boost::json::value &json);
struct PortablePaths {
    std::filesystem::path application, directory, report, whitelist, viewer, cli, settings, cache;
    static PortablePaths from_directory(const std::filesystem::path &directory);
};
std::filesystem::path executable_directory();
std::filesystem::path cli_installation_directory();
std::wstring installation_id(const PortablePaths &paths);
class ReportLock {
    Handle mutex_;

  public:
    explicit ReportLock(const PortablePaths &paths);
    ~ReportLock() noexcept;
    ReportLock(const ReportLock &) = delete;
    ReportLock &operator=(const ReportLock &) = delete;
};
class RunLock {
    Handle mutex_;

  public:
    explicit RunLock(const PortablePaths &paths, DWORD timeout = 0);
    ~RunLock() noexcept;
    RunLock(const RunLock &) = delete;
    RunLock &operator=(const RunLock &) = delete;
};
struct ReviewDocument {
    std::optional<Report> report;
    Whitelist whitelist;
    std::pair<std::int64_t, std::int64_t> revision{};
    bool whitelist_valid{true};
};
struct SaveResult {
    ReviewDocument document;
    std::string warning;
};
struct ImportData {
    std::filesystem::path source;
    bool no_op{};
    std::optional<Report> report;
    std::optional<Whitelist> whitelist;
    std::size_t count() const noexcept {
        return report ? report->songs.size() : whitelist ? whitelist->entries().size() : 0;
    }
};
ImportData inspect_import(const PortablePaths &paths, const std::filesystem::path &source,
                          bool report);
class ReportStore {
    PortablePaths paths_;

  public:
    explicit ReportStore(PortablePaths paths) : paths_(std::move(paths)) {}
    const PortablePaths &paths() const noexcept {
        return paths_;
    }
    void initialize(bool create_whitelist = true);
    std::pair<std::int64_t, std::int64_t> revision() const noexcept;
    ReviewDocument load() const;
    SaveResult available() const;
    Report publish(Report report) const;
    bool differs(const Whitelist &proposed) const;
    SaveResult replace(const Whitelist &proposed) const;
    Report import_report(Report report) const;
    SaveResult import_file(const ImportData &input) const;
    SaveResult apply(const ReviewDocument &baseline, const std::vector<std::string> &add,
                     const std::vector<std::string> &remove) const;
};
Whitelist proposed_whitelist(const ReviewDocument &baseline, const std::vector<std::string> &add,
                             const std::vector<std::string> &remove);
boost::json::object review_json(const ReviewDocument &document);
}
