/* ePS6800 Keypad Emulation */

#include "cpu_internal.h"
#include "eps6800.h"
#include "kbd_internal.h"
#include "mmio_internal.h"

#include <string.h>

void kbd_connect_bus_state(struct kbd_state *state, struct cpu_state *cpu, struct mmio_state *mmio) {
	state->cpu = cpu;
	state->mmio = mmio;
	state->traits = eps_get_variant_traits(mmio->variant);
	state->profile = &state->traits->kbd;
}

enum {
	KEY_PRESS_DELAY_CYCLES = 1000,
	KEY_RELEASE_HOLD_CYCLES = 1000,
	KBD_MATRIX_IDLE = 0xff,
	KBD_ON_COLUMN_MASK = 0x80,
	KBD_KEY_COLUMNS_MASK = 0x7f,
	KBD_EPS6009_PORTA_MASK = 0x8f,
	KBD_EPS6009_PORTA_KEY_MASK = 0x0f,
	KBD_EPS6009_PORTB_MASK = 0x03,
	KBD_INVALID_READ_VALUE = 0xff,
	KBD_DCR_RESET = 0xff
};

enum kbd_matrix_model {
	KBD_MATRIX_MODEL_GPIO = 0,
	KBD_MATRIX_MODEL_EPS6009
};

/* CG100_PERF OPT-1: key-input-enable test using the cached traits pointer. */
static inline int kbd_key_input_enabled_cached(const struct kbd_state *state) {
	const struct eps_variant_traits *t = state->traits;
	if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009)
		return ((state->reg[t->reg_stbcon] & BIT_AUTO_KEY_SCAN) != 0) ||
			((state->reg[t->reg_pacon] & 0x01u) != 0);
	return (state->reg[t->reg_stbcon] & BIT_KEY_INPUT_ENABLE) != 0;
}

/* CG100_PERF OPT-1: refresh one key's bit in the activity bitmap. */
static inline void kbd_refresh_key_activity(struct kbd_state *state, uint8_t key) {
	const uint32_t bit = 1u << (key & 31u);
	const uint32_t idx = (uint32_t)key >> 5;
	if (state->key_pending_down[key] || state->key_pending_up[key] ||
		state->key_press_cycles[key] || state->key_release_cycles[key])
		state->event_mask[idx] |= bit;
	else
		state->event_mask[idx] &= ~bit;
}

/* CG100_PERF OPT-1: safe early-out.  The level-sensitive path can assert an
 * interrupt with no key held only when the EPS6009 PA configuration drives the
 * key bits low (pacon[2:1] == 0); keep the full tick in that rare case. */
static inline bool kbd_can_skip_tick(const struct kbd_state *state) {
	const struct eps_variant_traits *t = state->traits;
	if (state->on_pending_down || state->on_pending_up || state->pending_press_mask)
		return false;
	if ((state->event_mask[0] | state->event_mask[1] |
		 state->event_mask[2] | state->event_mask[3]) != 0)
		return false;
	if (state->on_pressed || state->key_matrix_active)
		return false;
	if ((state->reg[t->reg_paintsta] & state->reg[t->reg_painten]) != 0)
		return false;
	if (state->profile->level_sensitive_inputs && !(state->reg[t->reg_pacon] & 0x06u))
		return false;
	return true;
}

static uint8_t kbd_bus_read_internal(struct kbd_state *state, uint8_t addr) {
	return mmio_read_byte_internal_state(state->mmio, addr);
}

static void kbd_bus_write_internal(struct kbd_state *state, uint8_t addr, uint8_t byte) {
	mmio_write_byte_internal_state(state->mmio, addr, byte);
}

static bool kbd_cpu_is_sleep_repeating(const struct kbd_state *state) {
	return cpu_is_sleep_repeating_state(state->cpu);
}

static bool kbd_cpu_accepts_pending_press(const struct kbd_state *state) {
	if (state->profile->accept_explicit_sleep_mode) {
		/* ePS9500 SLEP advances PC before entering Sleep/Idle, unlike the
		 * legacy ePS6800 repeat-at-SLEP implementation.  It must therefore
		 * accept a matrix edge from the explicit CPU mode as ePS6009 does. */
		return state->cpu->mode == CPU_MODE_SLEEP ||
			state->cpu->mode == CPU_MODE_IDLE ||
			kbd_cpu_is_sleep_repeating(state);
	}
	return kbd_cpu_is_sleep_repeating(state);
}

