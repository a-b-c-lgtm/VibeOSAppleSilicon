# Chapter 119 — HTML forms: submit buttons and GET wiring

This chapter lands the first end-to-end forms slice in the
browser: clicking a submit control now walks its owning
`<form>`, serializes controls into a query string, resolves
`action=`, and navigates to the resulting URL.

This is intentionally a thin vertical slice. It does not yet
add in-page text editing, caret/focus management for form
controls, or POST bodies. But it is a real milestone because
the browser now follows the classic form flow many sites rely
on: button click -> encoded request parameters -> navigation.

## Why this chapter exists

Before this change, the browser could:

- fetch and render documents,
- follow links by clicking `<a href=...>`,
- navigate from the URL bar.

But form controls were visual only. A click on a `<button
type=submit>` or `<input type=submit>` did nothing.

That gap blocked a large class of pages, even without
JavaScript. The pre-AJAX web was mostly links + forms.
Getting forms moving again is the right first step before
cookies and SOP in chapter 120.

## What ships in this slice

### 1. Submit-control hit testing

On mouse click in page content, the browser now:

1. hit-tests to the deepest layout box,
2. walks up the DOM ancestors,
3. detects a clicked submit control:
   - `<button>` with missing/empty `type` or `type=submit`
   - `<input>` with missing/empty `type` or `type=submit`
4. finds the nearest ancestor `<form>`.

If no submit control is found, normal link/navigation behavior
stays unchanged.

### 2. GET query construction from form descendants

The serializer walks the form subtree and emits name/value
pairs into `application/x-www-form-urlencoded` query shape:

- includes only controls with a non-empty `name`,
- uses `value` attributes from DOM,
- handles submit controls correctly:
  - includes only the clicked submit control's pair,
  - excludes other submit/button controls,
- handles checkbox/radio defaults:
  - only includes checked controls,
  - uses `value=on` if checked with no explicit value,
- skips unsupported-in-this-slice kinds such as file/reset.

Encoding rules match classic HTML form behavior:

- unreserved characters stay literal,
- space becomes `+`,
- all other bytes become `%HH` (uppercase hex).

### 3. Action resolution + canonicalization

`action=` resolution uses the same URL logic already used by
browser navigation:

- absolute URLs stay absolute,
- relative actions resolve against current page URL,
- resulting URL is fed through canonicalization/proxy rules.

If `action` is omitted, submit defaults to the current page URL.

### 4. GET navigation handoff

After query generation, the browser appends it to the resolved
action URL and runs the normal navigation path (`browser_navigate`),
so history, repaint, parser thread behavior, and URL bar updates
follow existing browser semantics.

### 5. Form controls get recognizable button chrome

To make submit controls visually obvious, browser rendering now
adds button-like chrome based on paint-stream DOM metadata.
This piggybacks on shared `libgui` drawing primitives so the
same button look can be reused by other apps.

This part matters ergonomically: users now see what is clickable,
instead of guessing from plain text.

## What this slice does not do yet

This chapter documents the implemented behavior, not the final
forms end-state. The following remain future work:

- in-page text entry (`<input type=text>` editing),
- Tab/Enter focus traversal among controls,
- textarea/select semantics,
- POST request body generation and headers,
- per-control runtime state mutation beyond static DOM attrs.

If a form uses `method=post`, the browser currently reports it
as unsupported and does not submit.

## Key implementation points

### DOM-first, not box-type-first

The submit path deliberately walks DOM ancestry rather than
depending on new layout box kinds. That keeps the first slice
small and robust: if layout representation changes, form
submission logic still keys off semantic HTML elements.

### Iterative tree walks

Form collection uses iterative traversal with bounded stacks,
matching the browser's broader strategy of avoiding deep
recursion in constrained userspace thread stacks.

### Reuse existing navigation pipeline

By handing the final URL to existing navigation code, forms
automatically inherit working behavior for:

- history/back-forward,
- proxy/TLS bridge canonicalization,
- parser-thread coalescing,
- GUI redraw lifecycle.

That keeps Chapter 119 focused: forms compute a destination;
the existing browser machinery does the rest.

## Local deterministic fixture: forms.html

To avoid external site variability and rate limits, this chapter
adds a local fixture page at:

- `assets/osfs/forms.html`

It is included in the OS image build and contains simple GET
forms with visible submit controls and predictable targets under
`/mnt/...`.

Use it as the default bring-up target for form regressions.

## Manual verification flow

1. Boot to desktop and launch browser.
2. Open `http://127.0.0.1:80/mnt/forms.html`.
3. Click the first submit button.
4. Confirm navigation to the target URL with query parameters.
5. Go back, click the second submit button, confirm navigation.

If this works, the click -> form collect -> encode -> navigate
path is alive end-to-end.

## Failure modes found during bring-up

- Button visuals were initially too subtle to identify as
  controls; paint-stream DOM grouping plus shared chrome fixed it.
- Endpoint behavior can still fail independently of form logic
  (for example, upstream 429s), so local fixtures are required
  to isolate browser-side regressions.

## Applied to

- Existing app modified:
  - `userspace/browser/browser.c`
- Shared UI primitive reused:
  - `userspace/libgui/draw.h`
- Fixture added:
  - `assets/osfs/forms.html`
- Build wiring updated:
  - `Makefile` (OSFS inclusion)
- Existing tests upgraded:
  - none yet dedicated to forms submission
- New tests added:
  - none yet (currently validated with local fixture + manual run)

## What this unlocks

- Real submit-button flows on static/form-driven pages.
- A clean staging point for chapter 120 cookies/SOP work.
- The next implementation step: editable form inputs and POST,
  built on top of this already-working GET submit path.
