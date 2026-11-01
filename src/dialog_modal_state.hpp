#pragma once

#include <atomic>

namespace erui::native {

enum class DialogModalLoss {
    none,
    dismissed,
    ownership_lost,
};

struct DialogModalReconcileResult {
    void* job{};
    DialogModalLoss loss{DialogModalLoss::none};
};

// r150: A reconcile that did not come from CSPopupMenu's own update (the
// popup manager stopped updating or vanished) only saw the task disappear;
// it did not watch the dialog close. Without a recorded button response
// nobody answered it, so report it as lost, never as a successful dismissal.
[[nodiscard]] constexpr DialogModalLoss classify_unobserved_loss(
    DialogModalLoss loss,
    bool response_recorded) noexcept {
    return loss == DialogModalLoss::dismissed && !response_recorded
        ? DialogModalLoss::ownership_lost
        : loss;
}

// Tracks the exact CSPopupMenu blocking task owned by ERNativeUI. The owner
// is published before the job; acquiring a non-null job therefore also makes
// its owner visible. Reconciliation clears a task exactly once.
class DialogModalState {
public:
    void publish(void* owner, void* job) noexcept {
        owner_.store(owner, std::memory_order_relaxed);
        job_.store(job, std::memory_order_release);
    }

    [[nodiscard]] bool active() const noexcept {
        return job_.load(std::memory_order_acquire) != nullptr;
    }

    // r150: The task currently owned, for callers that sample the popup slot
    // on another thread. Load this before sampling the slot and pass it to
    // reconcile_submitted().
    [[nodiscard]] void* submitted() const noexcept {
        return job_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool owns(void* owner, void* slot_job) const noexcept {
        void* const submitted = job_.load(std::memory_order_acquire);
        return submitted && owner == owner_.load(std::memory_order_relaxed) &&
            slot_job == submitted;
    }

    // Returns the task and reason when ownership ends. A compare/exchange
    // makes repeated observations and nested hooks idempotent.
    [[nodiscard]] DialogModalReconcileResult reconcile(
        void* owner,
        void* slot_job) noexcept {
        void* const submitted = job_.load(std::memory_order_acquire);
        return reconcile_submitted(submitted, owner, slot_job);
    }

    // r150: Reconciles only the task `expected` (from submitted()). If that
    // task was already reconciled and another one published before the slot
    // was sampled, the stale sample must not end the new task, so nothing
    // happens unless `expected` is still the owned task.
    [[nodiscard]] DialogModalReconcileResult reconcile_submitted(
        void* expected,
        void* owner,
        void* slot_job) noexcept {
        if (!expected) return {};
        // Acquire pairs with publish(): the owner read below then belongs to
        // `expected` or to a later publish, which the exchange rejects.
        if (job_.load(std::memory_order_acquire) != expected) return {};
        const bool same_owner =
            owner == owner_.load(std::memory_order_relaxed);
        if (same_owner && slot_job == expected) return {};
        const DialogModalLoss loss = same_owner && !slot_job
            ? DialogModalLoss::dismissed
            : DialogModalLoss::ownership_lost;
        void* submitted = expected;
        if (!job_.compare_exchange_strong(
                submitted, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return {};
        }
        // r150: owner_ is left as is. It is only read together with a
        // non-null job, and clearing it here could race a publish() on the
        // game thread (same owner value) and orphan the newly published task.
        return {expected, loss};
    }

    void reset() noexcept {
        job_.store(nullptr, std::memory_order_release);
        owner_.store(nullptr, std::memory_order_release);
    }

private:
    std::atomic<void*> owner_{};
    std::atomic<void*> job_{};
};

} // namespace erui::native
