# Documents & Documentation

Principles of great documentation:

- Used sparingly. Every line of documentation is additional debt and maintenance, not checked by automated processes. Documentation is earned on its value. It alone has negative value.
- Describes current reality only.
- Omits transaction history. We have version history for viewing the previous state of affairs.
- Is not a source of truth. The code is a source of truth. Do not duplicate implementation details.
- Links to other documents where reasonably possible.
- Brief on specifics - the more technical detail included, the faster it will become stale.

Summed up - write as little documentation as possible. Do not document or refer to the current item of work. Write documentation that has minimal ongoing maintenance costs.

Documentation is lube for understanding context, it cannot replace well-written, readable code. It cannot crutch complicated systems.

## Formatting

- Generally, prefer statements over narrative prose.
- Lists are preferable to "x, y and z" patterns.

# Task Completion

- Check changed files for lint diagnostics before considering a task complete.
- Resolve diagnostics; use narrow, justified suppressions only where required.
- Report any lint checks that could not be completed.
