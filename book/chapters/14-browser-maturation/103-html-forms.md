# Chapter 103 — HTML forms: input, button, submit

**Status:** Stub. Tracking milestone 96.

The browser can render and click links. It cannot yet
*type into* a page. This chapter teaches forms — `<input
type=text>`, `<button>`, `<form action method>` — and
builds the GET/POST submit path on top of the existing
fetch pipeline.

## What this chapter adds

- New layout box flavour: `LAY_BOX_INPUT` with a focused
  state and a per-input text buffer.
- DOM tracks a `value` attribute that is initially the
  HTML's `value=` and is mutated by typing.
- `<form>` collects all `<input name=...>` descendants
  on submit; URL-encodes them; appends to URL for GET or
  to body for POST.
- POST adds a `Content-Type: application/x-www-form-
  urlencoded` and a real Content-Length to the request.
- A test page in `/data/www/forms.html` with the most basic
  shapes.

## Prerequisites

- Chapter 71 — browser
- Chapter 102 — local httpd to submit to (for testability)

## Plan

- Hit-test: clicks on an `<input>` focus it; subsequent
  keystrokes go to that input's buffer (same dispatch
  rules as the URL bar).
- Tab key cycles focus between inputs in DOM order.
- Enter inside a single-input form submits.
- httpd in chapter 101 grows a tiny POST handler that
  echoes the parsed form back as HTML, so the regression
  test is round-trippable.

## What you'll learn

- Why forms are the simplest possible interactive web
  primitive — pre-1995 web was essentially this and
  nothing else.
- URL encoding's sharp corners (space-as-plus vs %20).

## What this unlocks

- Login forms (once cookies land in chapter 104).
- A path to richer UI: textarea, select, checkbox.
