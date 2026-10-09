"""Ledger-level frontier worker tests without launching BizHawk or Codex."""

import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from scripts.autonomy.continuous import audit_state, serve
from scripts.autonomy.frontier_cycle_job import queue_next, register, run_lease
from scripts.autonomy.job_store import JobStore
from scripts.autonomy.supervisor import SupervisorError, file_sha256
from scripts.phase95_frontier_graph import FrontierGraph


REPO = Path(__file__).resolve().parents[1]
PRIVATE = REPO / "tools" / "private"


def fixture(root):
    state = root / "supervisor"
    cycle = root / "cycle"
    cycle.mkdir()
    graph = FrontierGraph()
    graph.add_node("start", "checkpoint")
    graph.add_node("target", "objective")
    graph.add_edge("search", "start", "target", "objective",
                   source_variant="us", required_capabilities=("search-exit",),
                   metadata={"driver": "search-exit", "exit_id": "a" * 64,
                             "max_nodes": 128, "max_expansions": 40,
                             "radius": 25, "checkpoint_sha256": "b" * 64})
    graph.add_entrypoint("oracle-us", "start")
    graph_path = cycle / "graph.json"
    graph.save(graph_path)
    cycle_state = {"kind": "jfg-phase95-frontier-cycle", "schema": 1,
                   "graph": graph_path.relative_to(PRIVATE).as_posix(),
                   "graph_sha256": file_sha256(graph_path), "next_index": 0,
                   "attempt": 0, "jobs": [], "pending_export": None,
                   "pending_verification": None, "pending_discovery": None,
                   "verification": None, "status": "active"}
    (cycle / "state.json").write_text(json.dumps(cycle_state))
    emulator = root / "emulator.exe"
    rom = root / "game.z64"
    script = root / "bridge.lua"
    emulator.write_bytes(b"emulator")
    rom.write_bytes(b"private synthetic fixture")
    script.write_bytes(b"script")
    register(REPO, state, cycle, emulator, rom, script, file_sha256(rom))
    return state, cycle, graph_path


