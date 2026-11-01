# AI USAGE POLICY

## [Version 1](https://github.com/Logic-gate/AI-USAGE-POLICY)

## Project

**Project name:** `ProCam`

**Policy version:** AI Usage Policy, Version 1

**Effective date:** `18/09/2026`

## Preamble

Artificial intelligence can be a useful development tool. It can help with
research, code review, documentation, testing, and implementation.

It can also produce incorrect, insecure, irrelevant, or unmaintainable work
that appears convincing at first glance.

This policy does not prohibit the use of AI. It requires the person using it
to remain responsible for the work.

The purpose of this policy is to protect maintainers from low-effort
contributions and to make sure that AI-assisted work is reviewed, understood,
and verified before it reaches the project.

## Terms and Conditions

### 0. Definitions

For the purpose of this policy:

**AI Tool** means any system that generates, rewrites, reviews, translates,
summarizes, or materially assists with code, text, images, audio, video, or
other project content.

This includes tools such as Codex, Claude Code, Cursor, Amp, Copilot, ChatGPT,
and similar systems.

**AI-Assisted Work** means any contribution where an AI tool materially
influenced the submitted result.

**Contribution** means any code, patch, issue, discussion, documentation,
test, benchmark, media, review, or other material submitted to the project.

**Contributor** means the person submitting the contribution.

**Maintainer** means a person trusted by the project to review, accept, reject,
or manage contributions.

**Project Rules** means the additional rules declared by a project under
Section 8 of this policy.

### 1. Scope and Acceptance

This policy applies to contributions made to a project that has adopted it.

It does not replace or modify the project's software licence. The software
licence controls the use, copying, modification, and distribution of the
software. This policy controls how contributions may be submitted.

By submitting a contribution, you agree to follow this policy and the
applicable Project Rules.

### 2. Disclosure

All AI usage must be disclosed.

The disclosure must state:

1. The AI tool or tools used.
2. Which parts of the contribution were AI-assisted.
3. The extent of that assistance.
4. What the contributor personally reviewed, tested, or changed.

A vague statement such as "AI was used" is not enough.

A suitable disclosure would be:

> AI assistance: Codex was used to draft the initial implementation.
> I reviewed and modified the generated code, tested the affected paths,
> and wrote the final error handling manually.

Failure to disclose AI usage may result in the contribution being rejected,
even where the contribution would otherwise be acceptable.

### 3. Contributor Responsibility

The contributor remains fully responsible for the submitted work.

Using an AI tool does not transfer responsibility for:

* Incorrect behavior.
* Security problems.
* Performance regressions.
* Licensing problems.
* Fabricated information.
* Poor design.
* Unmaintainable code.
* Damage caused to the project or its users.

An AI tool cannot accept responsibility on your behalf.

### 4. Human Understanding

The contributor must understand the submitted work.

You must be able to explain:

* What the contribution does.
* Why the change is needed.
* How the implementation works.
* How it interacts with the rest of the project.
* What risks or limitations it introduces.
* How it was tested.

This explanation must be possible without asking an AI tool to explain the
work for you.

If you cannot explain your contribution, do not submit it.

### 5. Review and Verification

AI-assisted work must be checked before submission.

The contributor must:

1. Read the complete contribution.
2. Correct errors and remove unnecessary output.
3. Verify factual and technical claims.
4. Test the affected behavior.
5. Review security and performance implications where relevant.
6. Make sure the work follows the project's existing style and architecture.

AI-assisted work must meet the same standards as work written without AI.

Copying generated output directly into a contribution without meaningful
review is not acceptable.

### 6. Issues, Discussions, and Documentation

AI tools may be used to assist with issues, discussions, documentation, and
other written material.

The contributor must still review and edit the result before submitting it.

AI-generated writing often includes repetition, unnecessary explanation,
unsupported claims, and details that do not apply to the project. The
contributor must remove this material.

The contributor must also perform their own research and verify any technical
claims, citations, or proposed solutions.

AI must not be used to create the appearance of knowledge or investigation
that did not take place.

### 7. Prohibited Uses

AI must not be used to fabricate or misrepresent:

* Test results.
* Logs.
* Benchmarks.
* Citations.
* Sources.
* Reproduction steps.
* Security findings.
* User reports.
* Review results.
* Compatibility claims.
* Statements that a command or test was run when it was not.

Generated evidence must never be presented as real evidence.

AI must also not be used to hide authorship, evade review, impersonate another
person, or submit work under misleading claims.

### 8. Project Rules

A project adopting this policy may define additional Project Rules.

Project Rules may specify:

* Which AI tools are allowed or prohibited.
* Where AI disclosure must appear.
* The required disclosure format.
* Testing requirements for AI-assisted code.
* Whether AI-generated media is allowed.
* Whether AI-generated translations are allowed.
* Whether maintainers have additional permissions.
* Areas of the codebase where AI assistance is restricted.
* Additional review or approval requirements.
* Any other project-specific condition.

Project Rules form part of this policy once published by the project.

Project Rules may add or strengthen requirements. Any exception to this
policy must be written clearly and must identify the section being modified.
An exception must not be implied.

If Project Rules conflict with this policy, the stricter requirement applies
unless the Project Rules contain an explicit exception.

A recommended Project Rules template is provided in Appendix A.

### 9. AI-Generated Media

AI-generated art, images, icons, video, audio, voice, music, and similar media
are not allowed unless the Project Rules explicitly permit them.

Text and code may be AI-assisted when all other requirements of this policy
are followed.

A project that permits AI-generated media must state which types of media are
allowed and what disclosure or review is required.

### 10. Maintainers

Project Rules may permit maintainers to use AI tools at their discretion.

This does not remove maintainer responsibility.

Maintainers remain responsible for understanding, reviewing, testing, and
maintaining any AI-assisted work they accept into the project.

Maintainer access is based on established trust and responsibility. It is not
a general exemption from quality standards.

### 11. Enforcement

A contribution that does not follow this policy may be:

* Rejected.
* Closed without further review.
* Returned for correction.
* Removed after acceptance.
* Treated as a breach of contributor trust.

Contributors who repeatedly submit undisclosed, unreviewed, misleading, or
low-quality AI-assisted work may be blocked from future participation.

Serious or repeated abuse may be documented publicly where the maintainers
believe this is necessary to protect the project or other projects.

Enforcement decisions remain with the maintainers.

### 12. Junior Contributors

The project may help junior contributors learn and improve.

That help requires the contributor to participate in the work, understand the
feedback, and show their own reasoning.

AI must not be used to avoid learning, bypass review, or create the appearance
of experience that the contributor does not have.

A contributor who wants help should ask questions, show their attempt, and
work through the review process.

### 13. There Are Humans Here

Projects are maintained by humans.

Every issue, discussion, review, and pull request takes time to read,
understand, test, and respond to.

Submitting low-effort or unqualified work is harmful because it moves the
burden of understanding and correcting that work onto the maintainer.

In an ideal world, AI would always produce accurate, useful, and maintainable
work. In practice, it still makes mistakes, misses context, and produces work
that can look correct without being correct.

Strict rules are necessary until the tools improve, the people using them
improve, or both.

### 14. AI Is Welcome

This policy is not an anti-AI position.

Many projects adopting this policy may use AI extensively. AI can be a useful
part of a responsible development workflow.

The problem is not the tool itself. The problem is using it without judgment,
understanding, verification, or accountability.

AI is welcome when it helps a person produce better work. It is not welcome
when it is used to move the cost of checking that work onto someone else.