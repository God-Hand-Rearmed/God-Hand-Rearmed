# tools/perf: where the game thread's time goes (rung 1 of claude/native-next.md)

A dev build with the PC sampler on (`KAMITE_PC_SAMPLE=<seconds>`; `KAMITE_PC_STACKS=0` leaves the chains out) writes
the game thread's guest call chains beside each replayed case: `<case>.<30|60>.stacks.txt` (folded stacks, rewritten
every second; outside replays `logs\stacks_<pid>.txt`). Then:

    python3 tools/perf/subsystems.py <cases folder> [--top 30] [--flame flame.svg] [--rate 30|60]

- `subsystems.csv`: the rules that put a guest function in a subsystem (address, range, idantony module, name).
- `idantony_modules.csv`: EE address -> the idantony/godhand-windows (MIT) file that reimplements it, written by
  `import_idantony.py <checkout>` (facts only: which system an address belongs to).
