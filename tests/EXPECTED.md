# Expected events for the directed tests

These counts come from the C source. The addresses come from `objdump -d` of
binaries built by `tests/Makefile` with gcc 13.3 on Ubuntu 24.04. With a
different compiler version the addresses can shift, but the counts should not
change. Always check addresses against your own
`results/validation/<test>.objdump.txt`.

To compare, run `python3 scripts/summarize_log.py results/validation/<test>.csv <routine>`.

Things that appear in every test and can be ignored for the per-test checks:

- Startup and shutdown code in the main executable (`_start`, `_init`,
  `frame_dummy`, `register_tm_clones`, `__do_global_dtors_aux`, `_fini`).
- The path through the PLT when `main` calls `printf`. The `call printf@plt`
  is a direct call, and the stub then does an indirect `jmp *` through the
  GOT. With lazy binding, the first call also runs a `jmp` and a `jmp *` in
  `.plt` that lead into the dynamic loader.

Pin's disassembler sometimes uses a different name for the same instruction
than objdump does (for example `jnz` for `jne`, or `jnle` for `jg`); the
address and size are what to match.

A general check that works for every row: `size` equals the next address
minus `pc` in objdump, and `fallthrough_pc` equals `pc + size`.

## t_cond

`cond_test`: a 10-iteration loop with `if (i % 3 == 0)` inside.

| pc | instruction | type | executions | taken | notes |
|---|---|---|---|---|---|
| 0x401145 | `jmp 401183` | jmp_direct | 1 | 1 | jumps to the loop test |
| 0x40116e | `jne 40117f` | cond | 10 | 6 | taken for i = 1, 2, 4, 5, 7, 8 |
| 0x401187 | `jle 401147` | cond | 11 | 10 | loop back edge; falls through once |
| 0x40118c | `ret` | ret | 1 | 1 | next_pc 0x40119a (in main) |

`main`: `call cond_test` at 0x401195 runs once, target 0x401136,
fallthrough 0x40119a.

The `inst_count` column can also be checked here, because the loop body has
a fixed length:

- From the `jmp` event to the first `jle` event: +2.
- From a `jle` event to the next `jne` event: +15.
- From a `jne` event to the next `jle` event: +6 if the `jne` was not taken
  (the `sink++` ran), +3 if it was taken.

## t_calls

`top(3)` nests main → top → mid → leaf, twice. `fact(5)` recurses five deep.

| routine | pc | instruction | type | executions | taken | next_pc values |
|---|---|---|---|---|---|---|
| leaf | 0x401148 | `ret` | ret | 2 | 2 | 0x401162 both times |
| mid | 0x40115d | `call leaf` | call_direct | 2 | 2 | 0x401136 |
| mid | 0x401165 | `ret` | ret | 2 | 2 | 0x401180, then 0x40118f |
| top | 0x40117b | `call mid` | call_direct | 1 | 1 | 0x401149 |
| top | 0x40118a | `call mid` | call_direct | 1 | 1 | 0x401149 |
| top | 0x401196 | `ret` | ret | 1 | 1 | 0x4011dc |
| fact | 0x4011aa | `jg 4011b3` | cond | 5 | 4 | taken for n = 5, 4, 3, 2 |
| fact | 0x4011b1 | `jmp 4011c4` | jmp_direct | 1 | 1 | only for n = 1 |
| fact | 0x4011bb | `call fact` | call_direct | 4 | 4 | 0x401197 |
| fact | 0x4011c5 | `ret` | ret | 5 | 5 | 0x4011c0 four times, then 0x4011e9 |

`main`: `call top` (0x4011d7), `call fact` (0x4011e4), `call printf@plt`
(0x401203) and `ret` once each.

The order of events matters for the return-address stack later, so also
check the sequence for `top(3)`: call top, call mid, call leaf, ret → 0x401162,
ret → 0x401180, call mid, call leaf, ret → 0x401162, ret → 0x40118f,
ret → 0x4011dc.

## t_indirect

`dispatch(6)` calls through a function-pointer table, so one indirect call
rotates among three targets. `sw(k)` for k = 0..5 is a switch compiled to a
jump table, so one indirect jump goes to a different target each time.

