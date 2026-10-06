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

/* The firmware's defaults: provisioning 3 presses, identify 2. */
static const struct gesture_cfg cfg = {
	.press_count = 3,
	.identify_count = 2,
	.window_ms = 2000,
	.short_max_ms = 1000,
	.quiet_ms = 700,
	.erase_ms = 10000,
};

/* An image without the provisioning hand-off: identify only. */
static const struct gesture_cfg identify_only = {
	.identify_count = 2,
	.window_ms = 2000,
	.short_max_ms = 1000,
	.quiet_ms = 700,
};

/* An image without identify: the original classifier. */
static const struct gesture_cfg provision_only = {
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
	int identify;
};

static struct result run(const struct gesture_cfg *c, const struct press *p,
			 int n)
{
	struct gesture_state g;
	struct result r = { 0 };
	int i = 0;
	int64_t end = p[n - 1].up + 5000;

	gesture_init(&g, c);
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
		r.identify += e == GESTURE_IDENTIFY;
	}
	return r;
}

static int failures;

static void expect(const char *name, const struct gesture_cfg *c,
		   const struct press *p, int n, int provision, int erase,
		   int identify)
{
	struct result r = run(c, p, n);
	bool ok = r.provision == provision && r.erase == erase &&
		  r.armed == erase && r.identify == identify;

	printf("%-44s %s (provision=%d erase=%d armed=%d identify=%d)\n",
	       name, ok ? "ok" : "FAIL", r.provision, r.erase, r.armed,
	       r.identify);
	failures += !ok;
}

#define N(arr) ((int)(sizeof(arr) / sizeof(arr[0])))
#define EXPECT(name, arr, prov, erase, ident) \
	expect(name, &cfg, arr, N(arr), prov, erase, ident)
#define EXPECT_CFG(name, c, arr, prov, erase, ident) \
	expect(name, c, arr, N(arr), prov, erase, ident)

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

	static const struct press two_slow[] = { { 100, 250 }, { 2000, 2150 } };
	static const struct press two_then_two[] = {
		{ 100, 250 }, { 500, 650 }, { 3000, 3150 }, { 3400, 3550 },
	};

	EXPECT("1 press", one, 0, 0, 0);
	EXPECT("2 presses (identify)", two, 0, 0, 1);
	EXPECT("2 presses, too slow", two_slow, 0, 0, 0);
	EXPECT("identify twice, apart", two_then_two, 0, 0, 2);
	EXPECT("3 presses (provisioning, never identify)", three, 1, 0, 0);
	EXPECT("4 presses", four, 0, 0, 0);
	/* The second gap (750 ms) ends the sequence after two presses, so a
	 * slow triple is two quick presses and a lone one: identify, which
	 * changes nothing on the device. */
	EXPECT("3 presses, too slow", slow, 0, 0, 1);
	EXPECT_CFG("identify off: 3 presses, too slow", &provision_only, slow,
		   0, 0, 0);
	EXPECT("3 presses, last one held 1.6 s", long_third, 0, 0, 0);
	EXPECT("3 s hold", short_hold, 0, 0, 0);
	EXPECT("10.4 s hold (erase)", erase, 0, 1, 0);
	EXPECT("pattern twice, apart", three_then_three, 2, 0, 0);

	EXPECT_CFG("identify off: 2 presses", &provision_only, two, 0, 0, 0);
	EXPECT_CFG("identify off: 3 presses", &provision_only, three, 1, 0, 0);
	EXPECT_CFG("provisioning off: 2 presses", &identify_only, two, 0, 0, 1);
	EXPECT_CFG("provisioning off: 3 presses", &identify_only, three, 0, 0,
		   0);
	EXPECT_CFG("provisioning off: 10.4 s hold", &identify_only, erase, 0,
		   0, 0);

	printf("%s\n", failures ? "FAILED" : "all passed");
	return failures ? 1 : 0;
}