static void kbd_cpu_interrupt(struct kbd_state *state, uint8_t int_level) {
	cpu_interrupt_state(state->cpu, int_level);
}

static void kbd_cpu_wake(struct kbd_state *state, uint8_t source) {
	cpu_wake_state(state->cpu, source);
}

static uint8_t kbd_scan_row_mask(uint8_t row) {
	return (uint8_t)(1 << row);
}

static uint8_t kbd_key_input_mask(const struct kbd_state *state) {
	return state->profile->key_input_mask;
}

/*
 * The keyboard is an undioded PA/PB switch matrix.  A closed switch joins one
 * PA line to one PB line, so three corners of a rectangle can pull the fourth
 * corner low while firmware is scanning.  Propagate the low level through the
 * bipartite graph instead of merely ANDing the selected PB rows; this also
 * preserves simultaneous keys in the same row/column.
 *
 * DCR bit 0 means output.  Inputs and high outputs do not seed a low level;
 * low outputs do.  In an output conflict the low level wins, matching the
 * conservative electrical behaviour expected by the calculator firmware.
 */
static uint8_t kbd_get_eps6009_porta_base(const struct kbd_state *state) {
	uint8_t pa = 0;
	const uint8_t pacon = state->reg[state->traits->reg_pacon] & 0x0fu;

	if (pacon & 0x08u)
		pa |= KBD_ON_COLUMN_MASK;
	if (pacon & 0x06u)
		pa |= KBD_EPS6009_PORTA_KEY_MASK;
	return pa;
}

static uint8_t kbd_get_matrix(const struct kbd_state *state) {
	if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009) {
		uint8_t pa = kbd_get_eps6009_porta_base(state);
		const uint8_t strobe = state->reg[state->traits->reg_stbcon];
		if (kbd_key_input_enabled_cached(state)) {
			if ((strobe & BIT_AUTO_KEY_SCAN) || (strobe & BIT_ALL_STROBES)) {
				for (uint8_t row = 0; row < KBD_ROW_COUNT; ++row)
					pa &= (uint8_t)(state->matrix[row] | (uint8_t)~KBD_EPS6009_PORTA_KEY_MASK);
			}
			else if ((strobe & 0x0fu) < KBD_ROW_COUNT) {
				pa &= (uint8_t)(state->matrix[strobe & 0x0fu] | (uint8_t)~KBD_EPS6009_PORTA_KEY_MASK);
			}
		}
		if (state->on_pressed)
			pa &= (uint8_t)~KBD_ON_COLUMN_MASK;
		return (uint8_t)(pa & KBD_EPS6009_PORTA_MASK);
	}

	uint8_t low_pa = (uint8_t)~(state->reg[REG_DCRA] | state->porta_latch);
	uint8_t low_pb = (uint8_t)~(state->reg[REG_DCRB] | state->portb_latch);
	bool changed;

	do {
		int pb;
		uint8_t next_low_pa = low_pa;
		uint8_t next_low_pb = low_pb;

		for (pb = 0; pb < KBD_ROW_COUNT; ++pb) {
			const uint8_t closed_pa = (uint8_t)~state->matrix[pb];
			if (low_pb & kbd_scan_row_mask((uint8_t)pb)) {
				next_low_pa |= closed_pa;
			}
			if (closed_pa & low_pa) {
				next_low_pb |= kbd_scan_row_mask((uint8_t)pb);
			}
		}

		changed = next_low_pa != low_pa || next_low_pb != low_pb;
		low_pa = next_low_pa;
		low_pb = next_low_pb;
	} while (changed);

	if (state->on_pressed) {
		low_pa |= KBD_ON_COLUMN_MASK;
	}
	return (uint8_t)~low_pa;
}

