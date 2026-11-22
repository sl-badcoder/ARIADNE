#include <assert.h>
#include <stdio.h>

#include "uvm_ariadne_policy.h"

static void test_utlb_history(void)
{
    uvm_ariadne_utlb_history_t history = {0};
    unsigned i;

    uvm_ariadne_add_utlb(&history, 0);
    uvm_ariadne_add_utlb(&history, 256);
    uvm_ariadne_add_utlb(&history, 0);
    uvm_ariadne_add_utlb(&history, 0xffffffffU);
    assert(history.degree == 3);
    for (i = 0; i < UVM_ARIADNE_UTLB_HISTORY - 4; ++i)
        uvm_ariadne_add_utlb(&history, 0);
    assert(history.next == 0 && history.degree == 3);
    uvm_ariadne_add_utlb(&history, 0);
    assert(history.degree == 3);
    uvm_ariadne_add_utlb(&history, 0);
    assert(history.degree == 2);
    uvm_ariadne_add_utlb(&history, 0);
    uvm_ariadne_add_utlb(&history, 0);
    assert(history.degree == 1);

    for (i = 0; i < UVM_ARIADNE_UTLB_HISTORY; ++i)
        uvm_ariadne_add_utlb(&history, i);
    assert(history.degree == UVM_ARIADNE_UTLB_HISTORY);
    for (i = 0; i < UVM_ARIADNE_UTLB_HISTORY; ++i) {
        uvm_ariadne_add_utlb(&history, i);
        assert(history.degree == UVM_ARIADNE_UTLB_HISTORY);
    }
}

static void test_sd_window_and_pinning(void)
{
    uvm_ariadne_sd_window_t window = {0};
    const NvU64 max_time = ~(NvU64)0;
    unsigned i;

    assert(!uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 15));
    for (i = 0; i < UVM_ARIADNE_SD_WINDOW; ++i)
        uvm_ariadne_sample_sd(&window, 16);
    assert(window.sum == UVM_ARIADNE_SD_WINDOW * 16 && window.next == 0);
    assert(!uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 15));

    for (i = 0; i < UVM_ARIADNE_SD_WINDOW; ++i)
        uvm_ariadne_sample_sd(&window, 1);
    assert(window.count == UVM_ARIADNE_SD_WINDOW && window.sum == window.count);
    // Threshold 15 denotes 1.5 distinct uTLBs per sampled block.
    for (i = 0; i < 40; ++i)
        uvm_ariadne_sample_sd(&window, 2);
    assert(window.sum == 140);
    assert(uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 15) == 200);
    for (i = 0; i < 10; ++i)
        uvm_ariadne_sample_sd(&window, 2);
    assert(window.sum == 150);
    assert(uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 15) == 200);
    // Candidate sharing is ranked by eviction score, not a hard average gate.
    assert(uvm_ariadne_pin_deadline(100, 90, 100, 2, &window, 15) == 200);
    for (i = 0; i < 10; ++i)
        uvm_ariadne_sample_sd(&window, 2);
    assert(window.sum == 160);
    assert(!uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 15));
    for (i = 0; i < UVM_ARIADNE_SD_WINDOW; ++i)
        uvm_ariadne_sample_sd(&window, 1);
    assert(uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, 10) == 200);
    assert(!uvm_ariadne_pin_deadline(100, 0, 100, 1, &window, 15));
    assert(!uvm_ariadne_pin_deadline(100, 101, 100, 1, &window, 15));
    assert(!uvm_ariadne_pin_deadline(100, 90, 0, 1, &window, 15));
    assert(!uvm_ariadne_pin_deadline(100, 90, 100, 0, &window, 15));
    assert(uvm_ariadne_pin_deadline(189, 90, 100, 1, &window, 15) == 289);
    assert(!uvm_ariadne_pin_deadline(190, 90, 100, 1, &window, 15));
    assert(!uvm_ariadne_pin_deadline(max_time - 10, max_time - 12, 11, 1, &window, 15));
    assert(uvm_ariadne_pin_deadline(max_time - 10, max_time - 12, 10, 1, &window, 15) == max_time);
    assert(uvm_ariadne_pin_deadline(100, 90, 100, 1, &window, ~0U) == 200);
}

