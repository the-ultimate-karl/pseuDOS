# AGENTS.md

## Purpose

This repository is operated on by autonomous coding agents.

Agents are expected to behave as careful maintainers rather than blindly implementing changes. Investigate problems, fix bugs when possible, preserve existing behavior, document newly discovered bugs, and verify changes before considering a task complete.

---

# DO

## 1. Find and eliminate bugs

While implementing a task, actively look for bugs related to the code being modified.

If a bug can be safely and confidently fixed without expanding the task beyond reasonable scope, fix it.

Do not intentionally leave an obvious bug unfixed merely because it was not explicitly mentioned by the user.

However, do not "fix" behavior merely because it looks unusual. See the DON'T section regarding assumptions and intended behavior.

---

## 2. Track bugs that spawn during development

If implementing a change introduces, exposes, or causes additional bugs, record them in:

`BUGSPAWN.LOG`

Use this format:

```text
AGENTNAME (YYYY-MM-DD HH:MM)
- BUG1 AND CAUSE + PLACE
- BUG2 AND CAUSE + PLACE
- BUG3 AND CAUSE + PLACE
[IMPORTANT-BUGS]
- BUG1 AND REASON
- BUG2 AND REASON
- BUG3 AND REASON
```

The timestamp must use:

`YYYY-MM-DD HH:MM`

Do not overwrite, delete, reorder, or modify another agent's existing entries unless explicitly required to mark one of its bugs as fixed.

---

## 3. Never overwrite another agent's bug entry

If the same agent name already exists in `BUGSPAWN.LOG` but the current work is occurring on a different date, create a new entry after the previous entry.

Example:

```text
AGENT-A (2026-07-14 13:42)
- BUG1 ...
- BUG2 ...

[IMPORTANT-BUGS]
- BUG1 ...

AGENT-A (2026-07-25 09:17)
- BUG1 ...
- BUG2 ...

[IMPORTANT-BUGS]
- BUG1 ...
```

Never combine the two entries merely because the agent name is identical.

---

## 4. Mark fixed bugs

When a bug recorded in `BUGSPAWN.LOG` has actually been eliminated, mark that specific bug as fixed.

Use:

```text
- BUG1 AND CAUSE [FIXED + YYYY-MM-DD HH:MM]
```

Example:

```text
- Null pointer crash in renderer caused by missing initialization [FIXED + 2026-08-25 09:30]
```

Do not mark a bug as fixed unless the fix has actually been implemented and verified.

---

## 5. Do not investigate bugs already marked as fixed

If a bug in `BUGSPAWN.LOG` is marked:

`[FIXED + DATETIME]`

assume that bug has been fixed.

Do not repeatedly investigate or "fix" it again unless new evidence demonstrates that the same underlying bug has returned.

---

## 6. Verify your changes

After implementing changes:

1. Inspect the modified code.
2. Run relevant tests.
3. Build or compile the affected components.
4. Run linters, formatters, type checkers, or static analysis when available and relevant.
5. Check for regressions.
6. Review the final diff.
7. Confirm that files unrelated to the task were not accidentally modified.

Do not consider a change complete merely because the code looks correct.

---

## 7. Preserve existing behavior

Unless the task explicitly requests a behavior change, preserve existing functionality.

When modifying a component, understand how other components depend on it before changing its behavior.

---

## 8. Keep changes focused

Prefer the smallest change that correctly solves the problem.

If a larger refactor is genuinely necessary, explain why and verify that the refactor does not introduce unrelated regressions.

---

## 9. Read before modifying

Before changing unfamiliar code:

* Read the relevant files.
* Identify dependencies.
* Identify callers and consumers.
* Understand configuration involved.
* Check existing tests.
* Check relevant documentation.
* Check recent changes when available.

Do not modify code based solely on the first few lines you encounter.

---

## 10. Investigate failures instead of hiding them

If a test, build, command, or tool fails:

* Determine why it failed.
* Fix the underlying issue when appropriate.
* Document the failure if it cannot be fixed.
* Clearly report unresolved problems.

Do not simply disable the failing test, suppress the error, or remove the failing code to make the task appear successful.

---

# DON'T

## 1. Do not assume unusual behavior is a bug

Weird, inefficient, old-fashioned, or unconventional code may be intentional.

Treat unusual behavior as **intentional by default** unless:

* the user explicitly says it is unintended;
* another agent explicitly documents it as unintended;
* tests/documentation/specifications demonstrate that it is incorrect; or
* there is strong technical evidence that the behavior violates an established requirement.

Do not "clean up" unusual behavior just because you personally dislike it.

---

## 2. Do not invent requirements

Do not add functionality because you think the project "should" have it.

Implement the actual requirements, not imagined requirements.

If an important requirement is unclear, inspect existing project conventions and documentation before making a decision.

---

## 3. Do not perform unrelated refactors