static uint8_t kbd_get_pa(const struct kbd_state *state) {
	if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009)
		return kbd_get_matrix(state);
	uint8_t input = kbd_get_matrix(state);
	uint8_t pa = (input & state->reg[REG_DCRA]) |
		(state->porta_latch & ~state->reg[REG_DCRA]);
	/* PA7 is the dedicated active-low ON input. Unlike PA0-PA6 matrix
	 * contacts, its physical level remains readable when firmware changes
	 * the ordinary Port A GPIO direction during shutdown. Official ePS9500
	 * sub_4077D0/sub_4255B0 drives PA7 directly for On/C; PB0 is the paired
	 * host event field, not a reason to route PA7 through the ordinary scan. */
	if (state->on_pressed)
		pa &= (uint8_t)~KBD_ON_COLUMN_MASK;
	else
		pa |= KBD_ON_COLUMN_MASK;
	return pa;
}

static void kbd_update_porta(struct kbd_state *state) {
	uint8_t pa = kbd_get_pa(state);
	const uint8_t reg_porta = state->traits->reg_porta;

	state->reg[reg_porta] = pa;
	kbd_bus_write_internal(state, reg_porta, pa);
}

static void kbd_update_eps6009_portb(struct kbd_state *state) {
	const enum eps_variant variant = state->mmio->variant;
	const uint8_t dcrb = state->reg[state->traits->reg_dcrb] & KBD_EPS6009_PORTB_MASK;
	const uint8_t pbcon = state->reg[state->traits->reg_pbcon] & KBD_EPS6009_PORTB_MASK;
	const uint8_t latch = state->portb_latch & KBD_EPS6009_PORTB_MASK;
	/* F-715SG-family firmware samples PB1:PB0 as model-selection straps at
	 * boot, then changes the same pins back to keyboard row outputs. */
	const uint8_t external = (uint8_t)(
		(state->portb_input_value & state->portb_input_mask) |
		(pbcon & (uint8_t)~state->portb_input_mask));
	const uint8_t pb = (uint8_t)((latch & (uint8_t)~dcrb) | (external & dcrb));

	state->reg[state->traits->reg_portb] = pb;
	kbd_bus_write_internal(state, state->traits->reg_portb, pb);
}

static uint8_t kbd_active_column_mask(const struct kbd_state *state) {
	int row;
	uint8_t mask = 0;
	for (row = 0; row < KBD_ROW_COUNT; row++) {
		mask |= (uint8_t)~state->matrix[row];
	}
	if (state->on_pressed) {
		mask |= KBD_ON_COLUMN_MASK;
	}
	return mask;
}

static void kbd_trigger_press(struct kbd_state *state, uint8_t mask) {
	const enum eps_variant variant = state->mmio->variant;
	uint8_t interrupt_mask = mask & state->reg[state->traits->reg_painten];
	uint8_t wake_mask = mask & state->reg[state->traits->reg_pawake];
	uint8_t key_wake_mask = kbd_key_input_enabled_cached(state)
		? (mask & kbd_key_input_mask(state))
		: 0;
	if (interrupt_mask) {
		state->reg[state->traits->reg_paintsta] |= interrupt_mask;
		kbd_bus_write_internal(state, state->traits->reg_paintsta, state->reg[state->traits->reg_paintsta]);
		if (kbd_bus_read_internal(state, state->traits->reg_cpucon) & BIT_GLINT) {
			kbd_cpu_interrupt(state, INT_LEVEL1_PAINT);
			return;
		}
	}
	if (wake_mask || key_wake_mask) {
		kbd_cpu_wake(state, WAKE_PAINT);
	}
}

static void kbd_queue_press(struct kbd_state *state, uint8_t mask) {
	state->pending_press_mask |= mask;
}

static void kbd_sync_latched_interrupt(struct kbd_state *state) {
	const enum eps_variant variant = state->mmio->variant;
	/* A key can wake firmware with GLINT masked. PAINTSTA retains that
	 * event even after key release; deliver it when firmware unmasks the
	 * interrupt, unless it has cleared the status or disabled PAINTEN.
	 * Do not reassert inside the ISR, where GLINT is masked again. */
	if ((state->reg[state->traits->reg_paintsta] & state->reg[state->traits->reg_painten]) &&
		(kbd_bus_read_internal(state, state->traits->reg_cpucon) & BIT_GLINT)) {
		kbd_cpu_interrupt(state, INT_LEVEL1_PAINT);
	}
}

