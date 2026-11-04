---
name: commit
description: Commit staged/working changes using repo conventions
---
1. Run `git status` and `git diff`. Run `git log --oneline -15` to mirror the existing style.
2. Group changes into LOGICAL commits. Never mix unrelated or pre-existing work into one commit.
3. Exclude build artifacts (*.o, *.elf, *.hex, *.bin, build/).
4. Message format: `LLM: <imperative summary>` for Claude's work, `MIXED: <summary>` only for mixed authorship. NEVER feat:/fix:.
5. Show the planned commit list, then commit.