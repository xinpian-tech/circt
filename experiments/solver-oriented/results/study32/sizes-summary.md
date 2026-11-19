### cv32 (97 checks)

Sums and medians over the checks (ANDs / latches after `&scl`):

| model | sum ANDs | median ANDs | sum latches | median latches |
|---|---|---|---|---|
| A0 base | 4863023 | 49366 | 248767 | 2554 |
| A1 base+&fraig | 4135048 | 41838 | 245445 | 2519 |
| A2 known bits+&fraig | 4132681 | 41810 | 245445 | 2519 |
| A3 sc | 4067286 | 41184 | 244390 | 2509 |
| sc+&fraig | 4067286 | 41184 | 244390 | 2509 |
| W ws3 | 4399040 | 44585 | 245415 | 2519 |
| WF ws3+&fraig | 4099575 | 41476 | 245285 | 2518 |

Split of sc's reduction A0-A3 (share of the summed reduction; median of the per-check shares over checks with A0>A3):

| part | ANDs sum | share of sum | median share | latches sum | share of sum | median share |
|---|---|---|---|---|---|---|
| sc total A0-A3 | 795737 | 100.0% | 100.0% | 4377 | 100.0% | 100.0% |
| combinational A0-A1 | 727975 | 91.5% | 92.0% | 3322 | 75.9% | 77.3% |
| constant register bits A1-A2 | 2367 | 0.3% | 0.3% | 0 | 0.0% | 0.0% |
| needs induction A2-A3 | 65395 | 8.2% | 7.7% | 1055 | 24.1% | 22.7% |
| WL without SAT A0-W | 463983 | 58.3% | 58.4% | 3352 | 76.6% | 77.3% |
| WL then &fraig A0-WF | 763448 | 95.9% | 96.4% | 3482 | 79.6% | 79.5% |

- ANDs: induction remainder A2-A3 negative on 0 of 97 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.
- latches: induction remainder A2-A3 negative on 0 of 97 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.

### pico (72 checks)

Sums and medians over the checks (ANDs / latches after `&scl`):

| model | sum ANDs | median ANDs | sum latches | median latches |
|---|---|---|---|---|
| A0 base | 2463848 | 34271 | 155191 | 2151 |
| A1 base+&fraig | 2065225 | 28758 | 154831 | 2146 |
| A2 known bits+&fraig | 2065225 | 28758 | 154831 | 2146 |
| A3 sc | 1895337 | 26400.5 | 154758 | 2145 |
| sc+&fraig | 1895337 | 26400.5 | 154758 | 2145 |
| W ws3 | 2246610 | 31253.5 | 155191 | 2151 |
| WF ws3+&fraig | 2063353 | 28729 | 154831 | 2146 |

Split of sc's reduction A0-A3 (share of the summed reduction; median of the per-check shares over checks with A0>A3):

| part | ANDs sum | share of sum | median share | latches sum | share of sum | median share |
|---|---|---|---|---|---|---|
| sc total A0-A3 | 568511 | 100.0% | 100.0% | 433 | 100.0% | 100.0% |
| combinational A0-A1 | 398623 | 70.1% | 70.0% | 360 | 83.1% | 83.3% |
| constant register bits A1-A2 | 0 | 0.0% | 0.0% | 0 | 0.0% | 0.0% |
| needs induction A2-A3 | 169888 | 29.9% | 30.0% | 73 | 16.9% | 16.7% |
| WL without SAT A0-W | 217238 | 38.2% | 38.3% | 0 | 0.0% | 0.0% |
| WL then &fraig A0-WF | 400495 | 70.4% | 70.4% | 360 | 83.1% | 83.3% |

- ANDs: induction remainder A2-A3 negative on 0 of 72 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.
- latches: induction remainder A2-A3 negative on 0 of 72 checks; sc reduction <= 0 on 0; WL reduces more than sc on 0.

