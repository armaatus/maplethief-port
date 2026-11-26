# How-tos

Step-by-step guides for the changes most contributions are made of. Each was followed once on a scratch branch to add a trivial example, and its *Worked example* section says what that took. Set up first ([CONTRIBUTING](../../../CONTRIBUTING.md#setting-up)), and run [the quick check](../testing.md#before-you-push) before you push.

| How-to | When |
|---|---|
| [Adding an HLE function](hle-function.md) | The game calls an iOS library function the runtime doesn't have yet (`MT_TRAP_IMPORT`) |
| [Adding a selector on a host class](host-selector.md) | The game sends a framework class a message it doesn't answer yet (`MT_TRAP_OBJC`) |
| [Adding a setting](setting.md) | A new `settings.ini` key, with its flag, presets, menu row and tests |
| [Debugging a trap](trap.md) | A run stopped with exit code 5 and a crash report |

The design behind them: [ARCHITECTURE.md](../../ARCHITECTURE.md), [ADR 0019](../../adr/0019-import-thunks-and-hle-abi.md) (imports and the HLE registry), [ADR 0026](../../adr/0026-objective-c-strategy.md) (host classes), [ADR 0030](../../adr/0030-runtime-diagnostics.md) (diagnostics) and [ADR 0070](../../adr/0070-settings-registry-and-presets.md) (the settings registry).