static bool kbd_key_valid(uint8_t key) {
	return key < KBD_KEY_COUNT;
}

static uint8_t kbd_key_row(uint8_t key) {
	return key / KBD_COL_COUNT;
}

static uint8_t kbd_key_col(uint8_t key) {
	return key % KBD_COL_COUNT;
}

static uint8_t kbd_key_mask(uint8_t key) {
	return (uint8_t)(1 << kbd_key_col(key));
}

static bool kbd_countdown_elapsed(uint16_t *counter, uint32_t cycles) {
	if (cycles >= *counter) {
		*counter = 0;
		return true;
	}

	*counter = (uint16_t)(*counter - cycles);
	return false;
}

static uint16_t kbd_press_delay_cycles(const struct kbd_state *state) {
	return state->profile->press_delay_cycles;
}

static void kbd_process_pending_press(struct kbd_state *state) {
	uint8_t active_mask = kbd_active_column_mask(state);
	uint8_t ready_mask;
	const enum eps_variant variant = state->mmio->variant;

	state->pending_press_mask &= active_mask;
	ready_mask = state->pending_press_mask & (state->reg[state->traits->reg_painten] | state->reg[state->traits->reg_pawake] |
		(kbd_key_input_enabled_cached(state) ? kbd_key_input_mask(state) : 0));
	if (!ready_mask || !kbd_cpu_accepts_pending_press(state)) {
		return;
	}

	state->pending_press_mask &= ~ready_mask;
	kbd_trigger_press(state, ready_mask);
}

/*
 * ePS6009 key inputs are level-sensitive.  The reference implementation
 * (the official Canon F-715SG emulator's embedded CIce core) re-synchronises
 * the key state on every instruction: while a PAINTEN bit is enabled and the
 * corresponding PORTA input reads low, the sticky PAINTSTA bit is asserted
 * and the PAINT interrupt is requested — regardless of whether the CPU is
 * running, idle, or sleeping.  The CPU takes the request whenever GLINT is
 * set, and the firmware's interrupt handler masks PAINTEN while it drains
 * the event, which stops the assertion until it re-enables the key inputs.
 *
 * The same level, gated by PAWAKE, wakes the CPU from Idle/Sleep so that a
 * key held across a sleep entry cannot strand the press.  Without this, a
 * key pressed while the CPU runs is never seen: the F-715SG firmware is
 * fully interrupt-driven and only scans the matrix from the PAINT interrupt
 * service routine.
 */
static void kbd_sync_level_sensitive_inputs(struct kbd_state *state) {
	uint8_t asserted;

	if (!state->profile->level_sensitive_inputs)
		return;

	const uint8_t pa = kbd_get_pa(state);
	asserted = (uint8_t)(state->reg[state->traits->reg_painten] &
		(uint8_t)~pa & KBD_EPS6009_PORTA_MASK);
	if (asserted) {
		state->reg[state->traits->reg_paintsta] |= asserted;
		kbd_bus_write_internal(state, state->traits->reg_paintsta,
			state->reg[state->traits->reg_paintsta]);
		kbd_cpu_interrupt(state, INT_LEVEL1_PAINT);
	}

	if (kbd_cpu_accepts_pending_press(state)) {
		asserted = (uint8_t)(state->reg[state->traits->reg_pawake] &
			(uint8_t)~pa & KBD_EPS6009_PORTA_MASK);
		if (asserted) {
			kbd_cpu_wake(state, WAKE_PAINT);
		}
	}
}

static void kbd_schedule_release_hold(struct kbd_state *state, uint8_t key) {
	state->key_pending_up[key] = false;
	state->key_release_cycles[key] = KEY_RELEASE_HOLD_CYCLES;
	kbd_refresh_key_activity(state, key);
}

