# Conformance harness

The conformance tests: the reference models and conformance packages under `tests/oracle/` driving the
S-xx logic of `src/`, independently of the unit tests in `tests/unit/`.

```
tools/conformance.sh                          # everything the conformance job runs
tools/conformance.sh harness                  # this harness only
make -f tests/conformance/Makefile            # build the host programs
python3 -I -B tests/conformance/run.py -v     # run the drivers
python3 -I -B tests/conformance/coverage.py   # write COVERAGE.md (generated, not tracked)
```

`tools/conformance.sh` checks that the generated test data is current (`tools/regen.py --check`), runs
the conformance packages' self-tests, then this harness and its coverage check.

## How it works

- `h_<name>.c` is a host program linking only the `src/` files the `Makefile` lists for it. It reads
  commands on stdin and prints one result line per command (`hlib.h` gives the notation).
- `h_s07p` is the exception: a conversation. It runs S-07's PSoC link on the unit tests' virtual
  clock and fake port, prints each write, and reads the answer `test_s07.py` computes with the PSoC
  reference model, so the timed transcripts are run against a live PSoC rather than replayed.
- `test_<name>.py` builds the inputs with the conformance packages (seeded, so runs repeat), sends them
  to the program and checks each result against the reference model. Each test method is named after the
  scenario or requirement it checks and carries a mark (`@scenario('S-04/R-ROWS')`,
  `@requirement('S-04.070')`).
- `scenarios.tsv` lists the scenario IDs. They are keyed by label number, `S-04/R-ROWS`, since short IDs
  repeat across numbers. `coverage.py` fails when a test's mark names a scenario that is not in the list.
- Most scenarios are emulator runs. A harness test checks the scenario's **logic slice**, the rule the
  scenario observes, applied to the logic alone; the emulator run around it is not covered here.
  `COVERAGE.md` keeps the two apart. It is generated, not committed: run `coverage.py`.
- Where the implementation disagrees with a requirement, the test is marked
  `@expected_failure(id, note)` and `src/` is left alone; `run.py` lists those at the end.

Python 3 standard library only. Run with `-I`: `harness.py` puts `tests/oracle/` on the path and the
drivers import `conformance.serial`, never a bare `serial`.
