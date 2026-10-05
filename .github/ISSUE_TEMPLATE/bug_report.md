---
name: Bug report
about: A service reports the wrong state, misses or duplicates an event, breaks a tray or notification client, or crashes
labels: bug
---

**Service:** power / audio / network / notifications / tray

**What the OS says** (the oracle: `upower -d`, `wpctl status`, `nmcli`,
`pmset -g batt`, `scutil --nwi`, the Windows battery or sound settings, the
tray client you used, ...):

```
```

**What brosys reports instead** (the model or event sequence, a crash, or the
failing `ctest --output-on-failure` output — paste it):

```
```

**How to reproduce it** (the calls, and what you did to the machine: unplug,
switch output, join a network, start a tray app):

**Environment:**
- OS and version; on Linux the desktop, and whether PipeWire / NetworkManager / UPower run:
- Compiler / toolchain (MSVC / GCC / Clang):
- brosys commit:
