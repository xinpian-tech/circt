### cv32 (97 checks)

Merged members: 673040 without initial values (comb: true in every state), 1091881 with (all); 418842 new merges with initial values (sources merged more often with than without), 1 lost (sources merged less often: their gates are covered by other classes, e.g. a gate merged into another gate without initial values and constant with them).

| category | comb | % of comb | new (induction) | % of new | all | % of all |
|---|---|---|---|---|---|---|
| const-gate | 32729 | 4.9% | 259749 | 62.0% | 336624 | 30.8% |
| reset-mux | 229534 | 34.1% | 692 | 0.2% | 207427 | 19.0% |
| mux-dup | 93031 | 13.8% | 55704 | 13.3% | 165808 | 15.2% |
| mux-select | 133768 | 19.9% | 27367 | 6.5% | 132226 | 12.1% |
| decode-compare | 103619 | 15.4% | 11173 | 2.7% | 105377 | 9.7% |
| unlocated | 38797 | 5.8% | 3490 | 0.8% | 42287 | 3.9% |
| const-reg | 0 | 0.0% | 39387 | 9.4% | 39387 | 3.6% |
| arith-same | 34751 | 5.2% | 293 | 0.1% | 34898 | 3.2% |
| arith-cross | 6811 | 1.0% | 19785 | 4.7% | 26645 | 2.4% |
| reg-design | 0 | 0.0% | 1164 | 0.3% | 1164 | 0.1% |
| reg-sva | 0 | 0.0% | 38 | 0.0% | 38 | 0.0% |

### pico (72 checks)

Merged members: 236819 without initial values (comb: true in every state), 444212 with (all); 207393 new merges with initial values (sources merged more often with than without), 0 lost (sources merged less often: their gates are covered by other classes, e.g. a gate merged into another gate without initial values and constant with them).

| category | comb | % of comb | new (induction) | % of new | all | % of all |
|---|---|---|---|---|---|---|
| mux-select | 152670 | 64.5% | 20592 | 9.9% | 173622 | 39.1% |
| first-mux | 0 | 0.0% | 158314 | 76.3% | 158314 | 35.6% |
| mux-dup | 25553 | 10.8% | 2232 | 1.1% | 25769 | 5.8% |
| const-gate | 9518 | 4.0% | 14256 | 6.9% | 25214 | 5.7% |
| decode-compare | 16983 | 7.2% | 4901 | 2.4% | 20300 | 4.6% |
| arith-same | 14942 | 6.3% | 0 | 0.0% | 14942 | 3.4% |
| arith-cross | 5408 | 2.3% | 72 | 0.0% | 5480 | 1.2% |
| regfile-slot | 4608 | 1.9% | 0 | 0.0% | 4608 | 1.0% |
| gate-eq-reg | 144 | 0.1% | 2346 | 1.1% | 4434 | 1.0% |
| reset-mux | 2746 | 1.2% | 1800 | 0.9% | 4402 | 1.0% |
| unlocated | 4247 | 1.8% | 144 | 0.1% | 4391 | 1.0% |
| reg-design | 0 | 0.0% | 2448 | 1.2% | 2448 | 0.6% |
| const-reg | 0 | 0.0% | 288 | 0.1% | 288 | 0.1% |

