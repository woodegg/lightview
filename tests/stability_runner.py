#!/usr/bin/env python3
"""Unit checks for the strict long-run stability verdict."""

import importlib.util
import contextlib
import io
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "stability_test", ROOT / "tools/stability-test.py")
STABILITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STABILITY)


class StabilityVerdictTest(unittest.TestCase):
    def setUp(self):
        self.initial = {"pid": 10, "reset_count": 0,
                        "navigation_view_rebuild_count": 0}
        self.final = {"pid": 10, "reset_count": 0,
                      "navigation_view_rebuild_count": 0,
                      "engine_state": "ready"}
        self.clean_row = {"load_error": "", "reset_during_site": False,
                          "ready_state": "complete", "body_elements": 10,
                          "body_text_length": 50, "html_length": 100,
                          "pss_mib": 600}
        self.clean_analysis = {
            "evaluable": True,
            "sustained_growth_suspected": False,
        }

    def assess(self, *, final=None, rows=None, crashed=False, elapsed=86400,
               analysis=None):
        return STABILITY.assess_run(
            self.initial, self.final if final is None else final,
            [dict(self.clean_row, site=name, requested_url=url,
                  committed_uri=url) for name, url in STABILITY.SITES]
            if rows is None else rows,
            crashed, elapsed, 24,
            self.clean_analysis if analysis is None else analysis)

    def test_clean_full_duration_passes(self):
        self.assertTrue(self.assess()["passed"])

    def test_parser_can_finish_within_the_existing_ready_budget(self):
        with (mock.patch.object(STABILITY.ctl, "request", side_effect=[
                {"ready": "loading"}, {"ready": "interactive"}]) as request,
              mock.patch.object(STABILITY.time, "monotonic", side_effect=[1, 2]),
              mock.patch.object(STABILITY.time, "sleep")):
            page = STABILITY.probe_document("/unused", 65, 60)
        self.assertEqual(page["ready"], "interactive")
        self.assertEqual(request.call_count, 2)

    def test_a_parser_that_never_finishes_still_fails(self):
        with (mock.patch.object(STABILITY.ctl, "request", return_value={
                "ready": "loading"}) as request,
              mock.patch.object(STABILITY.time, "monotonic", side_effect=[1, 60]),
              mock.patch.object(STABILITY.time, "sleep")):
            page = STABILITY.probe_document("/unused", 65, 60)
        row = dict(self.clean_row, ready_state=page["ready"])
        self.assertFalse(STABILITY.navigation_verified(row))
        self.assertEqual(request.call_count, 2)

    def test_document_probe_errors_are_not_hidden_by_readiness_wait(self):
        with (mock.patch.object(STABILITY.ctl, "request", side_effect=TimeoutError("stalled")),
              self.assertRaises(TimeoutError)):
            STABILITY.probe_document("/unused", 65, 60)

    def test_recovered_document_is_not_a_healthy_navigation(self):
        self.assertTrue(STABILITY.navigation_healthy(self.clean_row))
        for changed in ({"reset_during_site": True},
                        {"reset_count_before": 0, "reset_count_after": 1},
                        {"automation_timeout_count_before": 0, "automation_timeout_count_after": 1},
                        {"navigation_view_rebuild_count_before": 0,
                         "navigation_view_rebuild_count_after": 1}):
            row = dict(self.clean_row, **changed)
            self.assertTrue(STABILITY.navigation_verified(row))
            self.assertFalse(STABILITY.navigation_healthy(row))

    def test_restart_cannot_overwrite_existing_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.csv"
            output.write_text("existing evidence\n")
            with (mock.patch.object(sys, "argv", ["stability-test.py",
                  "--socket", "/unused", "--output", str(output)]),
                  mock.patch.object(STABILITY, "usable_status", return_value=self.initial),
                  mock.patch.object(STABILITY.ctl, "request") as request,
                  contextlib.redirect_stderr(io.StringIO()),
                  self.assertRaises(SystemExit) as raised):
                STABILITY.main()
            self.assertEqual(raised.exception.code, 2)
            self.assertEqual(output.read_text(), "existing evidence\n")
            request.assert_not_called()

    def test_load_error_fails(self):
        result = self.assess(rows=[{
            "load_error": "navigation failed", "reset_during_site": False}])
        self.assertFalse(result["passed"])
        self.assertIn("load_errors", result["failure_reasons"])

    def test_ui_only_navigation_is_not_verified_content(self):
        row = dict(self.clean_row, ready_state="ui-only", load_timed_out=True)
        result = self.assess(rows=[row])
        self.assertFalse(result["passed"])
        self.assertIn("unverified_documents", result["failure_reasons"])
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, self.final, row), "unverified_document")

    def test_titled_empty_body_is_still_blank(self):
        row = dict(self.clean_row, title="CNN", body_elements=0,
                   body_text_length=0)
        result = self.assess(rows=[row])
        self.assertIn("blank_documents", result["failure_reasons"])
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, self.final, row), "blank_document")

    def test_missing_page_probe_metrics_cannot_pass(self):
        row = dict(self.clean_row)
        del row["body_elements"]
        self.assertIn("unverified_documents",
                      self.assess(rows=[row])["failure_reasons"])

    def test_continuing_resource_load_can_pass_with_verified_document(self):
        rows = [dict(self.clean_row, site=name, requested_url=url,
                     committed_uri=url, load_timed_out=True,
                     ready_state="interactive") for name, url in STABILITY.SITES]
        self.assertTrue(self.assess(rows=rows)["passed"])

    def test_duration_without_complete_site_coverage_cannot_pass(self):
        result = self.assess(rows=[dict(self.clean_row, site="Google")])
        self.assertIn("site_coverage_incomplete", result["failure_reasons"])
        self.assertIn("CNN", result["unexercised_sites"])

    def test_provisional_address_does_not_count_as_committed_destination(self):
        state = {"engine_state": "ready", "navigation_sequence": 2,
                 "uri": "https://www.cnn.com/", "last_committed_uri": "about:blank",
                 "loading": False, "load_error": None}
        self.assertFalse(STABILITY.ctl.navigation_has_committed(
            state, 1, "https://www.cnn.com/"))
        committed = dict(state, navigation_sequence=3,
                         last_committed_uri="https://www.cnn.com/")
        self.assertTrue(STABILITY.ctl.navigation_has_committed(
            committed, 1, "https://www.cnn.com/"))
        self.assertFalse(STABILITY.ctl.navigation_has_committed(
            committed, 3, "https://www.cnn.com/"))
        # Recovery retains last-visited metadata, but the current document may
        # still be its local bootstrap even when a remote URL is provisional.
        historical = dict(committed, committed_uri="about:blank")
        self.assertFalse(STABILITY.ctl.navigation_has_committed(
            historical, 1, "https://www.cnn.com/"))

        # The CLI must not probe or return against the completed local document
        # before it has observed the later destination commit.
        with mock.patch.object(STABILITY.ctl, "request",
                               side_effect=[state, committed, True]) as request:
            with mock.patch.object(STABILITY.ctl.time, "sleep"):
                self.assertTrue(STABILITY.ctl.wait_for_navigation(
                    "/unused", 1, "https://www.cnn.com/", 1))
        self.assertEqual([call.args[1] for call in request.call_args_list],
                         ["status", "status", "eval"])

    def test_blank_commit_fails_even_if_row_has_no_load_error(self):
        row = dict(self.clean_row, requested_url="https://www.cnn.com/",
                   final_url="https://www.cnn.com/", last_committed_uri="about:blank")
        result = self.assess(rows=[row])
        self.assertFalse(result["passed"])
        self.assertIn("uncommitted_destinations", result["failure_reasons"])
        self.assertEqual(STABILITY.hard_failure_kind(self.initial, self.final, row),
                         "uncommitted_destination")
        historical = dict(row, last_committed_uri="https://www.cnn.com/",
                          committed_uri="about:blank")
        self.assertIn("uncommitted_destinations",
                      self.assess(rows=[historical])["failure_reasons"])

    def test_reset_fails_from_final_counter_or_site_record(self):
        reset_final = dict(self.final, reset_count=1)
        self.assertIn("webkit_resets",
                      self.assess(final=reset_final)["failure_reasons"])
        reset_row = {"load_error": "", "reset_during_site": True}
        self.assertIn("webkit_resets",
                      self.assess(rows=[reset_row])["failure_reasons"])

    def test_automation_timeout_fails(self):
        timed_out = dict(self.final, automation_timeout_count=1)
        self.assertIn("automation_timeouts",
                      self.assess(final=timed_out)["failure_reasons"])

    def test_webview_rebuild_fails(self):
        rebuilt = dict(self.final, navigation_view_rebuild_count=1)
        result = self.assess(final=rebuilt)
        self.assertIn("webkit_view_rebuilds", result["failure_reasons"])
        self.assertEqual(result["navigation_view_rebuild_count_delta"], 1)

    def test_main_process_change_fails(self):
        restarted = dict(self.final, pid=11)
        self.assertIn("main_process_restarted",
                      self.assess(final=restarted)["failure_reasons"])

    def test_hard_failure_kind_stops_on_each_strict_runtime_invariant(self):
        current = dict(self.final, automation_timeout_count=0)
        row = dict(self.clean_row)
        self.assertIsNone(STABILITY.hard_failure_kind(
            self.initial, current, row))

        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, dict(current, pid=11), row),
            "main_process_restarted")
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, dict(current, reset_count=1), row),
            "webkit_reset")
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, dict(current, automation_timeout_count=1), row),
            "automation_timeout")
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial,
            dict(current, navigation_view_rebuild_count=1), row),
            "webkit_view_rebuild")
        self.assertEqual(STABILITY.hard_failure_kind(
            self.initial, current,
            dict(row, load_error="navigation failed")),
            "load_error")

    def test_short_duration_fails(self):
        self.assertIn("duration_not_met",
                      self.assess(elapsed=86399.9)["failure_reasons"])

    def test_dead_browser_fails(self):
        result = self.assess(final={}, crashed=True)
        self.assertFalse(result["survived"])
        self.assertIn("browser_did_not_survive", result["failure_reasons"])

    def test_unavailable_or_suspicious_resource_trend_fails(self):
        unavailable = {"evaluable": False, "sustained_growth_suspected": False}
        suspicious = {"evaluable": True, "sustained_growth_suspected": True}
        self.assertIn("resource_trend_unavailable",
                      self.assess(analysis=unavailable)["failure_reasons"])
        self.assertIn("sustained_resource_growth",
                      self.assess(analysis=suspicious)["failure_reasons"])

    @staticmethod
    def resource_rows(growth_per_hour=0):
        rows = []
        for round_number in range(1, 21):
            elapsed_hours = (round_number - 1) * 0.3
            for site_index, (site, _url) in enumerate(STABILITY.SITES):
                pss = 600 + site_index * 20 + elapsed_hours * growth_per_hour
                rows.append({
                    "round": round_number,
                    "site": site,
                    "elapsed_seconds": elapsed_hours * 3600 + site_index,
                    "pss_mib": pss,
                    "main_fds": 30,
                    "network_fds": 40,
                    "web_fds": 50,
                    "main_threads": 10,
                    "network_threads": 20,
                    "web_threads": 30,
                })
        return rows

    def test_resource_trend_distinguishes_plateau_from_growth(self):
        stable = STABILITY.analyze_resource_trends(self.resource_rows())
        self.assertTrue(stable["evaluable"])
        self.assertFalse(stable["sustained_growth_suspected"])

        growing = STABILITY.analyze_resource_trends(
            self.resource_rows(growth_per_hour=40))
        self.assertTrue(growing["evaluable"])
        self.assertTrue(growing["sustained_growth_suspected"])
        self.assertIn("pss_baseline_mib", growing["growth_reasons"])

    def test_short_resource_run_reports_preview_without_diagnosis(self):
        rows = self.resource_rows(growth_per_hour=400)
        for row in rows:
            row["elapsed_seconds"] = float(row["elapsed_seconds"]) / 10
        preview = STABILITY.analyze_resource_trends(rows)
        self.assertFalse(preview["evaluable"])
        self.assertTrue(preview["metrics"]["pss_baseline_mib"]
                        ["threshold_exceeded"])
        self.assertFalse(preview["sustained_growth_suspected"])

    def test_blank_document_requires_empty_title_and_body(self):
        self.assertTrue(STABILITY.blank_document({
            "title": "", "bodyElements": 0, "textLength": 0}))
        self.assertFalse(STABILITY.blank_document({
            "title": "", "bodyElements": 12, "textLength": 0}))
        self.assertFalse(STABILITY.blank_document({
            "title": "", "bodyElements": 0, "textLength": 20}))
        self.assertFalse(STABILITY.blank_document({
            "title": "YouTube Music", "bodyElements": 0, "textLength": 0}))


if __name__ == "__main__":
    unittest.main()
