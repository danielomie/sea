# AGENTS.md

Instructions for AI coding agents (Codex, Gemini CLI, Cursor, Copilot, Claude Code, ...).

- **Writing or reading Sea code:** Sea is a language you have not seen in
  training. Read [`docs/sea-for-ai.md`](docs/sea-for-ai.md) first: it lists the
  rules models get wrong (0-based arrays built with `[]` and `a[] = v`, `len()`,
  `for k, v in t do`, one statement per line, sealed struct literals, errors as
  values with `?`). Verify with `bin/sea check FILE` and `bin/sea fmt FILE`, run
  with `bin/sea FILE`, test with `bin/sea test`.
- **Working on the VM, compiler or libraries in this repository:** follow
  [`CLAUDE.md`](CLAUDE.md) (build and test commands, architecture, memory
  ownership rules, which docs to update).
- Full references: [`docs/reference.md`](docs/reference.md) (language and
  standard library), [`docs/libs.md`](docs/libs.md) (external libraries).
