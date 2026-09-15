"""Bounded local Usage Tracker adapter; never copies transcript bodies."""
import argparse
import json
import time
from pathlib import Path
from codex_usage_tracker.kernel.application import RuntimePaths, build_application
from codex_usage_tracker.kernel.application.codec import json_value
from codex_usage_tracker.kernel.ingest import KernelIngestor, RefreshTrigger
from codex_usage_tracker.kernel.live import GenerationJournal

parser = argparse.ArgumentParser()
parser.add_argument('--session', required=True, help='Exact local session JSONL path')
args = parser.parse_args()
source = Path(args.session).resolve(strict=True)
repo = Path(__file__).resolve().parents[2]
cache = repo / 'Build/tooling/usage/cache'
paths = RuntimePaths(source.parent, cache)
start = time.perf_counter()
result = KernelIngestor(paths.kernel.analytical, paths.kernel.operational,
    journal=GenerationJournal(paths.kernel.operational)).refresh([source],
    trigger=RefreshTrigger.MCP_USAGE_REFRESH, owner_id='hs-bounded-query')
refresh_seconds = time.perf_counter()-start
app = build_application(paths)
request = {'requests':[{'dataset':'calls','operation':'aggregate',
    'measures':['calls','input_tokens','cached_input_tokens','output_tokens','total_tokens']} ]}
start = time.perf_counter()
summary = app.query(request)
query_seconds = time.perf_counter()-start
report = {'refresh_seconds':refresh_seconds,'query_seconds':query_seconds,
    'refresh':json_value(result),'summary':summary}
print(json.dumps(report,ensure_ascii=False))
