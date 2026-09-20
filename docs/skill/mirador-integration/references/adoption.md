# Make This Skill Available To A Downstream AI

An AI started in an application repository cannot automatically discover a skill kept in Mirador's source tree. Use one of these explicit paths.

## Read It In Place

When Mirador is cloned, vendored, or checked out beside the application, give the agent this instruction:

```text
Read /path/to/mirador/docs/skill/mirador-integration/SKILL.md before integrating Mirador.
```

Use the actual checked-out path. This is the lowest-cost option and keeps the agent on the exact library revision in use.

## Copy It With The Application

Copy the complete `docs/skill/mirador-integration/` directory into a documented location in the application repository, then put its exact `SKILL.md` path in that project's agent instruction or task prompt. Keep the `references/` directory beside it; the entry file links to it.

Refresh the copy whenever Mirador is upgraded. Do not copy only `SKILL.md`, because capability cards are loaded on demand.

## Select The Right Audience

This skill is for using Mirador in an application. Contributors changing Mirador's own implementation follow the repository-level rules instead: [AGENTS.md](../../../../AGENTS.md), the [engineering standards](../../../project/project-standards.md), and the [design document](../../../design/mirador-development-design.md); do not load those for normal integration work.