class FrontierCycleJobTests(unittest.TestCase):
    def setUp(self):
        PRIVATE.mkdir(parents=True, exist_ok=True)
        # queue_next refuses to run with under 50 GiB free. These tests use
        # small fixtures, so report ample space unless a test patches it
        # itself (see test_frontier_queue_rejects_low_disk_reserve).
        disk = patch("scripts.autonomy.frontier_cycle_job.shutil.disk_usage")
        disk.start().return_value.free = 1 << 40
        self.addCleanup(disk.stop)

    def test_pending_node_replay_is_agent_free_maintenance_step(self):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, cycle, _ = fixture(Path(directory))
            current = json.loads((cycle / "state.json").read_text())
            current["pending_node_verification"] = {
                "job_index": 0, "attempt": 0, "node_id": 1,
                "worker_pointer": "synthetic/worker"}
            (cycle / "state.json").write_text(json.dumps(current))
            with JobStore(state / "jobs.sqlite") as store:
                job_id = queue_next(store, REPO, state)
                packet = json.loads((state / "frontier-packets" /
                                     (job_id + ".json")).read_text())
                self.assertTrue(packet["maintenance"])
                self.assertIsNone(packet["selected_edge"])
                lease = store.lease_job(job_id, "test", ttl=120)
                calls = []

                def fake_command(argv, *_args, **_kwargs):
                    calls.append(argv)
                    resumed = json.loads((cycle / "state.json").read_text())
                    resumed["pending_node_verification"] = None
                    resumed["status"] = "budget_exhausted"
                    (cycle / "state.json").write_text(json.dumps(resumed))
                    return 0, None

                outcome = run_lease(store, lease, REPO, state,
                                    command_runner=fake_command)
                self.assertIn("maintained sealed", outcome)
                self.assertEqual(calls[0][calls[0].index("--max-jobs") + 1], "0")
                self.assertEqual(store.job(job_id)["state"], "passed")
                self.assertNotEqual(queue_next(store, REPO, state), job_id)

    def test_one_bounded_result_is_sealed_and_next_step_is_distinct(self):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, cycle, graph_path = fixture(Path(directory))
            with JobStore(state / "jobs.sqlite") as store:
                job_id = queue_next(store, REPO, state)
                self.assertIsNotNone(job_id)
                self.assertIsNone(queue_next(store, REPO, state))
                lease = store.lease_job(job_id, "test", ttl=120)
                calls = []
                limits = []

                def fake_command(argv, *_args, **_kwargs):
                    calls.append(argv)
                    limits.append(_kwargs)
                    graph = FrontierGraph.load(graph_path)
                    graph.record("bounded", "search", "oracle-us", "blocked",
                                 "job-0000-01/result.json",
                                 reason="bounded search incomplete")
                    new_graph = cycle / "frontier-evidence.json"
                    graph.save(new_graph)
                    job_dir = cycle / "job-0000-01"
                    job_dir.mkdir()
                    (job_dir / "result.json").write_text(json.dumps({
                        "kind": "jfg-phase95-frontier-result", "edge_id": "search",
                        "outcome": "blocked", "planner_source_stable": True}))
                    current = json.loads((cycle / "state.json").read_text())
                    current.update(graph=new_graph.relative_to(PRIVATE).as_posix(),
                                   graph_sha256=file_sha256(new_graph), next_index=1,
                                   jobs=[{"index": 0, "edge": "search", "outcome": "blocked",
                                          "result": (job_dir / "result.json").relative_to(
                                              PRIVATE).as_posix()}],
                                   status="budget_exhausted")
                    (cycle / "state.json").write_text(json.dumps(current))
                    return 0, None

                outcome = run_lease(store, lease, REPO, state,
                                    command_runner=fake_command)
                self.assertIn("blocked sealed", outcome)
                self.assertEqual(calls[0][calls[0].index("--max-jobs") + 1], "1")
                self.assertEqual(limits[0]["minimum_free_bytes"],
                                 50 * 1024 * 1024 * 1024)
                self.assertEqual(limits[0]["max_output_tree_bytes"],
                                 4 * 1024 * 1024 * 1024)
                self.assertEqual(limits[0]["output_tree"], cycle / "job-0000-01")
                saved = store.job(job_id)
                self.assertEqual(saved["state"], "passed")
                result = json.loads(Path(saved["sealed_artifact"]).read_text())
                self.assertTrue(result["complete"])
                self.assertEqual(result["outcome"], "blocked")
                self.assertFalse(result["native_parity_verified"])
                self.assertNotEqual(queue_next(store, REPO, state), job_id)

    def test_frontier_queue_rejects_low_disk_reserve(self):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, _, _ = fixture(Path(directory))
            with JobStore(state / "jobs.sqlite") as store, patch(
                    "scripts.autonomy.frontier_cycle_job.shutil.disk_usage") as usage:
                usage.return_value.free = 0
                with self.assertRaisesRegex(SupervisorError, "disk reserve"):
                    queue_next(store, REPO, state)
                self.assertEqual(store.status_projection()["counts"]["queued"], 0)

    def test_registration_rejects_public_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            public = Path(directory)
            with self.assertRaisesRegex(SupervisorError, "private"):
                register(REPO, PRIVATE / "synthetic-state", public,
                         public / "emu", public / "rom", public / "lua",
                         "a" * 64)

    def test_changed_script_pin_stops_enqueue(self):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, cycle, _ = fixture(Path(directory))
            (Path(directory) / "bridge.lua").write_bytes(b"mutated")
            with JobStore(state / "jobs.sqlite") as store:
                with self.assertRaisesRegex(SupervisorError, "script pin changed"):
                    queue_next(store, REPO, state)

    # Model the dead Windows job owner; the production OS probe is tested separately.
    @patch("scripts.autonomy.supervisor.owner_process_dead", return_value=True)
    def test_expired_finished_step_is_recovered_without_relaunch(self, _dead_owner):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, cycle, graph_path = fixture(Path(directory))
            with JobStore(state / "jobs.sqlite") as store:
                job_id = queue_next(store, REPO, state)
                first = store.lease_job(job_id, "first", ttl=120)
                prior = state / "attempts" / job_id / "0001"
                prior.mkdir(parents=True)
                (prior / "frontier.guard.json").write_text(json.dumps({
                    "schema": 1, "state": "finished", "owner_pid": 999999}))
                job_dir = cycle / "job-0000-01"
                job_dir.mkdir()
                (job_dir / "result.json").write_text(json.dumps({
                    "kind": "jfg-phase95-frontier-result", "edge_id": "search",
                    "outcome": "blocked", "planner_source_stable": True}))
                current = json.loads((cycle / "state.json").read_text())
                current.update(next_index=1, jobs=[{
                    "index": 0, "edge": "search", "outcome": "blocked",
                    "result": (job_dir / "result.json").relative_to(PRIVATE).as_posix()}],
                    status="budget_exhausted")
                (cycle / "state.json").write_text(json.dumps(current))
                store.reclaim_expired(now=time.time() + 200)
                second = store.lease_job(job_id, "second", ttl=120,
                                         now=time.time() + 201)
                self.assertEqual(second["attempt"], 2)

                def forbidden(*_args, **_kwargs):
                    self.fail("completed frontier step must not be replayed")

                outcome = run_lease(store, second, REPO, state,
                                    command_runner=forbidden)
                self.assertIn("blocked sealed", outcome)
                self.assertEqual(store.job(job_id)["state"], "passed")

    # Model the dead Windows job owner; the production OS probe is tested separately.
    @patch("scripts.autonomy.supervisor.owner_process_dead", return_value=True)
    def test_agent_free_service_retries_expired_guarded_step(self, _dead_owner):
        with tempfile.TemporaryDirectory(dir=PRIVATE) as directory:
            state, cycle, graph_path = fixture(Path(directory))
            with JobStore(state / "jobs.sqlite") as store:
                job_id = queue_next(store, REPO, state)
                first = store.lease_job(job_id, "first", ttl=120)
                store.start(job_id, first["token"])
                prior = state / "attempts" / job_id / "0001"
                prior.mkdir(parents=True)
                (prior / "frontier.guard.json").write_text(json.dumps({
                    "schema": 1, "state": "guarded", "owner_pid": 999999,
                    "child_pid": 999998}))
                current = json.loads((cycle / "state.json").read_text())
                current["attempt"] = 1
                (cycle / "state.json").write_text(json.dumps(current))
                store.reclaim_expired(now=time.time() + 200)

            calls = []

            def fake_command(argv, *_args, **_kwargs):
                calls.append(argv)
                graph = FrontierGraph.load(graph_path)
                graph.record("retry", "search", "oracle-us", "blocked",
                             "job-0000-02/result.json",
                             reason="bounded search incomplete")
                new_graph = cycle / "retry-evidence.json"
                graph.save(new_graph)
                job_dir = cycle / "job-0000-02"
                job_dir.mkdir()
                result_path = job_dir / "result.json"
                result_path.write_text(json.dumps({
                    "kind": "jfg-phase95-frontier-result", "edge_id": "search",
                    "outcome": "blocked", "planner_source_stable": True}))
                resumed = json.loads((cycle / "state.json").read_text())
                resumed.update(graph=new_graph.relative_to(PRIVATE).as_posix(),
                               graph_sha256=file_sha256(new_graph), next_index=1,
                               attempt=0, jobs=[{
                                   "index": 0, "edge": "search", "outcome": "blocked",
                                   "result": result_path.relative_to(PRIVATE).as_posix()}],
                               status="budget_exhausted")
                (cycle / "state.json").write_text(json.dumps(resumed))
                return 0, None

            def retry(store, lease, repo, supervisor_state):
                return run_lease(store, lease, repo, supervisor_state,
                                 command_runner=fake_command)

            with patch("scripts.autonomy.frontier_cycle_job.run_lease",
                       side_effect=retry):
                outcomes = serve(REPO, state, Path("unused-codex"),
                                 max_cycles=1, max_agent_attempts_per_utc_day=0)
            self.assertEqual(outcomes, [f"{job_id}: frontier blocked sealed"])
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][calls[0].index("--max-jobs") + 1], "1")
            with JobStore(state / "jobs.sqlite") as store:
                self.assertEqual([(entry["number"], entry["outcome"])
                                  for entry in store.attempt_history(job_id)],
                                 [(1, "expired"), (2, "passed")])
                self.assertEqual(audit_state(store, state)["passed_verified"], 1)


if __name__ == "__main__":
    unittest.main()
