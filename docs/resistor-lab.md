# Resistor Lab

Open **Resistor Lab** on the ninth launcher row to identify a color-banded resistor or a printed SMD marking. It works offline without an attached device or SD card. Each format keeps its own draft through Home/re-entry; restarting restores the examples. Nothing is saved to storage.

## Color bands

Choose **4 color bands** or **5 color bands**, then select each numbered band's color. The drawing and result update immediately. Color names and digit/multiplier/tolerance values appear in every selector, so the display color is not the only way to identify a band. Read toward the separated tolerance band.

Four-band decoding uses two significant digits; five-band decoding uses three. The following band supplies the multiplier and the final band supplies tolerance. The result shows nominal resistance, the tolerance percentage, and the corresponding lower/upper resistance limits. **Example** restores yellow/violet/red/gold for four bands (4.7 kohm, +/-5%) or brown/black/black/red/brown for five bands (10 kohm, +/-1%).

The supported mapping follows [Vishay's color-code reference](https://www.vishay.com/docs/20143/colorcod.pdf): digits black through white, multipliers from silver (x0.01) through white (x1 billion), and brown/red/green/blue/violet/gray/gold/silver tolerances of 1/2/0.5/0.25/0.1/0.05/5/10 percent. The first significant digit cannot be black. Three- or six-band parts, extra reliability/TCR markings, pink bands, and other manufacturer conventions are outside this decoder; use the part's datasheet.

## SMD markings

Choose **SMD marking** and tap the input to open the marking keyboard. Enter up to four uppercase ASCII characters; the result updates as you type. The checkmark or keyboard Cancel dismisses the keyboard. **Example** restores `103`.

| Marking form | Example | Nominal result |
| --- | --- | --- |
| Three digits: two significant digits and a decimal exponent | `103` | 10 kohm |
| Four digits: three significant digits and a decimal exponent | `4422` | 44.2 kohm |
| R as the decimal point in ohms | `4R7`, `8R25`, `R005` | 4.7, 8.25, 0.005 ohm |
| EIA-96 index plus multiplier letter | `10C` | 12.4 kohm |
| All-zero or zero R-decimal marking | `0`, `000`, `0R0` | Zero-ohm jumper |

EIA-96 indices run from `01` through `96`. Supported letters are `A`-`H` (x1 through x10 million), `X` (x0.1), `Y` (x0.01), and `Z` (x0.001), as documented in the [Bourns CR-series marking table, page 6](https://www.bourns.com/docs/product-datasheets/CRxxxxx.pdf). Other letters, spaces, signs, decimal points, lowercase characters, and unit suffixes are rejected. Pasted line breaks remain in the field and are rejected, so two lines cannot silently become one marking. An invalid draft replaces the old result; an overlong draft remains invalid after Home/re-entry.

The app displays the numeric interpretation and its calculation. SMD codes do not establish tolerance here, so no tolerance range is invented from the number of digits or the EIA-96 form. A zero marking identifies a jumper, whose actual resistance and current rating depend on its specifications.

Markings alone do not establish power/voltage ratings or prove a part is healthy. Ambiguous or damaged markings need a meter and the relevant datasheet. Display colors are illustrative, not a color measurement.

## Verification

The pure host test checks known values, invalid band roles and markings, tolerance bounds, sub-ohm/large values, and output clearing on errors. It also compares all 96 EIA-96 entries across 11 multipliers with an independently generated, rounded geometric series (1,056 cases). The same test runs in CI, and small known-value checks run at firmware startup.

`./tools/test_offline_ui.ps1 -Snapshots` exercises the real LVGL interface, including band-role changes, independent drafts, popup cleanup, invalid/Unicode input retention, the marking keyboard, Example, visible bounds, and 100 open/close cycles. It writes PPM previews under `build/offline-ui/`. All four offline apps pass the shared lifecycle check with no lost heap bytes or additional live allocations. Physical touch, display colors, both panel families, and device heap behavior remain in the [hardware smoke checklist](hardware-smoke-checklist.md).
