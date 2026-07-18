"""Logger API 동작 테스트: 파일 출력 내용, 레지스트리, 레벨, 패턴, async, 타입 계약."""
import os
import re
import subprocess
import sys
import time

import pytest
import spdlog_swyang as spdlog


@pytest.fixture
def file_logger(tmp_path, request):
    name = f"beh-{request.node.name}"
    path = tmp_path / "out.log"
    lg = spdlog.FileLogger(name=name, filename=str(path),
                           multithreaded=False, truncate=True)
    try:
        yield lg, path
    finally:
        lg.close()


class TestFileOutput:
    def test_info_roundtrip_exact_lines(self, file_logger):
        lg, path = file_logger
        lg.set_pattern("%v")
        msgs = ["hello", "한글 메시지", "", "with,comma", "x" * 1000]
        for m in msgs:
            lg.info(m)
        lg.close()
        assert path.read_text(encoding="utf-8").splitlines() == msgs

    def test_default_pattern_contains_message_and_level(self, file_logger):
        lg, path = file_logger
        lg.info("needle-abc")
        lg.close()
        text = path.read_text()
        assert "needle-abc" in text and "[info]" in text

    def test_flush_on_makes_lines_visible_without_close(self, file_logger):
        # market_collector 실사용 경로: flush_on(INFO) → close 없이 즉시 관측 가능해야 함
        lg, path = file_logger
        lg.set_pattern("%v")
        lg.flush_on(spdlog.LogLevel.INFO)
        lg.info("immediate")
        assert "immediate" in path.read_text()

    def test_level_filtering_in_file(self, file_logger):
        lg, path = file_logger
        lg.set_pattern("%v")
        lg.set_level(spdlog.LogLevel.WARN)
        lg.info("filtered-out")
        lg.warn("kept")
        lg.close()
        lines = path.read_text().splitlines()
        assert lines == ["kept"]

    def test_utc_pattern_variant(self, file_logger):
        # utc 타입은 전용 포맷터 대체 없이 일반 pattern_formatter 경로
        lg, path = file_logger
        lg.set_pattern("%H:%M:%S.%e,%v", spdlog.PatternTimeType.utc)
        lg.info("payload")
        lg.close()
        assert re.match(r"^\d{2}:\d{2}:\d{2}\.\d{3},payload$",
                        path.read_text().splitlines()[0])


class TestTypeContract:
    def test_bytes_message_rejected(self, file_logger):
        lg, _ = file_logger
        with pytest.raises(TypeError):
            lg.info(b"bytes-not-allowed")

    def test_bytes_filename_rejected(self, tmp_path):
        with pytest.raises(TypeError):
            spdlog.FileLogger(name="bytes-fn", filename=str(tmp_path / "x.log").encode(),
                              multithreaded=False, truncate=True)

    def test_non_str_message_rejected(self, file_logger):
        lg, _ = file_logger
        with pytest.raises(TypeError):
            lg.info(123)


class TestRegistry:
    def test_get_returns_registered_logger(self, file_logger):
        lg, _ = file_logger
        assert spdlog.get(lg.name()).name() == lg.name()

    def test_get_unknown_raises(self):
        with pytest.raises(RuntimeError, match="could not be found"):
            spdlog.get("no-such-logger-xyz")

    def test_get_after_close_raises(self, tmp_path):
        lg = spdlog.FileLogger(name="closes", filename=str(tmp_path / "c.log"),
                               multithreaded=False, truncate=True)
        lg.close()
        with pytest.raises(RuntimeError):
            spdlog.get("closes")

    def test_same_name_after_close_reusable(self, tmp_path):
        for _ in range(3):
            lg = spdlog.FileLogger(name="reuse-me", filename=str(tmp_path / "r.log"),
                                   multithreaded=False, truncate=True)
            try:
                lg.info("ok")
            finally:
                lg.close()


class TestLevels:
    def test_level_roundtrip(self, file_logger):
        lg, _ = file_logger
        for lv in (spdlog.LogLevel.TRACE, spdlog.LogLevel.DEBUG, spdlog.LogLevel.INFO,
                   spdlog.LogLevel.WARN, spdlog.LogLevel.ERR, spdlog.LogLevel.CRITICAL,
                   spdlog.LogLevel.OFF):
            lg.set_level(lv)
            assert lg.level() == lv

    def test_should_log(self, file_logger):
        lg, _ = file_logger
        lg.set_level(spdlog.LogLevel.INFO)
        assert not lg.should_log(spdlog.LogLevel.DEBUG)
        assert lg.should_log(spdlog.LogLevel.INFO)
        assert lg.should_log(spdlog.LogLevel.ERR)


class TestMisc:
    def test_sinks_list(self, file_logger):
        lg, _ = file_logger
        sinks = lg.sinks()
        assert isinstance(sinks, list) and len(sinks) == 1
        sinks[0].set_level(spdlog.LogLevel.INFO)  # smoke

    def test_underlying_logger_type(self, file_logger):
        lg, _ = file_logger
        assert type(lg.get_underlying_logger()).__name__ == "_spd_logger"

    def test_error_handler_smoke(self, file_logger):
        lg, _ = file_logger
        lg.set_error_handler(lambda msg: None)

    def test_rotating_and_daily_smoke(self, tmp_path):
        rl = spdlog.RotatingLogger("rot-smoke", str(tmp_path / "rot.log"),
                                   False, 1 << 20, 3)
        try:
            rl.info("rotating")
        finally:
            rl.close()
        dl = spdlog.DailyLogger("daily-smoke", str(tmp_path / "daily.log"))
        try:
            dl.info("daily")
        finally:
            dl.close()
        assert any(p.name.startswith("rot") for p in tmp_path.iterdir())


class TestAsync:
    def test_async_file_logging_subprocess(self, tmp_path):
        # set_async_mode는 전역 상태(스레드풀·기본 async 플래그)를 바꾸므로 서브프로세스에서 실행
        logfile = tmp_path / "async.log"
        script = f"""
import spdlog_swyang as spdlog
spdlog.set_async_mode(queue_size=1 << 14)
lg = spdlog.FileLogger(name='async-t', filename={str(logfile)!r},
                       multithreaded=False, truncate=True)
assert lg.async_mode() is True
lg.set_pattern('%v')
lg.set_csv_schema('i,f2')
for i in range(500):
    lg.log_csv(i, i + 0.5)
lg.flush()
lg.close()
"""
        subprocess.run([sys.executable, "-c", script], check=True, timeout=60)
        deadline = time.time() + 5
        lines = []
        while time.time() < deadline:
            if logfile.exists():
                lines = logfile.read_text().splitlines()
                if len(lines) == 500:
                    break
            time.sleep(0.1)
        assert len(lines) == 500
        assert lines[0] == "0,0.50" and lines[-1] == "499,499.50"