| routine | pc | instruction | type | executions | taken | next_pc values |
|---|---|---|---|---|---|---|
| dispatch | 0x40118a | `jmp 4011d5` | jmp_direct | 1 | 1 | 0x4011d5 |
| dispatch | 0x4011cc | `call *%rdx` | call_indirect | 6 | 6 | 0x401136, 0x401149, 0x40115b, repeating |
| dispatch | 0x4011db | `jl 40118c` | cond | 7 | 6 | loop back edge |
| dispatch | 0x4011e1 | `ret` | ret | 1 | 1 | 0x40125e |
| add1, dbl, neg | 0x401148, 0x40115a, 0x40116c | `ret` | ret | 2 each | 2 each | 0x4011ce |
| sw | 0x4011f1 | `ja 401241` | cond | 6 | 0 | default case never runs |
| sw | 0x401214 | `notrack jmp *%rax` | jmp_indirect | 6 | 6 | 0x401217, 0x40121e, 0x401225, 0x40122c, 0x401233, 0x40123a |
| sw | 0x40121c, 0x401223, 0x40122a, 0x401231, 0x401238, 0x40123f | `jmp 401246` | jmp_direct | 1 each | 1 each | 0x401246 |
| sw | 0x401247 | `ret` | ret | 6 | 6 | 0x40127b |

`main`: `call dispatch` once, `jmp 401282` (0x40126f) once, `jle 401271`
(0x401286) 7 times with 6 taken, `call sw` (0x401276) 6 times,
`call printf@plt` and `ret` once each.

## Baseline predictor (spec Section 4.1)

The `wrong` column from `summarize_log.py` counts executions whose predicted
next PC was wrong. For jumps, calls and returns it can be worked out by hand
from the rules in `report/design_notes.md`:

- An instruction's first execution misses in the BTB, so it is wrong (a
  "cold" miss). Its later executions hit.
- Direct jumps and calls are then always right.
- Returns are right after their first execution, because the calls in these
  tests nest only a few levels deep, well within the 16-entry RAS.
- An indirect jump or call is right only when its target is the same as last
  time.

Conditional branches are not listed. Their predictions depend on the global
history, which also holds the outcomes of thousands of library branches, so
they can't be worked out by hand. They should still be wrong far less often
than they execute.

A small chance exists that a library branch shares the same low 24 address
bits as a test instruction (the BTB's index plus partial tag). That would show
up as an extra wrong or right prediction on one row; the run's
`btb_alias_hits` count would then be nonzero.

### t_calls

| routine | pc | instruction | executions | wrong | why |
|---|---|---|---|---|---|
| leaf | 0x401148 | `ret` | 2 | 1 | cold, then RAS |
| mid | 0x40115d | `call leaf` | 2 | 1 | cold, then BTB |
| mid | 0x401165 | `ret` | 2 | 1 | cold, then RAS gives 0x40118f |
| top | 0x40117b | `call mid` | 1 | 1 | cold |
| top | 0x40118a | `call mid` | 1 | 1 | cold |
| top | 0x401196 | `ret` | 1 | 1 | cold |
| fact | 0x4011b1 | `jmp 4011c4` | 1 | 1 | cold |
| fact | 0x4011bb | `call fact` | 4 | 1 | cold, then BTB |
| fact | 0x4011c5 | `ret` | 5 | 1 | cold, then RAS (5 deep) |
| main | 0x4011d7, 0x4011e4, 0x401203 | `call` | 1 each | 1 each | cold |
| main | 0x40120e | `ret` | 1 | 1 | cold |

### t_indirect

| routine | pc | instruction | executions | wrong | why |
|---|---|---|---|---|---|
| add1, dbl, neg | 0x401148, 0x40115a, 0x40116c | `ret` | 2 each | 1 each | cold, then RAS |
| dispatch | 0x40118a | `jmp 4011d5` | 1 | 1 | cold |
| dispatch | 0x4011cc | `call *%rdx` | 6 | 6 | cold, then the last target is never the next one |
| dispatch | 0x4011e1 | `ret` | 1 | 1 | cold |
| sw | 0x401214 | `notrack jmp *%rax` | 6 | 5 | first: BTB miss, so the next instruction (0x401217) is predicted, and that happens to be case 0's target; then a different target every time |
| sw | six `jmp 401246` | `jmp` | 1 each | 1 each | cold |
| sw | 0x401247 | `ret` | 6 | 1 | cold, then RAS |
| main | 0x401259, 0x40129f | `call` | 1 each | 1 each | cold |
| main | 0x40126f | `jmp 401282` | 1 | 1 | cold |
| main | 0x401276 | `call sw` | 6 | 1 | cold, then BTB |
| main | 0x4012aa | `ret` | 1 | 1 | cold |

The indirect call and the switch jump are the interesting rows: the baseline
BTB remembers only the most recent target, so a call that cycles among three
targets and a jump that takes a new target each time are always wrong once
the BTB has an entry for them. This is
the behavior a Section 5 target-prediction improvement can fix.