static void test_stream_against_recent_faults(void)
{
    uvm_ariadne_utlb_history_t history = {0};
    NvU32 recent[UVM_ARIADNE_UTLB_HISTORY] = {0};
    NvU32 random = 42;
    unsigned step;

    for (step = 0; step < 10000; ++step) {
        unsigned i, j, expected = 0;
        unsigned count = step + 1 < UVM_ARIADNE_UTLB_HISTORY ? step + 1 : UVM_ARIADNE_UTLB_HISTORY;
        random = random * 1664525U + 1013904223U;
        for (i = UVM_ARIADNE_UTLB_HISTORY - 1; i; --i)
            recent[i] = recent[i - 1];
        recent[0] = (random >> 24) % 7 ? (random >> 16) % 8 : 0xffffffffU;
        uvm_ariadne_add_utlb(&history, recent[0]);
        for (i = 0; i < count; ++i) {
            for (j = 0; j < i && recent[j] != recent[i]; ++j)
                ;
            expected += j == i;
        }
        assert(history.degree == expected);
        assert(history.count == count);
    }
}

static void test_working_set_budget_and_duration(void)
{
    const NvU64 block = 2 * 1024 * 1024;
    const NvU64 base = 100000000;
    const NvU64 limit = ~(NvU64)0;
    uvm_ariadne_working_set_t set = {0};

    assert(!uvm_ariadne_pin_budget(&set, 20 * block));
    set.active_bytes = 100 * block;
    set.capacity_bytes = 100 * block;
    assert(!uvm_ariadne_pin_budget(&set, 20 * block));
    set.active_bytes = 130 * block;
    assert(uvm_ariadne_severe_pressure(&set));
    assert(uvm_ariadne_pin_budget(&set, 0) == 30 * block);
    assert(uvm_ariadne_pin_budget(&set, 20 * block) == 50 * block);
    assert(uvm_ariadne_pin_budget(&set, limit) == set.active_bytes);
    // Partial regions use bytes, avoiding double counting after a split.
    set.active_bytes = 100 * block + 4096;
    assert(!uvm_ariadne_severe_pressure(&set));
    assert(uvm_ariadne_pin_budget(&set, 0) == 4096);
    set.active_bytes = limit;
    set.capacity_bytes = 1;
    assert(uvm_ariadne_pin_budget(&set, 2) == limit);
    assert(uvm_ariadne_severe_pressure(&set));

    set.capacity_bytes = 100 * block;
    set.active_bytes = 110 * block;
    assert(!uvm_ariadne_severe_pressure(&set));
    set.active_bytes += 4096;
    assert(uvm_ariadne_severe_pressure(&set));
    {
        uvm_ariadne_sd_window_t high_sharing = {0};
        unsigned i, ceiling;
        for (i = 0; i < UVM_ARIADNE_SD_WINDOW; ++i)
            uvm_ariadne_sample_sd(&high_sharing, 16);
        assert(!uvm_ariadne_low_sharing(&high_sharing, 15));
        ceiling = uvm_ariadne_severe_pressure(&set) ? ~0U : 15;
        assert(uvm_ariadne_pin_deadline(100, 90, 100, 16, &high_sharing, ceiling) == 200);
        set.active_bytes = 105 * block;
        ceiling = uvm_ariadne_severe_pressure(&set) ? ~0U : 15;
        assert(!uvm_ariadne_pin_deadline(100, 90, 100, 16, &high_sharing, ceiling));
    }

    assert(uvm_ariadne_pin_duration(base, block, block, 1) == base);
    assert(uvm_ariadne_pin_duration(base, 2000 * block, block, 1) == base);
    assert(uvm_ariadne_pin_duration(base, 4000 * block, block, 1) == base * 2);
    assert(uvm_ariadne_pin_duration(base, 4000 * block, block, 2) == base * 10);
    assert(uvm_ariadne_pin_duration(base, 2000 * block + 1, block, 1) == base + base / 2000);
    assert(uvm_ariadne_pin_duration(limit / 2, block, block, 1) == limit / 2);
    assert(uvm_ariadne_pin_duration(limit, block, block, 2) == limit);
    assert(uvm_ariadne_pin_duration(limit, limit, 1, 1) == limit);
    assert(!uvm_ariadne_pin_duration(0, block, block, 1));
    assert(!uvm_ariadne_pin_duration(base, block, 0, 1));
}

int main(void)
{
    test_utlb_history();
    test_sd_window_and_pinning();
    test_stream_against_recent_faults();
    test_working_set_budget_and_duration();
    puts("ARIADNE sharing and zero-copy policy tests passed");
    return 0;
}
