"""바인딩 핫패스 ns/건 벤치 (PERF_TUNING_CANDIDATES.md 항목별 전/후 측정용). 사용: python tests/bench_binding_hotpath.py [로그출력디렉토리]"""
import sys, time, statistics
import spdlog_swyang as spdlog

N = 200_000
RUNS = 5

def bench(fn, n):
    t0 = time.perf_counter_ns()
    fn(n)
    return (time.perf_counter_ns() - t0) / n

def make_logger(name, path):
    lg = spdlog.FileLogger(name=name, filename=path, multithreaded=False, truncate=True)
    lg.set_pattern("%H:%M:%S.%e,%v")
    return lg

out_dir = sys.argv[1] if len(sys.argv) > 1 else "/tmp"

# --- lob 40필드: 20 int + 20 float(f5) ---
lob_lg = make_logger("lob", f"{out_dir}/bench_lob.log")
lob_lg.set_csv_schema(",".join(["i"] * 20 + ["f5"] * 20))
lob_args = tuple(range(100, 120)) + tuple(x + 0.12345 for x in range(20))

def lob_csv(n):
    f = lob_lg.log_csv
    a = lob_args
    for _ in range(n):
        f(*a)

def lob_info_join(n):
    f = lob_lg.info
    ints = lob_args[:20]
    flts = lob_args[20:]
    for _ in range(n):
        f(",".join(map(str, ints)) + "," + ",".join(f"{x:.5f}" for x in flts))

# --- lob 40필드 str 폴백 (LS증권 실데이터: 숫자가 str로 옴) ---
lobs_lg = make_logger("lobs", f"{out_dir}/bench_lobs.log")
lobs_lg.set_csv_schema(",".join(["i"] * 40))
lobs_args = tuple(str(x) for x in range(1000, 1040))

def lob_csv_strfallback(n):
    f = lobs_lg.log_csv
    a = lobs_args
    for _ in range(n):
        f(*a)

# --- tick 5필드 ---
tick_lg = make_logger("tick", f"{out_dir}/bench_tick.log")
tick_lg.set_csv_schema("i,i,f3,f5,i")
tick_args = (12345, 678, 901.234, 5.6789, 42)

def tick_csv(n):
    f = tick_lg.log_csv
    a = tick_args
    for _ in range(n):
        f(*a)

def tick_fstring(n):
    f = tick_lg.info
    a, b, c, d, e = tick_args
    for _ in range(n):
        f(f"{a},{b},{c:.3f},{d:.5f},{e}")

# --- 순수 info() 고정 문자열 (string_view 경로, 바인딩 고정비 측정) ---
info_lg = make_logger("info", f"{out_dir}/bench_info.log")
MSG100 = "x" * 100

def info_fixed(n):
    f = info_lg.info
    m = MSG100
    for _ in range(n):
        f(m)

cases = [
    ("lob40 log_csv", lob_csv),
    ("lob40 info(join)", lob_info_join),
    ("lob40 log_csv str-fallback", lob_csv_strfallback),
    ("tick5 log_csv", tick_csv),
    ("tick5 info(f-string)", tick_fstring),
    ("info(100B fixed)", info_fixed),
]

for name, fn in cases:
    fn(20_000)  # warmup
    times = [bench(fn, N) for _ in range(RUNS)]
    print(f"{name:32s} {statistics.mean(times):8.1f} ns/call  (min {min(times):7.1f}, max {max(times):7.1f})")

for lg in (lob_lg, lobs_lg, tick_lg, info_lg):
    lg.close()
