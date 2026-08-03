/*
    led_integrity.h - Single header LED tamper detection library
    Copyright (c) 2026, James Collins.
    This code is released freely under the MIT License (see LICENSE file for details).
    Include this header in your project, implement the platform hooks outlined below,
    wire a rising edge interrupt on a GPIO pin connected to the LED DO line and call
    li_signal_edge() from the ISR.
*/

#ifndef LED_INTEGRITY_H
#define LED_INTEGRITY_H

#include <stdbool.h>
#include <stdint.h>

// Propagation delay window (microseconds).
// These defaults suit a single WS2812B LED. Adjust for different models and chain lengths.
// To determine accurately, use an oscilloscope or logic analyzer, as different LED models may behave differently.
#ifndef LI_PROP_DELAY_MIN
#define LI_PROP_DELAY_MIN  20u
#endif

#ifndef LI_PROP_DELAY_MAX
#define LI_PROP_DELAY_MAX  100u
#endif

// Callback type for tamper event
// Implement your tamper handler and pass it to li_init().
typedef void (*li_callback_fn)(void);


//state structure for LED tamper protection
typedef struct {
    uint32_t       probe_window;        // duration of the probe acceptance window in microseconds
    uint32_t       max_missed_probes;   // number of missed probes before tamper is declared
    uint32_t       probe_open_time;     // timestamp when the window was opened
    volatile uint32_t       probe_send_time;     // timestamp passed to li_probe()
    uint32_t       missed_probes;       // current missed probe count
    volatile bool  window_open;         // true during the probe acceptance window
    volatile bool  edge_valid;          // set by ISR when a valid edge is received
    volatile bool  tampered;            // latches true once tamper is declared
    volatile bool  has_probed;          // set after the first li_probe() call
    li_callback_fn on_tamper;          // callback for tamper event
} li_state_t;

/*
Platform hooks — implement both of these once in your application.

- li_send_probe()
  Send one extra frame down the LED chain to toggle the DO line.
  Restore the current display state at the end of this function if your
  probe frame differs from it (see examples).

- li_get_tick()
  Return the current time in microseconds.
  Called only from non-ISR context (li_init, li_probe, li_tick).

(examples in examples/ directory)
*/
extern void     li_send_probe(void);
extern uint32_t li_get_tick(void);

// Passes the tamper event to the application for handling
static inline void li_fire_tamper(li_state_t *state){
    state->tampered = true;
    state->window_open = false;
    if(state->on_tamper != NULL){
        state->on_tamper();
    }
}

//Initializes the li_state_t structure
static inline void li_init(li_state_t *state,
                            uint32_t probe_window_us,
                            uint32_t max_missed_probes,
                            li_callback_fn callback)
{
    state->probe_window       = probe_window_us;
    state->max_missed_probes  = max_missed_probes;
    state->on_tamper          = callback;
    state->probe_open_time    = 0;
    state->probe_send_time    = 0;
    state->missed_probes      = 0;
    state->tampered           = false;
    state->window_open        = false;
    state->edge_valid         = false;
    state->has_probed         = false;
}

//Open the interrupt acceptance window and send the probe frame
static inline void li_probe(li_state_t *state, uint32_t timestamp)
{
    if (state->tampered) {
        return;
    }
    state->probe_send_time = timestamp;
    state->probe_open_time = li_get_tick();
    state->edge_valid = false;
    state->window_open = true;
    state->has_probed = true;
    li_send_probe();
}

//Call from the rising-edge ISR on the LED DO pin
static inline void li_signal_edge(li_state_t *state, uint32_t timestamp)
{
    if (state->tampered) {
        return;
    }

    if (!state->window_open) {
        // Edge arrived outside any acceptance window — unexpected DO activity.
        if (state->has_probed) {
            li_fire_tamper(state);
        }
        return;
    }

    uint32_t delay = timestamp - state->probe_send_time;

    if (delay < LI_PROP_DELAY_MIN || delay > LI_PROP_DELAY_MAX) {
        // Propagation delay out of range: direct bridge or unexpected driver.
        li_fire_tamper(state);
        return;
    }

    // Valid edge received within window.
    state->edge_valid = true;
    state->window_open = false;
}

static inline void li_tick(li_state_t *state)
{
    if (state->tampered || !state->has_probed) {
        return;
    }

    // Harvest a valid edge flagged by the ISR
    if (state->edge_valid) {
        state->edge_valid    = false;
        state->missed_probes = 0;
    }

    uint32_t now = li_get_tick();

    // Check for probe timeout
    if (state->window_open && (now - state->probe_open_time >= state->probe_window)) {
        state->window_open = false;
        state->missed_probes++;
        if (state->missed_probes >= state->max_missed_probes) {
            li_fire_tamper(state);
            return;
        }
    }
}

// Function to reset the tamper state, e.g. on LED reinstallation or system reset
static inline void li_reset(li_state_t *state)
{
    state->tampered      = false;
    state->window_open   = false;
    state->edge_valid    = false;
    state->has_probed    = false;
    state->missed_probes = 0;
}

#endif /* LED_INTEGRITY_H */
