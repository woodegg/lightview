#!/usr/bin/env python3
"""Export the profile cards available in the current LinkedIn people search."""
import argparse
import csv
import json
import socket
import time
import urllib.parse
from pathlib import Path

import importlib.machinery
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("lightviewctl", str(ROOT / "tools/lightviewctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)

EXTRACT = r"""Array.from(document.querySelectorAll('[role=listitem]')).map(card => {
  const lines = card.innerText.split('\n').map(x => x.trim()).filter(Boolean);
  const primary = Array.from(card.querySelectorAll('a[href*="/in/"]'))
    .find(a => /\u2022\s*(1st|2nd|3rd\+?)/.test(a.innerText));
  if (!primary) return null;
  const first = lines[0] || '';
  const match = first.match(/^(.*?)\s*\u2022\s*(1st|2nd|3rd\+?)/);
  if (!match) return null;
  const stop = lines.findIndex((line, index) => index > 0 &&
    /^(Connect|Message|Follow|Pending)$/.test(line));
  const details = lines.slice(1, stop > 0 ? stop : 3);
  return {
    name: match[1].trim(), degree: match[2],
    headline: details[0] || '', location: details[1] || '',
    profile_url: primary.href.split('?')[0]
  };
}).filter(Boolean)"""


def with_page(url, page):
    parts = urllib.parse.urlsplit(url)
    query = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
    query = [(key, value) for key, value in query if key != "page"]
    # LinkedIn's client-side search can retain the previous page when `page` is
    # omitted, so keep an explicit page=1 as well.
    query.append(("page", str(page)))
    return urllib.parse.urlunsplit(parts._replace(query=urllib.parse.urlencode(query)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--max-pages", type=int, default=100)
    args = parser.parse_args()

    state = ctl.request(args.socket, "status")
    if "linkedin.com/search/results/people/" not in state["uri"]:
        parser.error("the browser must be on a LinkedIn people-search page")
    base_url = state["uri"]
    records = {}
    prior_urls = None
    last_page = 0

    for page in range(1, args.max_pages + 1):
        target = with_page(base_url, page)
        ctl.request(args.socket, "open", uri=target)
        deadline = time.monotonic() + 30
        cards = []
        while time.monotonic() < deadline:
            current = ctl.request(args.socket, "status", timeout=5)
            if current["load_error"]:
                raise RuntimeError(current["load_error"])
            cards = ctl.request(args.socket, "eval", timeout=5, script=EXTRACT)
            current_urls = tuple(card["profile_url"] for card in cards)
            if cards and current_urls != prior_urls:
                break
            time.sleep(0.25)
        else:
            break

        current_urls = tuple(card["profile_url"] for card in cards)
        if not cards or current_urls == prior_urls:
            break
        prior_urls = current_urls
        last_page = page
        for card in cards:
            records.setdefault(card["profile_url"], card)
        print(f"page {page}: {len(cards)} profiles; {len(records)} unique", flush=True)

        nav = ctl.request(args.socket, "eval", timeout=5, script=
            "Array.from(document.querySelectorAll('button')).some(b => "
            "b.innerText.trim() === 'Next' && !b.disabled)")
        if not nav:
            break
        time.sleep(0.6)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream,
            fieldnames=["name", "degree", "headline", "location", "profile_url"])
        writer.writeheader()
        writer.writerows(records.values())
    print(json.dumps({"output": str(args.output), "profiles": len(records),
                      "pages": last_page}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, socket.timeout, RuntimeError) as error:
        raise SystemExit(f"export-linkedin-search: {error}")
