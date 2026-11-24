# Logs

Everything a command prints besides its data goes to stderr: errors, counted warnings, and messages about what it is doing. `common::Log` (`include/meshtinker/common/log.hpp`) writes them, and `cli::Command` sets it up from two options that every command takes:

- `--log-level <trace|debug|info|warn|error>`: print messages of this level and above. The default is `info` in both formats.
- `--log-format <text|json>`: text for people, the default, or one JSON object per line for a caller to parse as the lines arrive.

Both options are read before the command line is parsed, so a wrong command line is reported in the format asked for.

## Levels

The names are eCorpus's task log levels, so that a caller can pass them on as they are. There is no `fatal`: a command that fails logs an `error` and exits non-zero, and the exit code says how bad it is.

| Level | For |
| --- | --- |
| `trace` | Detail too fine for a normal run. Nothing uses it yet. |
| `debug` | What a command does in detail, such as the options `optimize` gives each step. |
| `info` | Progress: `optimize` logs each step as it starts and ends, with its time; `ktx` and `avif` each image as it is encoded, and `uv` each image as it is baked ([below](#progress-by-image)). Also the kept workspace with `--keep-tmp`, and the pointer to `--help` after a wrong command line. |
| `warn` | A counted diagnostic of `warning` severity. |
| `error` | The failure that ends the command, and diagnostics of `error` severity. |

Counted diagnostics are logged once each, when the command ends (or fails), with how many times each one happened and up to three examples. With `--json` they are also in the report on stdout.

## JSON lines

```json
{"level":"info","message":"Running simplify"}
{"level":"info","message":"simplify took 1 min 23 s"}
{"level":"warn","message":"steps that fail are left out, and the next one goes on from the stage before (1 time, e.g. meshtinker-uv failed: …)","code":"step-failed"}
{"level":"error","message":"cannot open 'scan.glb'","code":"file-unreadable"}
```

- `level` and `message` are always there. `message` is English, and is the text line without the program name, the severity or the code.
- `code` is only on diagnostics and failures. It is the stable code they carry in JSON reports: the report's `error.code` for a failure, and a diagnostic's `code` (see [inspect-report.md](inspect-report.md#diagnostic-codes) and each command's document). Callers can map codes to localized messages. Progress messages carry none.
- Other fields may come later, such as a `progress` figure. Parsers should ignore fields they don't know.
- Text that isn't valid UTF-8, such as a file name, is replaced rather than refused.

Not every line on stderr is JSON. The `meshtinker` wrapper prints its own errors (an unknown command, a program it can't run) as text. A sanitizer report, a crash, or a library writing to stderr are text too. A caller should treat a line that doesn't parse as a JSON object as a plain message.

## Text lines

GNU style, with the program's name first, since the steps of `optimize` share its stderr:

```text
meshtinker-optimize: Running simplify
meshtinker-simplify: warning: Primitives smaller than the error were removed (3 times, e.g. mesh "Rock", primitive 0) [primitive-removed]
meshtinker-optimize: simplify took 12.3 s
meshtinker-optimize: error: cannot open 'scan.glb' [file-unreadable]
```

`info` lines have no severity, `debug` and `trace` lines say `debug:` and `trace:`, and the code ends a line in brackets.

## Progress by image

Images take most of the time of `ktx`, `avif` and `uv`, so each one is logged at `info` once it is done, with its place in the count:

```text
meshtinker-ktx: Encoded image 3 of 12: rock.jpg 4096x4096 -> rock.ktx2 2048x2048, 12.3 MB -> 1.2 MB in 12.3 s
meshtinker-avif: Kept image 4 of 12 as it was: moss.png is 324 B, the new file would be 582 B
meshtinker-uv: Baked image 1 of 3 for Rock: rock.jpg 4096x4096 -> rock.png 3072x3072, 12.3 MB -> 9.8 MB in 4.1 s
```

- `ktx` and `avif` count the images they encode, without those left out from their header alone (missing, small, KTX2, in a format they can't decode, or used both as color and as data), and log them in the order they finish: large images one at a time, then small ones in batches ([images.md](images.md#memory-and-threads)). An image left as it was gets a `Left` or `Kept` line, so the count has no gaps; the warning with its reason comes with the diagnostics at the end.
- `uv` counts the images of each material group, named after its first material, and logs only the groups it repacks. A uniform image keeps its file (`Kept`). An image that can't be decoded stops the group (`Stopped at`), which is then left as it is, with its warning.

## Commands that run others

`optimize` passes `--log-level` and `--log-format` on to every step, when they aren't the defaults, so that the steps' lines on the shared stderr all come in one format ([chaining.md](chaining.md)). The options appear in each step's `argv` in the report.

## Sizes and times in text

Text reports and log messages show sizes in decimal units (`999 B`, `12.3 kB`, `45.6 MB`, `1.23 GB`, as file managers show them) and times as `850 ms`, `12.3 s`, `1 min 23 s` or `2 h 05 min`, through `common::formatBytes` and `common::formatDuration` (`include/meshtinker/common/format.hpp`). JSON keeps the raw figures, in bytes and seconds, for callers to format their own way.
