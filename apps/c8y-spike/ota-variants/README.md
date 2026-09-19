# Spike B test images

Overlays for the five test images of `c8y-direct-spikes` section 4, applied on
top of `overlay-spike-a.conf`, `overlay-spike-b.conf` and `overlay-github.conf`:

| Image | Version | Auto-confirm | Purpose |
|---|---|---|---|
| A | 0.0.1 | yes | base, flashed over USB; does the downloading |
| B | 0.0.2 | no | download + test boot, confirm from the shell (4.1, 4.2) |
| C | 0.0.3 | no | reset without confirming, so MCUboot reverts (4.3) |
| D | 0.0.4 | no | blocks the system workqueue after 5 s, so the watchdog fires (4.3) |
| E | 0.0.5 | yes | delivered via Cumulocity `c8y_Firmware` (4.5) |

`build_all.sh [A B C D E]` builds them into `build_spike_b_<X>/`.
