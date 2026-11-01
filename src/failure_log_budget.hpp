#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace erui::detail {

// r148: the native-tab failure log budget (r145) as a small class so its
// site table can be tested without the game. Each call site (identified by
// its format string pointer) has its own budget; a total cap bounds all of
// them together. Lock-free: sites are claimed with a compare-exchange.
template <std::size_t SiteCount>
class FailureLogBudget {
public:
    struct Decision {
        // Count of this failure at its site; 0 once the site table is full.
        std::uint32_t site_count{};
        // Count across all sites; 0 when the site budget already stopped it.
        std::uint32_t total_count{};
        bool log{};
        bool last_for_site{};
        bool last_overall{};
    };

    constexpr FailureLogBudget(
        std::uint32_t per_site_limit, std::uint32_t total_limit) noexcept
        : per_site_limit_(per_site_limit), total_limit_(total_limit) {}

    FailureLogBudget(const FailureLogBudget&) = delete;
    FailureLogBudget& operator=(const FailureLogBudget&) = delete;

    // Count of this failure at its call site; 0 once the site table is full.
    std::uint32_t count_site(const char* site_key) noexcept {
        if (!site_key) return 0;
        for (std::size_t i = 0; i < SiteCount; ++i) {
            const char* site = sites_[i].load(std::memory_order_acquire);
            if (!site) {
                const char* expected = nullptr;
                if (!sites_[i].compare_exchange_strong(
                        expected, site_key, std::memory_order_acq_rel)) {
                    site = expected;
                } else {
                    site = site_key;
                }
            }
            if (site == site_key) {
                return counts_[i].fetch_add(1, std::memory_order_relaxed) + 1;
            }
        }
        return 0;
    }

    // A site past its own budget does not use up the total budget. Failures
    // from sites that found the table full are still bounded by the total.
    Decision record(const char* site_key) noexcept {
        Decision decision{};
        decision.site_count = count_site(site_key);
        if (decision.site_count > per_site_limit_) return decision;
        decision.total_count = total_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (decision.total_count > total_limit_) return decision;
        decision.log = true;
        decision.last_for_site = decision.site_count == per_site_limit_;
        decision.last_overall = decision.total_count == total_limit_;
        return decision;
    }

private:
    std::uint32_t per_site_limit_;
    std::uint32_t total_limit_;
    std::atomic_uint32_t total_{};
    std::array<std::atomic<const char*>, SiteCount> sites_{};
    std::array<std::atomic_uint32_t, SiteCount> counts_{};
};

} // namespace erui::detail
