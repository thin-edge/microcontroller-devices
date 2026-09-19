/* SPDX-License-Identifier: Apache-2.0
 *
 * Host test for lib/common/button_gesture.c: replays recorded press timings
 * through the classifier the way the firmware drives it (edges plus a 50 ms
 * tick) and checks which gesture comes out.
 *
 *   cc -I lib/common -o /tmp/tbg tests/button_gesture/test_button_gesture.c \
 *      lib/common/button_gesture.c && /tmp/tbg
 */
#include "button_gesture.h"

#include <stdio.h>

static const struct gesture_cfg cfg = {
	.press_count = 3,
	.window_ms = 2000,
	.short_max_ms = 1000,
	.quiet_ms = 700,
	.erase_ms = 10000,
};

/* A press sequence: pairs of (press_at, release_at) in ms. */
struct press {
	int64_t down, up;
};

struct result {
	int provision;
	int erase;
	int armed;
};

static struct result run(const struct press *p, int n)
{
	struct gesture_state g;
	struct result r = { 0 };
	int i = 0;
	int64_t end = p[n - 1].up + 5000;

	gesture_init(&g, &cfg);
	for (int64_t t = 0; t <= end; t += 10) {
		enum gesture e = GESTURE_NONE;

		if (i < n && t == p[i].down) {
			e = gesture_press(&g, t);
		} else if (i < n && t == p[i].up) {
			e = gesture_release(&g, t);
			i++;
		} else if (t % 50 == 0) {
			e = gesture_tick(&g, t);
		}
		r.provision += e == GESTURE_PROVISION;
		r.erase += e == GESTURE_ERASE;
		r.armed += e == GESTURE_ERASE_ARMED;
	}
	return r;
}

static int failures;

static void expect(const char *name, const struct press *p, int n,
		   int provision, int erase)
{
	struct result r = run(p, n);
	bool ok = r.provision == provision && r.erase == erase &&
		  r.armed == erase;

	printf("%-40s %s (provision=%d erase=%d armed=%d)\n", name,
	       ok ? "ok" : "FAIL", r.provision, r.erase, r.armed);
	failures += !ok;
}

#define EXPECT(name, arr, prov, erase) \
	expect(name, arr, (int)(sizeof(arr) / sizeof(arr[0])), prov, erase)

int main(void)
{
	static const struct press one[] = { { 100, 250 } };
	static const struct press two[] = { { 100, 250 }, { 500, 650 } };
	static const struct press three[] = { { 100, 250 }, { 500, 650 },
					       { 900, 1050 } };
	static const struct press four[] = { { 100, 250 }, { 500, 650 },
					      { 900, 1050 }, { 1300, 1450 } };
	static const struct press slow[] = { { 100, 250 }, { 900, 1050 },
					      { 1800, 2300 } };
	static const struct press long_third[] = { { 100, 250 }, { 500, 650 },
						    { 900, 2500 } };
	static const struct press short_hold[] = { { 100, 3100 } };
	static const struct press erase[] = { { 100, 10500 } };
	static const struct press three_then_three[] = {
		{ 100, 250 }, { 500, 650 }, { 900, 1050 },
		{ 3000, 3150 }, { 3400, 3550 }, { 3800, 3950 },
	};

	EXPECT("1 press", one, 0, 0);
	EXPECT("2 presses", two, 0, 0);
	EXPECT("3 presses (the pattern)", three, 1, 0);
	EXPECT("4 presses", four, 0, 0);
	EXPECT("3 presses, too slow", slow, 0, 0);
	EXPECT("3 presses, last one held 1.6 s", long_third, 0, 0);
	EXPECT("3 s hold", short_hold, 0, 0);
	EXPECT("10.4 s hold (erase)", erase, 0, 1);
	EXPECT("pattern twice, apart", three_then_three, 2, 0);

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
