/* SPDX-License-Identifier: MPL-2.0 */
/* Fast mode: the resampling mixer loop runs here in C, sample by sample.
 *
 *   head:      LDR r6,[r5]; LDR r7,[r5,#0x630]
 *   head + 8:  mixer_packet_ops (one sample per pass, four per word)
 *
 * Every edge into the head leaves generated code, so C sees each channel and
 * keeps it until the channel ends, an event is due (fast mode takes it after
 * the sample that reaches it), the wave ends or a sample leaves the proven
 * domain. The guest state is then exactly the loop's at that sample
 * boundary: the head, or head + 8 with k samples of the word done, the r5
 * rotation k and r6/r7 = the partly mixed words rotated right by 8k.
 * Each sample's time and retired count are the interpreter's (packet guard). */
static const uint32_t mixer_head_ops[2] = {0xe5956000u, 0xe5957630u};
#define MIXER_HEAD_BYTES (8 + sizeof(mixer_packet_ops))

static bool mixer_head_at(const struct Gbn* m, uint32_t pc) {
	if (pc >> 24 != 3 || (pc & 3) || !m->iwram) return false;
	uint32_t offset = pc & (GBN_IWRAM_SIZE - 1);
	if (offset > GBN_IWRAM_SIZE - MIXER_HEAD_BYTES) return false;
	const uint8_t* code = m->iwram + offset;
	if (memcmp(code, mixer_head_ops, 8)) return false;
#if defined(AM_RVV) && (GBN_PPU_RVV & 8)
	return gbn_rvv_equal(code + 8, (const uint8_t*) mixer_packet_ops, sizeof(mixer_packet_ops));
#else
	return !memcmp(code + 8, mixer_packet_ops, sizeof(mixer_packet_ops));
#endif
}

/* Bytewise add without carries between lanes (the guest's top-byte adds). */
static inline uint32_t mixer_bytes(uint32_t a, uint32_t b) {
	return ((a & 0x7f7f7f7fu) + (b & 0x7f7f7f7fu)) ^ ((a ^ b) & 0x80808080u);
}

