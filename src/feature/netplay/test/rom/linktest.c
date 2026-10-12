/* linktest.gba: a link-cable workload for netplay determinism tests.
 * SPDX-License-Identifier: MIT
 *
 * Mirrors the shape of pokeemerald's link layer (src/link.c): SIO
 * Multiplayer mode at 115200 bps, and on every VBlank the parent starts a
 * burst of 9 transfers spaced by Timer 3 (reload -197, /64 prescaler, about
 * 0.75 ms apart). Every console sends a word derived from its own keys and
 * transfer count, and folds all four received words into a running hash.
 * Since every console receives the same SIOMULTI0-3 for each transfer, the
 * hashes match across consoles exactly when the link works.
 *
 * Results live at a fixed EWRAM address (see struct LinkTestResult) so a
 * host can read them back through the bus. The backdrop colour shows the
 * low bits of the hash, so a frame dump shows at a glance whether consoles
 * agree.
 *
 * Lag counters follow pokeemerald's checks: the parent counts a lag when a
 * burst has not finished by the next VBlank, a child counts one when more
 * than 10 VBlanks pass without a serial interrupt once the link has run.
 */
#include <stdint.h>

#define REG16(A) (*(volatile uint16_t*) (A))
#define REG32(A) (*(volatile uint32_t*) (A))

#define REG_DISPCNT REG16(0x04000000)
#define REG_DISPSTAT REG16(0x04000004)
#define REG_TM3CNT_L REG16(0x0400010C)
#define REG_TM3CNT_H REG16(0x0400010E)
#define REG_SIOMULTI(N) REG16(0x04000120 + 2 * (N))
#define REG_SIOCNT REG16(0x04000128)
#define REG_SIOMLT_SEND REG16(0x0400012A)
#define REG_KEYINPUT REG16(0x04000130)
#define REG_RCNT REG16(0x04000134)
#define REG_IE REG16(0x04000200)
#define REG_IF REG16(0x04000202)
#define REG_IME REG16(0x04000208)
#define BIOS_IF REG16(0x03007FF8)
#define BG_PALETTE ((volatile uint16_t*) 0x05000000)

#define IRQ_VBLANK 0x0001
#define IRQ_TIMER3 0x0040
#define IRQ_SERIAL 0x0080

#define SIO_115200 0x0003
#define SIO_SI 0x0004
#define SIO_SD 0x0008
#define SIO_ID_SHIFT 4
#define SIO_ERROR 0x0040
#define SIO_START 0x0080
#define SIO_MODE_MULTI 0x2000
#define SIO_IRQ 0x4000

#define TIMER_ENABLE 0x0080
#define TIMER_IRQ 0x0040
#define TIMER_64CLK 0x0001
#define TIMER_RELOAD ((uint16_t) -197)

#define BURST_LENGTH 9
#define RESULT_MAGIC 0x544C504E /* "NPLT" */

/* Keep in sync with src/feature/netplay/test/session.c. */
struct LinkTestResult {
	uint32_t magic;
	uint32_t frames;
	uint32_t transfers;
	uint32_t hash;
	uint32_t lagParent;
	uint32_t lagChild;
	uint32_t bursts;
	uint32_t errors;
	uint32_t playerId;
	uint16_t lastMulti[4];
};

#define RESULT ((volatile struct LinkTestResult*) 0x02000000)

static uint32_t burstLeft;
static uint32_t sequence;
static uint32_t vblanksSinceSerial;
static uint32_t linked;

static uint16_t nextWord(void) {
	uint32_t keys = ~REG_KEYINPUT & 0x3FF;
	uint32_t word = (keys << 6) ^ (sequence * 0x9E37) ^ (RESULT->playerId << 14);
	return word & 0xFFFF;
}

static int isParent(void) {
	return !(REG_SIOCNT & SIO_SI);
}

static void startTimer(void) {
	REG_TM3CNT_H = 0;
	REG_TM3CNT_L = TIMER_RELOAD;
	REG_TM3CNT_H = TIMER_ENABLE | TIMER_IRQ | TIMER_64CLK;
}

static void onVBlank(void) {
	++RESULT->frames;
	BG_PALETTE[0] = RESULT->hash & 0x7FFF;
	if (isParent()) {
		if (!(REG_SIOCNT & SIO_SD)) {
			return;
		}
		if (burstLeft) {
			++RESULT->lagParent;
		}
		burstLeft = BURST_LENGTH;
		++RESULT->bursts;
		startTimer();
	} else if (linked && ++vblanksSinceSerial > 10) {
		++RESULT->lagChild;
		vblanksSinceSerial = 0;
	}
}

static void onTimer3(void) {
	REG_TM3CNT_H = 0;
	if (!(REG_SIOCNT & SIO_START)) {
		REG_SIOCNT |= SIO_START;
	}
}

static void onSerial(void) {
	uint16_t siocnt = REG_SIOCNT;
	if (siocnt & SIO_ERROR) {
		++RESULT->errors;
	}
	RESULT->playerId = (siocnt >> SIO_ID_SHIFT) & 3;
	uint32_t hash = RESULT->hash;
	int i;
	for (i = 0; i < 4; ++i) {
		uint16_t word = REG_SIOMULTI(i);
		RESULT->lastMulti[i] = word;
		hash = (hash ^ word) * 0x01000193;
	}
	RESULT->hash = hash;
	++RESULT->transfers;
	++sequence;
	linked = 1;
	vblanksSinceSerial = 0;
	REG_SIOMLT_SEND = nextWord();
	if (isParent() && burstLeft && --burstLeft) {
		startTimer();
	}
}

__attribute__((target("arm"), noinline)) void irq_handler(void) {
	uint16_t flags = REG_IE & REG_IF;
	if (flags & IRQ_SERIAL) {
		onSerial();
	}
	if (flags & IRQ_TIMER3) {
		onTimer3();
	}
	if (flags & IRQ_VBLANK) {
		onVBlank();
	}
	REG_IF = flags;
	BIOS_IF |= flags;
}

int main(void) {
	int i;
	for (i = 0; i < (int) (sizeof(struct LinkTestResult) / 4); ++i) {
		((volatile uint32_t*) RESULT)[i] = 0;
	}
	RESULT->magic = RESULT_MAGIC;
	RESULT->hash = 0x811C9DC5;

	REG_DISPCNT = 0;
	REG_RCNT = 0;
	REG_SIOCNT = SIO_MODE_MULTI;
	REG_SIOCNT = SIO_MODE_MULTI | SIO_IRQ | SIO_115200;
	REG_SIOMLT_SEND = nextWord();

	REG_DISPSTAT = 0x0008;
	REG_IE = IRQ_VBLANK | IRQ_TIMER3 | IRQ_SERIAL;
	REG_IME = 1;

	while (1) {
		__asm__ volatile("swi 0x02" ::: "r0", "r1", "r2", "r3", "memory");
	}
}
