#include "podcast_idle.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

static podcast_idle_input_t listening_away(int64_t idle_us)
{
    return (podcast_idle_input_t){
        .now_us = INT64_C(100000000) + idle_us,
        .last_input_us = INT64_C(100000000),
        .screen_off_seconds = PODCAST_IDLE_SCREEN_OFF_SECONDS,
        .live_playback = true,
        .away_from_player = true,
        .screen_awake = true,
    };
}

static void test_deadlines_and_activity(void)
{
    podcast_idle_decision_t d = podcast_idle_decide(listening_away(6999999));
    assert(!d.return_to_player && !d.turn_screen_off);
    d = podcast_idle_decide(listening_away(7000000));
    assert(d.return_to_player && !d.turn_screen_off);
    d = podcast_idle_decide(listening_away(14999999));
    assert(d.return_to_player && !d.turn_screen_off);
    d = podcast_idle_decide(listening_away(15000000));
    assert(d.return_to_player && d.turn_screen_off);

    podcast_idle_input_t in = listening_away(15000000);
    in.last_input_us = in.now_us;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && !d.turn_screen_off);
    in.now_us += PODCAST_IDLE_RETURN_US;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && !d.turn_screen_off);
}

static void test_playback_and_protected_boundaries(void)
{
    podcast_idle_input_t in = listening_away(15000000);
    in.live_playback = false; /* Paused, finished, mismatched, or no episode. */
    podcast_idle_decision_t d = podcast_idle_decide(in);
    assert(!d.return_to_player && d.turn_screen_off);
    in.live_playback = true;
    in.away_from_player = false;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && d.turn_screen_off);
    in.away_from_player = true;
    in.protected_view = true;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && d.turn_screen_off);
    in.protected_view = false;
    in.screen_awake = false;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && !d.turn_screen_off);
    in.screen_awake = true;
    in.screen_off_seconds = 0;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && !d.turn_screen_off);
    in.screen_off_seconds = 30;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && !d.turn_screen_off);
    in.now_us += 15000000;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && d.turn_screen_off);
}

static void test_clock_limits(void)
{
    podcast_idle_input_t in = listening_away(0);
    in.now_us = in.last_input_us - 1;
    podcast_idle_decision_t d = podcast_idle_decide(in);
    assert(!d.return_to_player && !d.turn_screen_off);
    in.now_us = -1;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && !d.turn_screen_off);
    in.now_us = 1;
    in.last_input_us = -1;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && !d.turn_screen_off);

    in.last_input_us = 0;
    in.now_us = INT64_MAX;
    in.screen_off_seconds = UINT_MAX;
    d = podcast_idle_decide(in);
    assert(d.return_to_player && d.turn_screen_off);
    in.last_input_us = INT64_MAX - 6999999;
    d = podcast_idle_decide(in);
    assert(!d.return_to_player && !d.turn_screen_off);
}

static void test_wake_click_double_and_long(void)
{
    podcast_idle_wake_t guard = {0};
    podcast_idle_key_decision_t k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(1000000));
    assert(!k.request_wake && !k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(1000000));
    assert(!k.request_wake && !k.consume);

    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume); /* Second half of a double click. */
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_DOUBLE, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(1000000));
    assert(!k.request_wake && !k.consume);

    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_LONG, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_LONG);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_LONG, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(1000000));
    assert(!k.request_wake && !k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(1000000));
    assert(!k.request_wake && !k.consume);
}

static void test_wake_deferred_worker_and_fallback(void)
{
    podcast_idle_wake_t guard = {0};
    podcast_idle_key_decision_t k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_LONG, false, INT64_C(1000000));
    assert(!k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume); /* Wake IO has not completed yet. */
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, false, INT64_C(1000000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);

    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, false, INT64_C(1000000));
    assert(k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_DOUBLE, false, INT64_C(1000000));
    assert(k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_LONG, false, INT64_C(1000000));
    assert(k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_LONG);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(1000000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_OTHER, false, INT64_C(1000000));
    assert(!k.request_wake && !k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(NULL, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(!k.request_wake && !k.consume);
}

static void test_unexposed_multiple_click_terminal(void)
{
    podcast_idle_wake_t guard = {0};
    podcast_idle_key_decision_t k = podcast_idle_wake_input(
        &guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(1000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(1100000));
    assert(!k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(1200000));
    assert(!k.request_wake && k.consume);
    /* The BSP has no terminal callback for a triple click. A later gesture
     * still works; repeated presses cannot leave controls suppressed forever. */
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(2099999));
    assert(!k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(2999999));
    assert(!k.request_wake && !k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_CLICK, true, INT64_C(3200000));
    assert(!k.request_wake && !k.consume);

    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(4000000));
    assert(k.request_wake && k.consume);
    /* A second press can follow a just-under-500ms first hold and a nearly
     * 180ms inter-click wait; it is still part of the wake double click. */
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(4680000));
    assert(!k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_DOUBLE, true, INT64_C(4990000));
    assert(!k.request_wake && k.consume && guard.phase == PODCAST_IDLE_WAKE_NONE);

    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, false, INT64_C(5000000));
    assert(k.request_wake && k.consume);
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_PRESS, true, INT64_C(4999999));
    assert(!k.request_wake && k.consume); /* Regressed clock never opens controls. */
    k = podcast_idle_wake_input(&guard, PODCAST_IDLE_KEY_LONG, true, INT64_C(7000000));
    assert(!k.request_wake && k.consume); /* Long suppression is not time based. */
}

int main(void)
{
    test_deadlines_and_activity();
    test_playback_and_protected_boundaries();
    test_clock_limits();
    test_wake_click_double_and_long();
    test_wake_deferred_worker_and_fallback();
    test_unexposed_multiple_click_terminal();
    puts("Podcast idle: 7-second return, 15-second screen-off, fresh input, protected editing, clock bounds, whole wake gesture PASS");
    return 0;
}
