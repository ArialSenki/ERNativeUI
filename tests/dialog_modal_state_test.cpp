#include "dialog_modal_state.hpp"
#include "dialog_input_gate.hpp"

#include "test_assertions.hpp"

int main() {
    using erui::native::should_suppress_native_back;
    ERUI_TEST_CHECK(!should_suppress_native_back(false, false, 3));
    ERUI_TEST_CHECK(should_suppress_native_back(true, false, 3));
    ERUI_TEST_CHECK(!should_suppress_native_back(true, true, 3));
    ERUI_TEST_CHECK(!should_suppress_native_back(true, false, 2));

    erui::native::DialogModalState state{};
    int owner_a{};
    int owner_b{};
    int job_a{};
    int job_b{};

    ERUI_TEST_CHECK(!state.active());
    ERUI_TEST_CHECK(!state.owns(&owner_a, &job_a));
    ERUI_TEST_CHECK(state.reconcile(&owner_a, nullptr).job == nullptr);

    state.publish(&owner_a, &job_a);
    ERUI_TEST_CHECK(state.active());
    ERUI_TEST_CHECK(state.owns(&owner_a, &job_a));
    ERUI_TEST_CHECK(!state.owns(&owner_a, &job_b));
    ERUI_TEST_CHECK(!state.owns(&owner_b, &job_a));
    ERUI_TEST_CHECK(state.reconcile(&owner_a, &job_a).job == nullptr);
    ERUI_TEST_CHECK(state.active());

    const auto dismissed = state.reconcile(&owner_a, nullptr);
    ERUI_TEST_CHECK(dismissed.job == &job_a);
    ERUI_TEST_CHECK(dismissed.loss == erui::native::DialogModalLoss::dismissed);
    ERUI_TEST_CHECK(!state.active());
    ERUI_TEST_CHECK(state.reconcile(&owner_a, nullptr).job == nullptr);

    state.publish(&owner_a, &job_b);
    const auto replaced_owner = state.reconcile(&owner_b, &job_b);
    ERUI_TEST_CHECK(replaced_owner.job == &job_b);
    ERUI_TEST_CHECK(replaced_owner.loss ==
        erui::native::DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(!state.active());

    state.publish(&owner_a, &job_a);
    const auto replaced_job = state.reconcile(&owner_a, &job_b);
    ERUI_TEST_CHECK(replaced_job.job == &job_a);
    ERUI_TEST_CHECK(replaced_job.loss ==
        erui::native::DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(!state.active());

    // r150: Reconcile from outside CSPopupMenu's update (host thread). The
    // expected task is sampled first, then the popup slot.
    using erui::native::DialogModalLoss;
    state.publish(&owner_a, &job_a);
    ERUI_TEST_CHECK(state.submitted() == &job_a);
    ERUI_TEST_CHECK(
        state.reconcile_submitted(&job_a, &owner_a, &job_a).job == nullptr);
    ERUI_TEST_CHECK(state.active());
    // The popup manager vanished: no owner, no slot.
    const auto vanished = state.reconcile_submitted(&job_a, nullptr, nullptr);
    ERUI_TEST_CHECK(vanished.job == &job_a);
    ERUI_TEST_CHECK(vanished.loss == DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(!state.active());
    ERUI_TEST_CHECK(state.submitted() == nullptr);
    // The normal popup path then observes the same disappearance: no second
    // completion.
    ERUI_TEST_CHECK(state.reconcile(&owner_a, nullptr).job == nullptr);
    ERUI_TEST_CHECK(state.reconcile_submitted(nullptr, nullptr, nullptr).job ==
        nullptr);

    // Stale sample: job_a was reconciled by the popup path and job_b
    // published on the same owner before the host thread's stale (empty) slot
    // read reaches reconcile_submitted. job_b must survive.
    state.publish(&owner_a, &job_a);
    void* const sampled = state.submitted();
    ERUI_TEST_CHECK(state.reconcile(&owner_a, nullptr).job == &job_a);
    state.publish(&owner_a, &job_b);
    ERUI_TEST_CHECK(
        state.reconcile_submitted(sampled, &owner_a, nullptr).job == nullptr);
    ERUI_TEST_CHECK(state.active());
    ERUI_TEST_CHECK(state.owns(&owner_a, &job_b));
    ERUI_TEST_CHECK(state.reconcile(&owner_a, &job_b).job == nullptr);
    const auto lost_b = state.reconcile_submitted(&job_b, &owner_b, &job_b);
    ERUI_TEST_CHECK(lost_b.job == &job_b);
    ERUI_TEST_CHECK(lost_b.loss == DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(!state.active());

    // Re-publishing after a reconcile keeps ownership intact.
    state.publish(&owner_a, &job_a);
    ERUI_TEST_CHECK(state.owns(&owner_a, &job_a));
    const auto host_dismissed =
        state.reconcile_submitted(state.submitted(), &owner_a, nullptr);
    ERUI_TEST_CHECK(host_dismissed.job == &job_a);
    ERUI_TEST_CHECK(host_dismissed.loss == DialogModalLoss::dismissed);

    // An unobserved dismissal reports success only with a recorded response.
    using erui::native::classify_unobserved_loss;
    ERUI_TEST_CHECK(classify_unobserved_loss(DialogModalLoss::dismissed, true) ==
        DialogModalLoss::dismissed);
    ERUI_TEST_CHECK(classify_unobserved_loss(DialogModalLoss::dismissed, false) ==
        DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(
        classify_unobserved_loss(DialogModalLoss::ownership_lost, true) ==
        DialogModalLoss::ownership_lost);
    ERUI_TEST_CHECK(classify_unobserved_loss(DialogModalLoss::none, false) ==
        DialogModalLoss::none);

    state.publish(&owner_b, &job_a);
    state.reset();
    ERUI_TEST_CHECK(!state.active());
    ERUI_TEST_CHECK(!state.owns(&owner_b, &job_a));
    return 0;
}
