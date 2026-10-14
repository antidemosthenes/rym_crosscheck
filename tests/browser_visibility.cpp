#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/rym/transport.hpp"
#include <algorithm>
#include <array>
#include <boost/container/static_vector.hpp>
#include <future>
#include <iostream>

namespace {
using Windows = boost::container::static_vector<HWND, 256>;
Windows visible_edge_windows() {
    Windows windows;
    EnumWindows(
        [](HWND window, LPARAM context) noexcept -> BOOL {
            if (!IsWindowVisible(window))
                return TRUE;
            std::array<wchar_t, 64> name{};
            if (!GetClassNameW(window, name.data(), static_cast<int>(name.size())) ||
                std::wstring_view(name.data()) != L"Chrome_WidgetWin_1")
                return TRUE;
            DWORD pid{};
            GetWindowThreadProcessId(window, &pid);
            rym::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            std::array<wchar_t, 32768> executable{};
            DWORD length = static_cast<DWORD>(executable.size());
            if (process &&
                QueryFullProcessImageNameW(process.get(), 0, executable.data(), &length) &&
                std::wstring_view(executable.data(), length).ends_with(L"\\msedge.exe")) {
                auto &found = *reinterpret_cast<Windows *>(context);
                if (found.size() == found.capacity())
                    return FALSE;
                found.push_back(window);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}
}
int wmain(int argc, wchar_t *argv[]) {
    try {
        if (argc != 2)
            throw rym::Error("Supply a previously verified dedicated Edge profile.");
        rym::install_cancellation();
        auto initial = visible_edge_windows();
        std::atomic_bool visible{};
        std::jthread monitor([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (auto window : visible_edge_windows())
                    if (std::ranges::find(initial, window) == initial.end())
                        visible.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        {
            rym::Pacer pacer(std::chrono::milliseconds(1000));
            rym::Browser browser(argv[1], pacer);
            constexpr std::string_view chart =
                "https://rateyourmusic.com/charts/top/song/all-time/";
            auto first = rym::parse_chart(browser.fetch(chart), chart);
            if (first.next.empty() || first.songs.empty())
                throw rym::Error("Chart fixture has no pagination or songs.");
            auto worker = browser.fork();
            auto album = std::async(std::launch::async,
                                    [&] { return worker->fetch(first.songs.front().url); });
            auto second = rym::parse_chart(browser.fetch(first.next), first.next);
            auto song_html = album.get();
            if (second.songs.empty() || song_html.empty())
                throw rym::Error("Hidden retrieval returned empty content.");
            rym::Browser moved(std::move(browser));
            auto third = moved.fork();
            third->fetch(first.songs.back().url);
        }
        monitor.request_stop();
        monitor.join();
        if (visible.load())
            throw rym::Error("Retrieval opened a visible Edge window.");
        std::cout << "PASS: chart pagination, parallel song retrieval, browser moves, and teardown "
                     "opened no visible Edge windows.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