static __attribute__((noinline)) uint32_t mixer_hle(struct Gbn* m) {
	uint32_t pc = m->cpu.pc, head;
	if (m->cpu.pipe[0] == mixer_head_ops[0]) head = pc;
	else if (m->cpu.pipe[0] == mixer_packet_ops[0]) head = pc - 8;
	else return 0;
	if (m->code_wait || m->code_word_wait || !m->rom || !mixer_head_at(m, head)) return 0;
	if (m->cpu.pipe[1] != (pc == head ? mixer_head_ops[1] : mixer_packet_ops[1])) return 0;
	uint32_t* r = m->cpu.r;
	uint32_t r5 = r[5], r8 = r[8], lr = r[14], step = r[4], src = r[3];
	int32_t s = (int32_t) r[0], d = (int32_t) r[1], rem = (int32_t) r[2];
	/* Byte volumes; byte-range samples, so every MUL multiplier is a byte and
	 * early-terminates after one cycle; a sub-sample position and a step of
	 * at most 8 samples keep LR below 2^23; IWRAM output with the rotation in
	 * r5[31:30]; a ROM source. */
	if ((r[10] | r[11]) & 0xff00ffffu) return 0;
	if (s < -128 || s > 127 || s + d < -128 || s + d > 127) return 0;
	if (lr >> 23 || step > 0x4000000u || (r5 & 3) || (r5 >> 24 & 63) != 3) return 0;
	unsigned k = r5 >> 30, region = src >> 24;
	if ((pc == head && k) || region < 8 || region > 13) return 0;
	/* The two stores of every word must miss the loop's own code. */
	uint32_t base = r5 & 0x3fffffffu, code = head & (GBN_IWRAM_SIZE - 1);
	uint32_t words = (int32_t) r8 > 0 ? ((r8 + 3) >> 2) + 1 : 1;
	for (unsigned side = 0; side < 2; ++side) {
		uint32_t from = (base + side * 0x630) & (GBN_IWRAM_SIZE - 1), to = from + 4 * words;
		if ((from < code + MIXER_HEAD_BYTES && code < to) ||
		    (to > GBN_IWRAM_SIZE && code < to - GBN_IWRAM_SIZE)) return 0;
	}
	uint32_t deadline = m->now + GBN_MAX_DELAY;
	int next = gbn_cpu_event(m);
	if (next >= 0) deadline = m->events[next].when;
	/* Source offsets stay below `limit`: in the image, one 16 MiB bus page. */
	uint32_t q = src & 0x01ffffffu, q0 = q, limit = (q & ~0xffffffu) + 0x1000000u;
	if (limit > m->rom_size) limit = m->rom_size;
	if (q + 1 >= limit) return 0;
	const uint8_t* rom = m->rom;
	uint8_t* iwram = m->iwram;
	uint32_t load_cost = m->rom_nonseq[(region - 8) >> 1] + 2u;
	int32_t vr = (int32_t) (r[10] >> 16), vl = (int32_t) (r[11] >> 16);
	uint32_t now = m->now, retired = 0, word_r = 0, word_l = 0, add_r = 0, add_l = 0;
	uint32_t last_n = 0, last_v = 0, old8 = r8;
	bool at_head = pc == head, ended = false;
	if (!at_head) {
		/* Mid-word: undo the rotation of the partly mixed words. */
		unsigned sh = 8 * k;
		word_r = sh ? r[6] << sh | r[6] >> (32 - sh) : r[6];
		word_l = sh ? r[7] << sh | r[7] >> (32 - sh) : r[7];
	}
	/* r0/r1 are the sample bytes around r3 - 1 (loop invariant, also kept
	 * below); then every sample of a word follows from its position alone. */
	bool positional = q >= 1 && (int8_t) rom[q - 1] == s && (int8_t) rom[q] == s + d;
	while ((int32_t) (now - deadline) < 0) {
		if (at_head && positional) {
			/* Whole word with independent samples (no position chain). */
			uint32_t x1 = lr + step, x2 = x1 + step, x3 = x2 + step, x4 = x3 + step;
			uint32_t i1 = x1 >> 23, i2 = x2 >> 23, i3 = x3 >> 23, i4 = x4 >> 23;
			if (rem <= (int32_t) i4 || q + i4 >= limit) goto single;
			const int8_t* b = (const int8_t*) rom + q - 1;
			int32_t v0 = b[0] + ((int32_t) lr * (b[1] - b[0]) >> 23);
			int32_t v1 = b[i1] + ((int32_t) (x1 & 0x7fffff) * (b[i1 + 1] - b[i1]) >> 23);
			int32_t v2 = b[i2] + ((int32_t) (x2 & 0x7fffff) * (b[i2 + 1] - b[i2]) >> 23);
			int32_t v3 = b[i3] + ((int32_t) (x3 & 0x7fffff) * (b[i3 + 1] - b[i3]) >> 23);
			uint32_t n0 = i1, n1 = i2 - i1, n2 = i3 - i2, n3 = i4 - i3;
			uint32_t moved = (n0 != 0) + (n1 != 0) + (n2 != 0) + (n3 != 0);
			uint32_t extra = (n0 > 1) + (n1 > 1) + (n2 > 1) + (n3 > 1);
			bool more = (int32_t) (r8 - 4) > 0;
			uint32_t cost = 90 + (more ? 2 : 0) + 6 * moved + load_cost * (moved + extra);
			uint32_t last = 20 + 4 + (more ? 2 : 0) + (n3 ? 6 + load_cost * (n3 > 1 ? 2 : 1) : 0);
			/* Each sample starts before the event, as in the single steps. */
			if ((int32_t) (now + cost - last - deadline) >= 0) goto single;
			uint32_t add_r4 = (uint32_t) (uint8_t) (vr * v0 >> 8) | (uint32_t) (uint8_t) (vr * v1 >> 8) << 8
				| (uint32_t) (uint8_t) (vr * v2 >> 8) << 16 | (uint32_t) (uint8_t) (vr * v3 >> 8) << 24;
			uint32_t add_l4 = (uint32_t) (uint8_t) (vl * v0 >> 8) | (uint32_t) (uint8_t) (vl * v1 >> 8) << 8
				| (uint32_t) (uint8_t) (vl * v2 >> 8) << 16 | (uint32_t) (uint8_t) (vl * v3 >> 8) << 24;
			uint32_t o = base & (GBN_IWRAM_SIZE - 1);
			uint32_t* wr = (uint32_t*) __builtin_assume_aligned(iwram + o, 4);
			uint32_t* wl = (uint32_t*) __builtin_assume_aligned(iwram + ((o + 0x630) & (GBN_IWRAM_SIZE - 1)), 4);
			word_r = mixer_bytes(*wr, add_r4); word_l = mixer_bytes(*wl, add_l4);
			*wl = word_l; *wr = word_r;
			s = b[i4]; d = b[i4 + 1] - b[i4]; q += i4; rem -= (int32_t) i4; lr = x4 & 0x7fffff;
			last_n = n3; last_v = (uint32_t) v3;
			base += 4; old8 = r8; r8 -= 4;
			now += cost; retired += 58 + 8 * moved;
			if (!more) { ended = true; break; }
			continue;
		}
	single:;
		uint32_t cost = 20, count = 13, f = lr + step, n = f >> 23;
		/* SUBS r2 / BLE leaves for the end of the wave; stop before it. */
		if (n && (rem <= (int32_t) n || q + n >= limit)) break;
		if (at_head) {
			uint32_t o = base & (GBN_IWRAM_SIZE - 1);
			word_r = *(const uint32_t*) __builtin_assume_aligned(iwram + o, 4);
			word_l = *(const uint32_t*) __builtin_assume_aligned(iwram + ((o + 0x630) & (GBN_IWRAM_SIZE - 1)), 4);
			cost += 6; count += 2;
		}
		int32_t v = s + ((int32_t) lr * d >> 23);
		add_r |= (uint32_t) (uint8_t) (vr * v >> 8) << 8 * k;
		add_l |= (uint32_t) (uint8_t) (vl * v >> 8) << 8 * k;
		last_v = (uint32_t) v; last_n = n; at_head = false;
		if (n) {
			f &= 0x7fffffu; rem -= (int32_t) n;
			if (n == 1) s += d;
			else { q += n - 1; s = (int8_t) rom[q]; cost += load_cost; }
			++q; d = (int8_t) rom[q] - s;
			cost += 6 + load_cost; count += 8;
		}
		lr = f;
		if (++k == 4) {
			/* Word end: the BCC falls through, two stores, SUBS r8 and BGT. */
			uint32_t o = base & (GBN_IWRAM_SIZE - 1);
			word_r = mixer_bytes(word_r, add_r); word_l = mixer_bytes(word_l, add_l);
			*(uint32_t*) __builtin_assume_aligned(iwram + ((o + 0x630) & (GBN_IWRAM_SIZE - 1)), 4) = word_l;
			*(uint32_t*) __builtin_assume_aligned(iwram + o, 4) = word_r;
			add_r = add_l = 0; k = 0;
			base += 4; old8 = r8; r8 -= 4;
			bool more = (int32_t) r8 > 0;
			cost += 4 + (more ? 2 : 0); count += 4;
			at_head = true;
			if (!more) ended = true;
		}
		now += cost; retired += count;
		if (ended) break;
	}
	if (!retired) return 0;
	uint32_t flags = m->cpu.cpsr & 0x0fffffffu;
	if (k) {
		/* After ADDS r5,r5,#0x40000000: N for rotations 2 and 3, V for 2. */
		unsigned sh = 8 * k;
		uint32_t a = mixer_bytes(word_r, add_r), b = mixer_bytes(word_l, add_l);
		r[6] = a >> sh | a << (32 - sh); r[7] = b >> sh | b << (32 - sh);
		r[5] = base | k << 30;
		flags |= (k >= 2 ? 0x80000000u : 0) | (k == 2 ? 0x10000000u : 0);
		m->cpu.shifter_carry = 0;
		m->cpu.pc = head + 8;
	} else {
		r[6] = word_r; r[7] = word_l; r[5] = base;
		flags |= (r8 & 0x80000000u) | (r8 ? 0 : 0x40000000u) | (old8 >= 4 ? 0x20000000u : 0)
			| (((old8 ^ 4) & (old8 ^ r8)) >> 31) << 28;
		m->cpu.shifter_carry = 1;
		m->cpu.pc = ended ? head + MIXER_HEAD_BYTES : head;
	}
	r[0] = (uint32_t) s; r[1] = (uint32_t) d; r[2] = (uint32_t) rem; r[3] = src + (q - q0);
	r[8] = r8; r[9] = last_n ? last_n - 1 : 0; r[12] = (r[11] * last_v) & ~0xff0000u; r[14] = lr;
	m->cpu.cpsr = flags;
	m->prefetched_pc = 0;
	m->now = now;
	/* The pipeline of a loop boundary is the proven loop code itself. */
	if (m->cpu.pc == head) { m->cpu.pipe[0] = mixer_head_ops[0]; m->cpu.pipe[1] = mixer_head_ops[1]; }
	else if (m->cpu.pc == head + 8) { m->cpu.pipe[0] = mixer_packet_ops[0]; m->cpu.pipe[1] = mixer_packet_ops[1]; }
	else { m->cpu.pipe[0] = fetch(m, m->cpu.pc, 4); m->cpu.pipe[1] = fetch(m, m->cpu.pc + 4, 4); }
	return retired;
}
#undef MIXER_HEAD_BYTES