Do not rewrite, reorganize, rename, modernize, optimize, or redesign unrelated code simply because you are already working nearby.

Avoid turning:

> "Fix this bug"

into:

> "Rewrite the entire subsystem."

---

## 4. Do not change public interfaces without a reason

Do not arbitrarily change:

* APIs
* function signatures
* command-line arguments
* configuration formats
* file formats
* network protocols
* database schemas
* exported symbols
* environment variables

unless the task requires it or compatibility has been deliberately considered.

---

## 5. Do not delete code merely because it appears unused

Code that appears unused may be:

* called dynamically;
* used by another platform;
* used by external consumers;
* required by configuration;
* intentionally reserved;
* part of an API;
* used by generated code.

Investigate before deleting.

---

## 6. Do not silently overwrite another agent's work

Do not discard, revert, or overwrite changes made by another agent unless:

* those changes are demonstrably incorrect;
* the task explicitly requires replacing them; or
* the changes directly conflict with the current implementation.

When possible, preserve compatible work from other agents.

---

## 7. Do not modify BUGSPAWN.LOG destructively

Never erase another agent's bug entries.

Never rewrite the entire file just to add your own entry.

Append new entries and make narrowly targeted modifications when marking specific bugs as fixed.

---

## 8. Do not claim something was tested when it was not

Never report:

* "tests passed" when tests were not run;
* "build succeeded" when no build was performed;
* "bug fixed" when the fix was not verified;
* "no regressions" when no meaningful verification was performed.

Be explicit about what was and was not verified.

---

## 9. Do not disable tests to make them pass

Do not:

* delete failing tests;
* skip tests without justification;
* weaken assertions;
* suppress errors;
* change expected values solely to match broken behavior;
* disable warnings simply to produce a clean result.

If the test is genuinely incorrect, establish why before changing it.

---

## 10. Do not hide errors

Do not add broad exception handling, silent fallbacks, empty catch blocks, or error suppression solely to make failures disappear.

Errors should be handled intentionally.

---

## 11. Do not make speculative fixes

Do not modify code merely because you suspect something *might* be wrong.

First establish:

1. what the behavior currently is;
2. what behavior is expected;
3. why the current behavior violates that expectation;
4. how the proposed change addresses the actual cause.

---

## 12. Do not optimize without evidence

Do not sacrifice readability, compatibility, correctness, or maintainability for hypothetical performance improvements.

If performance is the problem, measure it first when practical.

---

## 13. Do not change dependencies casually

Do not add, remove, upgrade, or downgrade dependencies unless necessary.

Dependency changes can introduce unrelated compatibility, security, build, and runtime problems.

---

## 14. Do not modify generated files manually unless required

If a file is generated automatically, modify its source/template/configuration instead whenever possible.

Only directly modify generated output when the project explicitly requires it.

---

## 15. Do not ignore existing conventions

Before introducing a new pattern, inspect how the repository already handles similar problems.

Prefer consistency with the existing architecture unless there is a concrete reason to change it.

---

## 16. Do not blindly trust comments

Comments can become outdated.

Use the actual implementation, tests, documentation, and observed behavior to determine how the system works.

Do not modify working code solely because a comment appears strange, and do not blindly preserve behavior solely because a comment claims it works a certain way.

---

## 17. Do not blindly trust tests either

Tests are evidence, not infallible truth.

If implementation, specification, documentation, and tests disagree, investigate the discrepancy instead of automatically changing whichever one is easiest.

---

## 18. Do not leave temporary artifacts behind

Remove temporary files, debugging output, experimental code, generated junk, and test artifacts unless they are intentionally part of the project.

Do not leave secrets, credentials, tokens, dumps, or personal data in the repository.

---

## 19. Do not expose secrets

Never commit or print sensitive credentials such as:

* API keys
* passwords
* authentication tokens
* private keys
* session cookies
* credentials stored in environment variables

If a secret is accidentally discovered, do not copy it into logs or commits.

---

## 20. Do not stop at "it compiles"

Compilation is not proof of correctness.

A successful build does not guarantee:

* correct runtime behavior;
* correct edge-case handling;
* compatibility;
* absence of regressions;
* correct error handling.

Perform the strongest practical verification available for the change.

---

# AFTER IMPLEMENTING CHANGES

Before finishing a task, the agent MUST:

1. Review all changes made.
2. Check for unintended modifications.
3. Run applicable tests.
4. Build/compile when applicable.
5. Investigate any newly discovered bugs.
6. Update `BUGSPAWN.LOG` if necessary.
7. Mark genuinely fixed bugs with `[FIXED + DATETIME]`.
8. Check for regressions.
9. Remove temporary artifacts.
10. Provide a concise summary of:

* what changed;
* what was tested;
* bugs discovered;
* bugs fixed;
* unresolved issues or limitations.

A task is not considered complete merely because the requested code was written. The implementation must also be inspected and verified.
