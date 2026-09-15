---
name: project-hs-tooling
description: Use the measured Project HS tooling workflow for C++ semantic navigation, development-cost investigation, or GPU capture diagnosis.
---

Use `Tools/tooling/WORKFLOW.md` for tested commands and adoption evidence.
For a known C++ symbol, prefer hs_lsp definition/symbol tools over reading whole
files. Keep rg for literal text and file discovery. LSP diagnostics supplement
MSVC tests; never treat them as the compiler oracle. Cross-file references need
a completed index and the relevant compilation database; core excludes Runtime.
Do not enable automatic post-edit hooks or bulk rename without inspecting scope.
Use `Tools/verify.ps1` for builds. sccache is excluded: its tested Windows output
broke Ninja header tracking despite cache speedups. Do not enable cached presets.
Use usage/GPU tools only for a concrete question, not on every edit. Keep raw
sessions and captures in ignored Build/artifact paths. Never upload session logs.
