#!/usr/bin/env python3
"""Repeatedly navigate Lightview and record survival, load state, and memory."""
import argparse
import csv
import importlib.machinery
import importlib.util
import json
import statistics
import subprocess
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("lightviewctl", str(ROOT / "tools/lightviewctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)

SITES = [
    ("Google", "https://www.google.com/"),
    ("YouTube", "https://www.youtube.com/"),
    ("YouTube Music", "https://music.youtube.com/"),
    ("Facebook", "https://www.facebook.com/"),
    ("Instagram", "https://www.instagram.com/"),
    ("X", "https://x.com/"),
    ("Reddit", "https://www.reddit.com/"),
    ("Wikipedia", "https://www.wikipedia.org/"),
    ("Amazon", "https://www.amazon.com/"),
    ("GitHub", "https://github.com/"),
    ("Microsoft", "https://www.microsoft.com/"),
    ("Apple", "https://www.apple.com/"),
    ("LinkedIn", "https://www.linkedin.com/"),
    ("Yahoo", "https://www.yahoo.com/"),
    ("Bing", "https://www.bing.com/"),
    ("Cloudflare", "https://www.cloudflare.com/"),
    ("CNN", "https://www.cnn.com/"),
]


def memory(pid):
    output = subprocess.check_output(
        [str(ROOT / "tools/lightview-memory"), str(pid)], text=True, timeout=10)
    return json.loads(output)


def process_group(usage, name):
    processes = [row for row in usage["processes"] if row.get("name") == name]
    return {
        "pss_mib": round(sum(row.get("pss_mib", 0) for row in processes), 2),
        "threads": sum(row.get("threads", 0) for row in processes),
        "fds": sum(row.get("fds", 0) for row in processes),
        "count": len(processes),
    }


class EngineFailed(RuntimeError):
    pass


def same_site(expected, actual):
    """Accept path/query redirects, but never count a blank/error page as success."""
    wanted = (urlsplit(expected).hostname or "").lower().removeprefix("www.")
    landed = (urlsplit(actual).hostname or "").lower().removeprefix("www.")
    return bool(wanted and landed and
                (wanted == landed or wanted.endswith("." + landed) or
                 landed.endswith("." + wanted)))


def blank_document(page):
    """Distinguish an actually empty document from a late page title."""
    return (not str(page.get("title", "")).strip() and
            int(page.get("bodyElements", 0)) == 0 and
            int(page.get("textLength", 0)) == 0)


def committed_destination_mismatch(row):
    """Audit CSV rows independently of transient address-bar state."""
    return bool(row.get("requested_url") and not same_site(
        row["requested_url"], row.get(
            "committed_uri", row.get("last_committed_uri", ""))))


def document_probe_failure(row):
    """Require a readable, populated document rather than address-bar metadata."""
    if row.get("ready_state") not in ("interactive", "complete"):
        return "unverified_document"
    try:
        elements, text, html = (int(row[field]) for field in
                                ("body_elements", "body_text_length", "html_length"))
    except (KeyError, TypeError, ValueError):
        return "unverified_document"
    if min(elements, text) < 0 or html <= 0:
        return "unverified_document"
    if elements == 0 and text == 0:
        return "blank_document"
    return None


def navigation_verified(row):
    return (not row.get("load_error") and
            not committed_destination_mismatch(row) and
            document_probe_failure(row) is None)


def navigation_healthy(row):
    if not navigation_verified(row) or row.get("reset_during_site") in (True, "True", "true", "1"):
        return False
    for counter in ("reset_count", "automation_timeout_count", "navigation_view_rebuild_count"):
        before, after = row.get(counter + "_before"), row.get(counter + "_after")
        if before not in (None, "") and after not in (None, "") and int(after) > int(before):
            return False
    return True


def usable_status(socket_path, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        state = ctl.request(socket_path, "status", timeout=3)
        if state["engine_state"] == "failed":
            raise EngineFailed(state.get("load_error") or "WebKit engine failed")
        if state["engine_state"] in ("ready", "suspended"):
            return state
        if time.monotonic() >= deadline:
            raise EngineFailed(f"WebKit remained {state['engine_state']} for {timeout} seconds")
        time.sleep(0.25)


def probe_document(socket_path, operation_timeout, ready_deadline):
    """Keep the resource wait separate from the bounded document-ready wait."""
    while True:
        page = ctl.request(socket_path, "eval", timeout=operation_timeout,
            script="({url:String(location.href),title:String(document.title),"
                   "ready:String(document.readyState),"
                   "bodyElements:document.body?document.body.getElementsByTagName('*').length:0,"
                   "bodyChildren:document.body?document.body.childElementCount:0,"
                   "textLength:document.body?document.body.innerText.length:0,"
                   "htmlLength:document.documentElement?document.documentElement.outerHTML.length:0})")
        remaining = ready_deadline - time.monotonic()
        if page["ready"] != "loading" or remaining <= 0:
            return page
        time.sleep(min(0.25, remaining))


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires at least one value")
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def median_pairwise_slope(points):
    """Return a robust units-per-hour slope for (elapsed_hours, value) pairs."""
    slopes = []
    for index, (left_time, left_value) in enumerate(points):
        for right_time, right_value in points[index + 1:]:
            if right_time > left_time:
                slopes.append((right_value - left_value) /
                              (right_time - left_time))
    return statistics.median(slopes) if slopes else 0.0


def analyze_resource_trends(rows):
    """Detect sustained post-warmup growth across comparable site rounds.

    A single heavy page or cache warmup is not a leak. The runner repeats the
    same ordered site set, so each complete round supplies comparable baseline,
    median, FD, and thread observations. A finding requires at least four hours
    of post-warmup evidence plus both a positive robust slope and a material
    early-to-late increase.
    """
    expected_sites = {name for name, _url in SITES}
    by_round = defaultdict(list)
    required = ("elapsed_seconds", "pss_mib", "main_fds", "network_fds",
                "web_fds", "main_threads", "network_threads", "web_threads")
    for row in rows:
        try:
            if not row.get("site") or any(row.get(field, "") == "" for field in required):
                continue
            round_number = int(row["round"])
            values = {field: float(row[field]) for field in required}
        except (KeyError, TypeError, ValueError):
            continue
        by_round[round_number].append((row["site"], values))

    summaries = []
    for round_number in sorted(by_round):
        entries = by_round[round_number]
        if {site for site, _values in entries} != expected_sites:
            continue
        elapsed = [values["elapsed_seconds"] / 3600 for _site, values in entries]
        pss = [values["pss_mib"] for _site, values in entries]
        total_fds = [values["main_fds"] + values["network_fds"] +
                     values["web_fds"] for _site, values in entries]
        total_threads = [values["main_threads"] + values["network_threads"] +
                         values["web_threads"] for _site, values in entries]
        summaries.append({
            "round": round_number,
            "elapsed_hours": statistics.median(elapsed),
            "pss_baseline_mib": percentile(pss, 0.2),
            "pss_median_mib": statistics.median(pss),
            "total_fds_median": statistics.median(total_fds),
            "total_threads_median": statistics.median(total_threads),
        })

    result = {
        "evaluable": False,
        "complete_rounds": len(summaries),
        "post_warmup_hours": 0.0,
        "sustained_growth_suspected": False,
        "growth_reasons": [],
        "metrics": {},
    }
    if len(summaries) < 12:
        return result

    warmup_rounds = max(3, len(summaries) // 10)
    observed = summaries[warmup_rounds:]
    span = observed[-1]["elapsed_hours"] - observed[0]["elapsed_hours"]
    result["post_warmup_hours"] = round(span, 3)
    if len(observed) < 9:
        return result
    result["evaluable"] = span >= 4

    window = max(3, len(observed) // 4)
    checks = {
        "pss_baseline_mib": (8.0, 128.0, 0.10),
        "pss_median_mib": (8.0, 128.0, 0.10),
        "total_fds_median": (2.0, 48.0, 0.20),
        "total_threads_median": (1.0, 24.0, 0.20),
    }
    for name, (minimum_slope, minimum_delta, minimum_fraction) in checks.items():
        points = [(row["elapsed_hours"], row[name]) for row in observed]
        early = statistics.median(row[name] for row in observed[:window])
        late = statistics.median(row[name] for row in observed[-window:])
        delta = late - early
        slope = median_pairwise_slope(points)
        threshold = max(minimum_delta, early * minimum_fraction)
        threshold_exceeded = slope > minimum_slope and delta > threshold
        suspected = result["evaluable"] and threshold_exceeded
        result["metrics"][name] = {
            "early": round(early, 3),
            "late": round(late, 3),
            "delta": round(delta, 3),
            "slope_per_hour": round(slope, 3),
            "material_delta_threshold": round(threshold, 3),
            "threshold_exceeded": threshold_exceeded,
            "suspected": suspected,
        }
        if suspected:
            result["growth_reasons"].append(name)

    result["sustained_growth_suspected"] = bool(result["growth_reasons"])
    return result


def assess_run(initial, final_state, rows, crashed, elapsed_seconds,
               duration_hours=None, resource_analysis=None):
    """Return the strict stability verdict and its machine-readable reasons."""
    survived = (not crashed and final_state is not None and
                final_state["engine_state"] in ("ready", "suspended"))
    load_error_count = sum(bool(row["load_error"]) for row in rows)
    sites_with_resets = sum(row["reset_during_site"] is True for row in rows)
    reset_count_delta = (max(0, final_state.get("reset_count", 0) -
                             initial.get("reset_count", 0))
                         if final_state else None)
    automation_timeout_count_delta = (
        max(0, final_state.get("automation_timeout_count", 0) -
            initial.get("automation_timeout_count", 0))
        if final_state else None)
    navigation_view_rebuild_count_delta = (
        max(0, final_state.get("navigation_view_rebuild_count", 0) -
            initial.get("navigation_view_rebuild_count", 0))
        if final_state else None)
    main_pid_stable = bool(final_state and
                           final_state.get("pid") == initial.get("pid"))
    duration_target_seconds = (duration_hours * 3600
                               if duration_hours is not None else None)
    duration_met = (duration_target_seconds is None or
                    elapsed_seconds >= duration_target_seconds)
    if resource_analysis is None:
        resource_analysis = analyze_resource_trends(rows)
    require_resource_trend = bool(duration_hours is not None and
                                  duration_hours >= 4)
    document_failures = [document_probe_failure(row) for row in rows]
    exercised_sites = {row.get("site") for row in rows if navigation_verified(row)}
    unexercised_sites = sorted({name for name, _url in SITES} - exercised_sites)

    failure_reasons = []
    if not survived:
        failure_reasons.append("browser_did_not_survive")
    if load_error_count:
        failure_reasons.append("load_errors")
    if any(committed_destination_mismatch(row) for row in rows):
        failure_reasons.append("uncommitted_destinations")
    if "unverified_document" in document_failures:
        failure_reasons.append("unverified_documents")
    if "blank_document" in document_failures:
        failure_reasons.append("blank_documents")
    if unexercised_sites:
        failure_reasons.append("site_coverage_incomplete")
    if reset_count_delta or sites_with_resets:
        failure_reasons.append("webkit_resets")
    if automation_timeout_count_delta:
        failure_reasons.append("automation_timeouts")
    if navigation_view_rebuild_count_delta:
        failure_reasons.append("webkit_view_rebuilds")
    if not main_pid_stable:
        failure_reasons.append("main_process_restarted")
    if not duration_met:
        failure_reasons.append("duration_not_met")
    if require_resource_trend and not resource_analysis["evaluable"]:
        failure_reasons.append("resource_trend_unavailable")
    elif resource_analysis["sustained_growth_suspected"]:
        failure_reasons.append("sustained_resource_growth")

    return {
        "passed": not failure_reasons,
        "survived": survived,
        "failure_reasons": failure_reasons,
        "load_errors": load_error_count,
        "unverified_documents": document_failures.count("unverified_document"),
        "blank_documents": document_failures.count("blank_document"),
        "unexercised_sites": unexercised_sites,
        "sites_with_resets": sites_with_resets,
        "reset_count_delta": reset_count_delta,
        "automation_timeout_count_delta": automation_timeout_count_delta,
        "navigation_view_rebuild_count_delta":
            navigation_view_rebuild_count_delta,
        "main_pid_stable": main_pid_stable,
        "duration_target_seconds": duration_target_seconds,
        "duration_met": duration_met,
        "resource_analysis": resource_analysis,
    }


def hard_failure_kind(initial, current, row):
    """Return the first strict invariant violated by a completed site row.

    Long runs should stop as soon as their final verdict is irrecoverably
    failed.  Preserve the row first so the triggering navigation remains in
    the CSV, then use the same monotonic counters as the final assessment.
    """
    if current and current.get("pid") != initial.get("pid"):
        return "main_process_restarted"
    if current and current.get("reset_count", 0) > initial.get("reset_count", 0):
        return "webkit_reset"
    if (current and current.get("automation_timeout_count", 0) >
            initial.get("automation_timeout_count", 0)):
        return "automation_timeout"
    if (current and current.get("navigation_view_rebuild_count", 0) >
            initial.get("navigation_view_rebuild_count", 0)):
        return "webkit_view_rebuild"
    if row.get("load_error"):
        return "load_error"
    if committed_destination_mismatch(row):
        return "uncommitted_destination"
    return document_probe_failure(row)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--rounds", type=int)
    parser.add_argument("--duration-hours", type=float,
                        help="run until this duration elapses; may be combined with --rounds")
    parser.add_argument("--load-timeout", type=float, default=12)
    parser.add_argument("--settle", type=float, default=2)
    parser.add_argument("--output", type=Path, default=Path("stability-results.csv"))
    args = parser.parse_args()
    if args.rounds is None and args.duration_hours is None:
        args.rounds = 2
    if ((args.rounds is not None and args.rounds < 1) or
            (args.duration_hours is not None and args.duration_hours <= 0) or
            args.load_timeout <= 0 or args.settle < 0):
        parser.error("rounds, duration, and timeouts must be positive")

    initial = usable_status(args.socket)
    # Match the browser's page-operation watchdog and leave enough client-side
    # time to receive its structured timeout/reset response. A fixed five-second
    # client timeout misclassifies a temporarily busy main thread as a browser
    # failure even when Lightview remains within its documented control limit.
    page_operation_timeout = max(5, initial.get("control_timeout_seconds", 30) + 5)
    # A top-level navigation has a wider bounded recovery path than an eval:
    # one full control budget followed by two half-budget retries in the same
    # WebView. Keep polling long enough to observe that recovery, while the
    # separate load timeout below still stops waiting quickly once a usable
    # document has committed but continues loading subresources indefinitely.
    navigation_timeout = max(
        5, initial.get("control_timeout_seconds", 30) * 2 + 5)
    fields = ["timestamp", "elapsed_seconds", "round", "site", "requested_url",
              "final_url", "title", "ready_state", "body_elements",
              "body_children", "body_text_length", "html_length",
              "load_error", "load_timed_out", "engine_state", "committed_uri",
              "last_committed_uri",
              "running_mode",
              "memory_kill_threshold_mib", "generation_before", "generation_after",
              "reset_count_before", "reset_count_after", "reset_during_site",
              "automation_timeout_count_before", "automation_timeout_count_after",
              "navigation_retry_count_before", "navigation_retry_count_after",
              "navigation_view_rebuild_count_before",
              "navigation_view_rebuild_count_after",
              "proactive_recycle_count_before", "proactive_recycle_count_after",
              "network_recycle_count_before", "network_recycle_count_after",
              "load_stop_count_before", "load_stop_count_after",
              "last_termination_reason", "control_clients", "page_requests_active",
              "pss_mib", "rss_mib", "processes",
              "main_pss_mib", "main_threads", "main_fds",
              "network_pss_mib", "network_threads", "network_fds", "network_processes",
              "web_pss_mib", "web_threads", "web_fds", "web_processes"]
    rows = []
    crashed = False
    failed_early = False
    failure_kind = None
    started = time.monotonic()
    run_deadline = started + args.duration_hours * 3600 if args.duration_hours else None
    round_number = 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        output_stream = args.output.open("x", encoding="utf-8-sig", newline="")
    except FileExistsError:
        parser.error(f"output already exists; choose a fresh CSV path: {args.output}")
    writer = csv.DictWriter(output_stream, fieldnames=fields)
    writer.writeheader()
    output_stream.flush()
    while args.rounds is None or round_number <= args.rounds:
        for name, url in SITES:
            if run_deadline is not None and time.monotonic() >= run_deadline:
                break
            row = {field: "" for field in fields}
            row.update(timestamp=datetime.now(timezone.utc).isoformat(),
                       elapsed_seconds=round(time.monotonic() - started, 3),
                       round=round_number, site=name, requested_url=url)
            current_state = None
            try:
                before = usable_status(args.socket)
                row.update(
                    generation_before=before["web_process_generation"],
                    reset_count_before=before["reset_count"],
                    automation_timeout_count_before=before.get(
                        "automation_timeout_count", 0),
                    navigation_retry_count_before=before.get(
                        "navigation_retry_count", 0),
                    navigation_view_rebuild_count_before=before.get(
                        "navigation_view_rebuild_count", 0),
                    proactive_recycle_count_before=before.get("proactive_recycle_count", 0),
                    network_recycle_count_before=before.get("network_process_recycle_count", 0),
                    load_stop_count_before=before.get("load_stop_count", 0),
                    running_mode=before["running_mode"],
                    memory_kill_threshold_mib=before["memory_kill_threshold_mib"],
                )
                ctl.request(args.socket, "open", timeout=3, uri=url)
                navigation_deadline = time.monotonic() + navigation_timeout
                matching_commit_at = None
                load_timed_out = False
                navigation_matched = False
                state = None
                while time.monotonic() < navigation_deadline:
                    state = ctl.request(args.socket, "status", timeout=3)
                    if state["engine_state"] == "failed":
                        raise EngineFailed(state.get("load_error") or "WebKit engine failed")
                    navigation_matched = ctl.navigation_has_committed(
                        state, before.get("navigation_sequence", 0), url)
                    if state["load_error"]:
                        break
                    if navigation_matched:
                        if matching_commit_at is None:
                            matching_commit_at = time.monotonic()
                        if state["load_error"] or not state["loading"]:
                            break
                        if time.monotonic() - matching_commit_at >= args.load_timeout:
                            load_timed_out = True
                            # Bound waiting for subresources, then verify that
                            # the committed document still accepts automation.
                            # Leave the load active for the next atomic open.
                            break
                    time.sleep(0.25)
                time.sleep(args.settle)
                state = usable_status(args.socket)
                navigation_matched = ctl.navigation_has_committed(
                    state, before.get("navigation_sequence", 0), url)
                # A page may keep loading subresources after commit. Record
                # that fact without using it to skip the document probe.
                if navigation_matched and state["loading"]:
                    load_timed_out = True
                row["load_timed_out"] = bool(load_timed_out or
                    state["engine_state"] != "ready")
                row.update(
                    engine_state=state["engine_state"],
                    committed_uri=state.get("committed_uri", state["last_committed_uri"]),
                    last_committed_uri=state["last_committed_uri"],
                    generation_after=state["web_process_generation"],
                    reset_count_after=state["reset_count"],
                    reset_during_site=state["reset_count"] > before["reset_count"],
                    automation_timeout_count_after=state.get(
                        "automation_timeout_count", 0),
                    navigation_retry_count_after=state.get(
                        "navigation_retry_count", 0),
                    navigation_view_rebuild_count_after=state.get(
                        "navigation_view_rebuild_count", 0),
                    proactive_recycle_count_after=state.get("proactive_recycle_count", 0),
                    network_recycle_count_after=state.get("network_process_recycle_count", 0),
                    load_stop_count_after=state.get("load_stop_count", 0),
                    last_termination_reason=state["last_termination_reason"] or "",
                )
                if not navigation_matched:
                    page = {"url": state["uri"], "title": state["title"],
                            "ready": "unavailable", "bodyElements": 0,
                            "bodyChildren": 0, "textLength": 0, "htmlLength": 0}
                    if not state["load_error"]:
                        state["load_error"] = (
                            f"NAVIGATION_TIMEOUT: requested {url}, reached "
                            f"{state.get('uri') or '<empty>'}")
                else:
                    # The shorter resource budget does not establish that the
                    # parser has finished. Give a head blocked on CSS/scripts
                    # the existing operation budget from commit to become a
                    # usable document; an expired loading document still fails.
                    ready_deadline = ((matching_commit_at or time.monotonic()) +
                        initial.get("control_timeout_seconds", 30))
                    try:
                        page = probe_document(args.socket, page_operation_timeout,
                                              ready_deadline)
                    except ctl.ControlError as error:
                        if not error.retryable:
                            raise
                        # Collect the recovered document as failure evidence;
                        # the monotonic counters still reject any timeout/reset.
                        recovery_deadline = time.monotonic() + page_operation_timeout
                        while True:
                            state = usable_status(args.socket)
                            if (not state["loading"] and
                                    same_site(url, state.get("uri", ""))):
                                break
                            if time.monotonic() >= recovery_deadline:
                                raise TimeoutError(
                                    f"Recovered WebKit did not restore {url}; "
                                    f"current URL is {state.get('uri') or '<empty>'}")
                            time.sleep(0.25)
                        row.update(
                            engine_state=state["engine_state"],
                            committed_uri=state.get("committed_uri", state["last_committed_uri"]),
                            last_committed_uri=state["last_committed_uri"],
                            generation_after=state["web_process_generation"],
                            reset_count_after=state["reset_count"],
                            reset_during_site=state["reset_count"] > before["reset_count"],
                            automation_timeout_count_after=state.get(
                                "automation_timeout_count", 0),
                            navigation_retry_count_after=state.get(
                                "navigation_retry_count", 0),
                            navigation_view_rebuild_count_after=state.get(
                                "navigation_view_rebuild_count", 0),
                            proactive_recycle_count_after=state.get("proactive_recycle_count", 0),
                            network_recycle_count_after=state.get("network_process_recycle_count", 0),
                            load_stop_count_after=state.get("load_stop_count", 0),
                            last_termination_reason=state["last_termination_reason"] or "",
                        )
                        page = probe_document(args.socket, page_operation_timeout,
                                              ready_deadline)
                    except Exception as error:
                        page = {"url": state["uri"], "title": state["title"],
                                "ready": "unavailable", "bodyElements": 0,
                                "bodyChildren": 0, "textLength": 0, "htmlLength": 0}
                        if not state["load_error"]:
                            state["load_error"] = f"PAGE_EVAL_FAILURE: {error}"
                if not state["load_error"] and not same_site(url, page["url"]):
                    state["load_error"] = (
                        f"FINAL_URL_MISMATCH: requested {url}, reached {page['url'] or '<empty>'}")
                if (not state["load_error"] and
                        page["ready"] not in ("loading", "ui-only") and
                        blank_document(page)):
                    state["load_error"] = "BLANK_DOCUMENT"
                control = ctl.request(args.socket, "status", timeout=3)
                current_state = control
                # The load or page-operation watchdog can fire while the page
                # probe is outstanding. Attribute counters and recovery state
                # from the final status rather than the earlier post-settle
                # snapshot, otherwise those events fall between CSV rows.
                row.update(
                    engine_state=control["engine_state"],
                    committed_uri=control.get("committed_uri", control["last_committed_uri"]),
                    last_committed_uri=control["last_committed_uri"],
                    generation_after=control["web_process_generation"],
                    reset_count_after=control["reset_count"],
                    reset_during_site=control["reset_count"] > before["reset_count"],
                    automation_timeout_count_after=control.get(
                        "automation_timeout_count", 0),
                    navigation_retry_count_after=control.get(
                        "navigation_retry_count", 0),
                    navigation_view_rebuild_count_after=control.get(
                        "navigation_view_rebuild_count", 0),
                    proactive_recycle_count_after=control.get(
                        "proactive_recycle_count", 0),
                    network_recycle_count_after=control.get(
                        "network_process_recycle_count", 0),
                    load_stop_count_after=control.get("load_stop_count", 0),
                    last_termination_reason=control["last_termination_reason"] or "",
                )
                if control.get("load_error"):
                    state["load_error"] = control["load_error"]
                usage = memory(initial["pid"])
                main = process_group(usage, "lightview")
                network = process_group(usage, "WebKitNetworkPr")
                web = process_group(usage, "WebKitWebProces")
                row.update(final_url=page["url"], title=page["title"],
                           ready_state=page["ready"],
                           body_elements=page.get("bodyElements", ""),
                           body_children=page.get("bodyChildren", ""),
                           body_text_length=page.get("textLength", ""),
                           html_length=page.get("htmlLength", ""),
                           load_error=state["load_error"] or "",
                           control_clients=control.get("control_clients", ""),
                           page_requests_active=control.get("page_requests_active", ""),
                           pss_mib=usage["total_pss_mib"], rss_mib=usage["total_rss_mib"],
                           processes=len(usage["processes"]),
                           main_pss_mib=main["pss_mib"], main_threads=main["threads"],
                           main_fds=main["fds"],
                           network_pss_mib=network["pss_mib"],
                           network_threads=network["threads"], network_fds=network["fds"],
                           network_processes=network["count"],
                           web_pss_mib=web["pss_mib"], web_threads=web["threads"],
                           web_fds=web["fds"], web_processes=web["count"])
                print(f"round {round_number} {name}: ready={row['ready_state']} "
                      f"error={bool(row['load_error'])} reset={row['reset_during_site']} "
                      f"PSS={row['pss_mib']} MiB", flush=True)
            except EngineFailed as error:
                row["load_error"] = f"ENGINE_FAILED: {error}"
                row["engine_state"] = "failed"
                print(f"round {round_number} {name}: permanent engine failure: {error}", flush=True)
                crashed = True
                failure_kind = "engine_failed"
            except Exception as error:
                try:
                    state = ctl.request(args.socket, "status", timeout=2)
                except Exception:
                    row["load_error"] = f"CONTROL_FAILURE: {error}"
                    print(f"round {round_number} {name}: browser/control failure: {error}", flush=True)
                    crashed = True
                    failure_kind = "control_failure"
                else:
                    current_state = state
                    if state["engine_state"] == "failed":
                        row["load_error"] = f"ENGINE_FAILED: {state.get('load_error') or error}"
                        row["engine_state"] = "failed"
                        print(f"round {round_number} {name}: permanent engine failure: {error}", flush=True)
                        crashed = True
                        failure_kind = "engine_failed"
                    else:
                        row["load_error"] = f"PAGE_FAILURE: {error}"
                        row["engine_state"] = state["engine_state"]
                        print(f"round {round_number} {name}: page failure, browser alive: {error}", flush=True)
            rows.append(row)
            writer.writerow(row)
            output_stream.flush()
            if not crashed:
                failure_kind = hard_failure_kind(initial, current_state, row)
                failed_early = failure_kind is not None
                if failed_early:
                    print(f"strict stability invariant failed: {failure_kind}; "
                          "stopping the run after preserving the evidence row",
                          flush=True)
            if crashed or failed_early:
                break
        if (crashed or failed_early or
                (run_deadline is not None and time.monotonic() >= run_deadline)):
            break
        round_number += 1

    output_stream.close()
    final_state = None
    try:
        final_state = ctl.request(args.socket, "status", timeout=3)
    except Exception:
        pass
    elapsed_seconds = time.monotonic() - started
    verdict = assess_run(initial, final_state, rows, crashed, elapsed_seconds,
                         args.duration_hours)

    print(json.dumps({"output": str(args.output), "navigations": len(rows),
                      "successful_navigations": sum(navigation_healthy(r)
                                                    for r in rows),
                      "duration_seconds": round(elapsed_seconds, 3),
                      **verdict,
                      "failure_kind": failure_kind,
                      "final_engine_state": final_state["engine_state"] if final_state else None,
                      "max_pss_mib": max((float(r["pss_mib"]) for r in rows if r["pss_mib"] != ""), default=None),
                      "load_timeouts": sum(r["load_timed_out"] is True for r in rows),
                      "proactive_recycles": (final_state.get("proactive_recycle_count", 0)
                                             if final_state else None),
                      "network_recycles": (final_state.get("network_process_recycle_count", 0)
                                            if final_state else None),
                      "navigation_retries": (final_state.get("navigation_retry_count", 0) -
                                               initial.get("navigation_retry_count", 0)
                                               if final_state else None),
                      "navigation_view_rebuilds": (
                          final_state.get("navigation_view_rebuild_count", 0) -
                          initial.get("navigation_view_rebuild_count", 0)
                          if final_state else None),
                      "max_control_clients": max((int(r["control_clients"]) for r in rows
                                                  if r["control_clients"] != ""), default=None),
                      "max_page_requests_active": max((int(r["page_requests_active"]) for r in rows
                                                       if r["page_requests_active"] != ""), default=None)}, indent=2))
    return 0 if verdict["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