uint8_t kbd_read_byte_state(struct kbd_state *state, uint8_t addr) {
	uint8_t byte;
	if (addr < KBD_REG_COUNT) {
		if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009) {
			if (addr == state->traits->reg_porta) {
				kbd_update_porta(state);
				return state->reg[addr];
			}
			if (addr == state->traits->reg_portb) {
				kbd_update_eps6009_portb(state);
				return state->reg[addr];
			}
			return state->reg[addr];
		}
		switch (addr) {
		case REG_PORTA:
			kbd_update_porta(state);
			byte = state->reg[REG_PORTA];
			break;
		case REG_PORTB:
			byte = state->portb_latch;
			state->reg[REG_PORTB] = byte;
			break;
		case REG_PORTC:
			byte = (uint8_t)(
				(state->portc_latch & (uint8_t)~state->reg[REG_DCRC]) |
				(((state->portc_input_value & state->portc_input_mask) |
					(state->portc_latch & (uint8_t)~state->portc_input_mask)) & state->reg[REG_DCRC]));
			break;
		default:
			byte = state->reg[addr];
		}
	}
	else {
		byte = KBD_INVALID_READ_VALUE;
		mmio_bad_read_byte_state(state->mmio, addr);
	}
	return byte;
}

void kbd_write_byte_state(struct kbd_state *state, uint8_t addr, uint8_t byte) {
	if (addr < KBD_REG_COUNT) {
		state->reg[addr] = byte;
		if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009) {
			if (addr == state->traits->reg_porta) {
				state->porta_latch = byte & KBD_EPS6009_PORTA_MASK;
				kbd_update_porta(state);
			}
			else if (addr == state->traits->reg_portb) {
				state->portb_latch = byte & KBD_EPS6009_PORTB_MASK;
				kbd_update_eps6009_portb(state);
			}
			else if (addr == state->traits->reg_stbcon ||
				addr == state->traits->reg_painten ||
				addr == state->traits->reg_pawake ||
				addr == state->traits->reg_pacon ||
				addr == state->traits->reg_dcrb) {
				if (addr == state->traits->reg_pacon) {
					state->reg[addr] = byte & 0x0fu;
					kbd_update_porta(state);
				}
				else if (addr == state->traits->reg_dcrb) {
					state->reg[addr] = byte & KBD_EPS6009_PORTB_MASK;
					kbd_update_eps6009_portb(state);
				}
				kbd_process_pending_press(state);
			}
			else if (addr == state->traits->reg_pbcon) {
				state->reg[addr] = byte & KBD_EPS6009_PORTB_MASK;
				kbd_update_eps6009_portb(state);
			}
			else if (addr == state->traits->reg_paintsta) {
				kbd_bus_write_internal(state, state->traits->reg_paintsta, state->reg[addr]);
			}
			return;
		}
		switch (addr) {
		case REG_PORTA:
			state->porta_latch = byte;
			break;
		case REG_PORTB:
			state->portb_latch = byte;
			break;
		case REG_PORTC:
			state->portc_latch = byte;
			break;
		case REG_DCRA:
		case REG_DCRB:
		case REG_STBCON:
		case REG_PAINTEN:
		case REG_PAWAKE:
			kbd_process_pending_press(state);
			break;
		case REG_PAINTSTA:
			kbd_bus_write_internal(state, REG_PAINTSTA, state->reg[REG_PAINTSTA]);
			break;
		}
	}
	else {
		mmio_bad_write_byte_state(state->mmio, addr);
	}
}

void kbd_keydown_state(struct kbd_state *state, uint8_t key) {
	if (!kbd_key_valid(key)) {
		return;
	}

	state->key_release_cycles[key] = 0;
	state->key_pending_down[key] = true;
	state->key_pending_up[key] = false;
	state->key_press_cycles[key] = kbd_press_delay_cycles(state);
	kbd_refresh_key_activity(state, key);
}

static void kbd_activate_key(struct kbd_state *state, uint8_t key) {
	if (!kbd_key_valid(key)) {
		return;
	}

	uint8_t row = kbd_key_row(key);
	uint8_t mask = kbd_key_mask(key);
	if (state->matrix[row] & mask) {
		state->matrix[row] &= ~mask;
		state->key_matrix_active = true;
		kbd_queue_press(state, mask);
	}
	kbd_refresh_key_activity(state, key);
}

