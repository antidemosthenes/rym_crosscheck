#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include "rym_crosscheck/rym/transport.hpp"
#include <functional>
namespace rym {
Snapshot fetch_chart(Transport &transport, std::size_t top);
void enrich_albums(const std::vector<Missing> &missing, Snapshot &snapshot,
                   const std::function<std::unique_ptr<Transport>()> &factory, std::size_t workers);
}
