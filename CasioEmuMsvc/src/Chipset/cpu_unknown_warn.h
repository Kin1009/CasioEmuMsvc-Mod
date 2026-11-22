/* CG100_PERF OPT-3: diagnostics for undecodable opcodes.
 *
 * The original CPU::Next() executed one libc printf() for EVERY undecodable
 * opcode.  A malformed/empty ROM produced 35.17 M log lines in 8 s (~4.4 M
 * printf/s), i.e. a per-instruction host cost and a log-flood DoS.
 *
 * Default build: no call at all (the inline helper is empty).
 * Define CPUEMU_WARN_UNKNOWN_INST to re-enable diagnostics, capped at 16
 * distinct warnings per process so a bad image cannot flood the log.
 */
#ifndef CG100_PERF_CPU_UNKNOWN_WARN_H
#define CG100_PERF_CPU_UNKNOWN_WARN_H

#include <cstdint>

#if defined(CPUEMU_WARN_UNKNOWN_INST)
#include <cstdio>
inline void cpu_warn_unknown_inst(uint32_t pc, uint32_t opcode) {
	enum { CPUEMU_MAX_UNKNOWN_WARNINGS = 16 };
	static uint32_t warned_count = 0;
	if (warned_count >= CPUEMU_MAX_UNKNOWN_WARNINGS)
		return;
	++warned_count;
	std::fprintf(stderr,
		"[CPU][Warn] Unknown Inst at 0x%x: %x (further warnings capped at %d)\n",
		pc, opcode, CPUEMU_MAX_UNKNOWN_WARNINGS);
}
#else
inline void cpu_warn_unknown_inst(uint32_t, uint32_t) {}
#endif

#endif /* CG100_PERF_CPU_UNKNOWN_WARN_H */