static void kbd_release_key(struct kbd_state *state, uint8_t key) {
	if (!kbd_key_valid(key)) {
		return;
	}

	uint8_t row = kbd_key_row(key);
	state->matrix[row] |= kbd_key_mask(key);
	state->key_matrix_active = false;
	for (int r = 0; r < KBD_ROW_COUNT; ++r) {
		if (state->matrix[r] != (uint8_t)KBD_MATRIX_IDLE) {
			state->key_matrix_active = true;
			break;
		}
	}
	kbd_refresh_key_activity(state, key);
}

void kbd_keyup_state(struct kbd_state *state, uint8_t key) {
	if (!kbd_key_valid(key)) {
		return;
	}

	uint8_t row = kbd_key_row(key);
	uint8_t mask = kbd_key_mask(key);
	if (state->key_pending_down[key]) {
		state->key_pending_up[key] = true;
		kbd_refresh_key_activity(state, key);
		return;
	}
	if (!(state->matrix[row] & mask)) {
		kbd_schedule_release_hold(state, key);
	}
}

static void kbd_process_pending_on_press(struct kbd_state *state, uint32_t cycles) {
	if (state->on_pending_down) {
		if (kbd_countdown_elapsed(&state->on_press_cycles, cycles)) {
			state->on_pressed = true;
			/* The official mode-4 host writes the PA7 level into SFR 31h before
			 * waking the interpreter. Some ROM bit-test paths sample the latched
			 * SFR directly rather than invoking the GPIO read callback. */
			if (state->profile->refresh_on_contact_level)
				kbd_update_porta(state);
			kbd_cpu_wake(state, WAKE_ON);
			kbd_trigger_press(state, KBD_ON_COLUMN_MASK);
			state->on_pending_down = false;
			if (state->on_pending_up) {
				state->on_press_cycles = KEY_RELEASE_HOLD_CYCLES;
			}
		}
	}
}

void kbd_restore_keydown_state(struct kbd_state *state, uint8_t key) {
	if (!kbd_key_valid(key)) {
		return;
	}

	/* This path represents a contact that remained physically closed while
	 * ON reset the CPU/SFRs. It must be visible to the reset-vector scan
	 * immediately instead of being treated as a fresh, bouncing key press. */
	state->key_pending_down[key] = false;
	state->key_pending_up[key] = false;
	state->key_press_cycles[key] = 0;
	state->key_release_cycles[key] = 0;
	kbd_refresh_key_activity(state, key);
	kbd_activate_key(state, key);
}

static void kbd_process_pending_on_release(struct kbd_state *state, uint32_t cycles) {
	if (state->on_pending_up && !state->on_pending_down &&
		kbd_countdown_elapsed(&state->on_press_cycles, cycles)) {
		state->on_pressed = false;
		if (state->profile->refresh_on_contact_level)
			kbd_update_porta(state);
		state->on_pending_up = false;
	}
}

static void kbd_process_pending_key_press(
	struct kbd_state *state,
	uint8_t key,
	uint32_t cycles
) {
	if (!state->key_pending_down[key]) {
		return;
	}

	if (cycles < state->key_press_cycles[key]) {
		kbd_countdown_elapsed(&state->key_press_cycles[key], cycles);
		return;
	}

	state->key_pending_down[key] = false;
	state->key_press_cycles[key] = 0;
	kbd_refresh_key_activity(state, key);
	kbd_activate_key(state, key);
	if (state->key_pending_up[key]) {
		kbd_schedule_release_hold(state, key);
	}
}

static void kbd_process_pending_key_presses(struct kbd_state *state, uint32_t cycles) {
	int key;

	for (key = 0; key < KBD_KEY_COUNT; key++) {
		kbd_process_pending_key_press(state, (uint8_t)key, cycles);
	}
}

static void kbd_process_pending_key_release(struct kbd_state *state, uint8_t key, uint32_t cycles) {
	if (!state->key_release_cycles[key]) {
		return;
	}

	if (kbd_countdown_elapsed(&state->key_release_cycles[key], cycles)) {
		kbd_release_key(state, key);
	}
}

static void kbd_process_pending_key_releases(struct kbd_state *state, uint32_t cycles) {
	int key;

	for (key = 0; key < KBD_KEY_COUNT; key++) {
		kbd_process_pending_key_release(state, (uint8_t)key, cycles);
	}
}

void kbd_tick_state(struct kbd_state *state, uint32_t cycles) {
	/* CG100_PERF OPT-1: an idle keyboard needs no per-instruction servicing. */
	if (kbd_can_skip_tick(state))
		return;
	kbd_process_pending_on_press(state, cycles);
	kbd_process_pending_on_release(state, cycles);
	kbd_process_pending_key_presses(state, cycles);
	kbd_process_pending_press(state);
	kbd_process_pending_key_releases(state, cycles);
	kbd_process_pending_press(state);
	/* sub_4255B0 continuously reapplies the active mode-4 host key level;
	 * firmware GPIO writes must not permanently erase a held On/C input. */
	if (state->profile->refresh_on_contact_level && state->on_pressed)
		kbd_update_porta(state);
	kbd_sync_level_sensitive_inputs(state);
	kbd_sync_latched_interrupt(state);
}

void kbd_ondown_state(struct kbd_state *state) {
	if (!state->on_pressed) {
		state->on_pending_down = true;
	}
	state->on_pending_up = false;
	state->on_press_cycles = kbd_press_delay_cycles(state);
}

void kbd_onup_state(struct kbd_state *state) {
	state->on_pending_up = true;
	if (!state->on_pending_down)
		state->on_press_cycles = KEY_RELEASE_HOLD_CYCLES;
}

void kbd_set_portc_input_state(struct kbd_state *state, uint8_t mask, uint8_t value) {
	state->portc_input_mask = mask;
	state->portc_input_value = (uint8_t)(value & mask);
}

void kbd_set_portb_input_state(struct kbd_state *state, uint8_t mask, uint8_t value) {
	state->portb_input_mask = (uint8_t)(mask & KBD_EPS6009_PORTB_MASK);
	state->portb_input_value = (uint8_t)(value & state->portb_input_mask);
	if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009)
		kbd_update_eps6009_portb(state);
}

static void kbd_clear_storage(struct kbd_state *state) {
	memset(state->reg, 0, sizeof(state->reg));
	memset(state->matrix, KBD_MATRIX_IDLE, sizeof(state->matrix));
	memset(state->key_pending_down, 0, sizeof(state->key_pending_down));
	memset(state->key_pending_up, 0, sizeof(state->key_pending_up));
	memset(state->key_press_cycles, 0, sizeof(state->key_press_cycles));
	memset(state->key_release_cycles, 0, sizeof(state->key_release_cycles));
	memset(state->event_mask, 0, sizeof(state->event_mask));
	state->key_matrix_active = false;
}

static void kbd_apply_reset_defaults(struct kbd_state *state) {
	state->pending_press_mask = 0;
	if (state->profile->matrix_model == KBD_MATRIX_MODEL_EPS6009) {
		state->reg[state->traits->reg_pacon] = 0x0eu;
		state->reg[state->traits->reg_pbcon] = 0x00u;
		state->reg[state->traits->reg_dcrb] = 0x03u;
		state->porta_latch = KBD_EPS6009_PORTA_MASK;
		state->portb_latch = KBD_EPS6009_PORTB_MASK;
		kbd_update_porta(state);
		kbd_update_eps6009_portb(state);
	}
	else {
		state->reg[REG_DCRA] = KBD_DCR_RESET;
		state->reg[REG_DCRB] = KBD_DCR_RESET;
		state->reg[REG_DCRC] = KBD_DCR_RESET;
		state->porta_latch = 0;
		state->portb_latch = 0;
	}
	state->portc_latch = 0;
	state->on_pressed = false;
	state->on_pending_down = false;
	state->on_pending_up = false;
	state->on_press_cycles = 0;
}

void kbd_reset_state(struct kbd_state *state) {
	kbd_clear_storage(state);
	kbd_apply_reset_defaults(state);
}
