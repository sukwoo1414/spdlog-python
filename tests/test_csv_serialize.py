"""log_csv / format_csv / set_csv_schema 핫패스 계약 테스트.

핵심 계약 (PERF_TUNING_CANDIDATES.md 검증 방법):
  format_csv(*args) == 같은 스키마의 파이썬 f-string 결과 (바이트 단위 동일)
  - "i"  토큰: f"{x}"   (int64 범위 밖·비정수는 str(x) 폴백 — 동일 출력)
  - "fN" 토큰: f"{x:.Nf}" (nan은 부호 무시 "nan")
라인 버퍼는 4096B 고정 — 초과는 RuntimeError, 정확히 4096B는 성공해야 한다.
"""
import math
import random

import pytest
import spdlog_swyang as spdlog

BUF = 4096  # src/pyspdlog.cpp log_csv/format_csv의 스택 버퍼 크기와 일치해야 함


@pytest.fixture
def logger(tmp_path, request):
    name = f"csv-{request.node.name}"
    lg = spdlog.FileLogger(name=name, filename=str(tmp_path / "t.log"),
                           multithreaded=False, truncate=True)
    try:
        yield lg
    finally:
        lg.close()


def py_format(schema, args):
    """스키마 계약을 파이썬 f-string으로 그대로 재현한 기대값."""
    out = []
    for tok, a in zip(schema.split(","), args):
        if tok == "i":
            out.append(f"{a}")
        else:
            out.append(f"{a:.{int(tok[1:])}f}")
    return ",".join(out)


def check(logger, schema, args):
    logger.set_csv_schema(schema)
    assert logger.format_csv(*args) == py_format(schema, args)


# ---------- 바이트 동일성: 타입별 ----------

class TestByteEquality:
    def test_int_basics(self, logger):
        check(logger, "i,i,i,i,i", (0, 1, -1, 2**63 - 1, -2**63))

    def test_int64_overflow_falls_back_to_str(self, logger):
        check(logger, "i,i,i", (2**63, -2**63 - 1, 2**200))

    @pytest.mark.parametrize("prec", [0, 1, 2, 3, 5, 9, 17])
    def test_float_precisions(self, logger, prec):
        vals = (0.0, -0.0, 1.5, -1.5, 0.1, math.pi, 1e300, 1e-300, 5e-324,
                123456789.987654321, 1e15 + 0.125)
        check(logger, ",".join([f"f{prec}"] * len(vals)), vals)

    def test_nan_inf(self, logger):
        check(logger, "f3,f3,f3,f3", (float("nan"), float("-nan"), math.inf, -math.inf))

    def test_str_fallback_on_i(self, logger):
        # LS증권 실데이터 경로: 숫자가 str로 온다 ('+'/'-' 부호, 빈 문자열 포함)
        check(logger, "i,i,i,i,i", ("123", "+45", "-67", "", "007"))

    def test_str_fallback_unicode_and_specials(self, logger):
        check(logger, "i,i,i", ("한글값", "a,b", "line\nbreak"))

    def test_bool(self, logger):
        check(logger, "i,i,f3", (True, False, True))

    def test_float_on_i_and_int_on_f(self, logger):
        check(logger, "i,f3", (1.5, 5))

    def test_lob40_shape(self, logger):
        schema = ",".join(["i"] * 20 + ["f5"] * 20)
        args = tuple(range(-10, 10)) + tuple(x + 0.12345 for x in range(20))
        check(logger, schema, args)

    def test_schema_reset_between_calls(self, logger):
        logger.set_csv_schema("i,i")
        assert logger.format_csv(1, 2) == "1,2"
        logger.set_csv_schema("f2")
        assert logger.format_csv(1) == "1.00"

    def test_randomized_against_fstring(self, logger):
        rng = random.Random(20260718)
        schema = "i,f0,f3,f7,f17,i,i,f5"
        specials = [float("nan"), math.inf, -math.inf, -0.0, 5e-324, 1e300]
        for _ in range(300):
            args = []
            for tok in schema.split(","):
                r = rng.random()
                if tok == "i":
                    if r < 0.4:
                        args.append(rng.randint(-2**70, 2**70))
                    elif r < 0.8:
                        args.append(str(rng.randint(-10**9, 10**9)))
                    else:
                        args.append(rng.uniform(-1e6, 1e6))
                else:
                    if r < 0.15:
                        args.append(rng.choice(specials))
                    else:
                        args.append(rng.uniform(-1e12, 1e12))
            check(logger, schema, tuple(args))


# ---------- 에러 경로 (B2에서 cold 분리한 throw들) ----------

class TestErrors:
    def test_log_before_schema_raises(self, logger):
        with pytest.raises(RuntimeError, match="set_csv_schema"):
            logger.log_csv(1)
        with pytest.raises(RuntimeError, match="set_csv_schema"):
            logger.format_csv(1)

    def test_arg_count_mismatch(self, logger):
        logger.set_csv_schema("i,i")
        with pytest.raises(ValueError, match="argument count"):
            logger.format_csv(1)
        with pytest.raises(ValueError, match="argument count"):
            logger.format_csv(1, 2, 3)

    @pytest.mark.parametrize("bad", ["x", "f", "fx", "f18", "f-1", "", ",", "i,,i", "I", "i, i"])
    def test_bad_schema_tokens(self, logger, bad):
        with pytest.raises(ValueError, match="bad token"):
            logger.set_csv_schema(bad)

    def test_failed_schema_keeps_previous(self, logger):
        logger.set_csv_schema("i")
        with pytest.raises(ValueError):
            logger.set_csv_schema("bogus")
        assert logger.format_csv(7) == "7"

    def test_non_numeric_on_f_field(self, logger):
        logger.set_csv_schema("f3")
        with pytest.raises(TypeError):
            logger.format_csv("abc")

    def test_str_raising_in_fallback_propagates(self, logger):
        class Boom:
            def __str__(self):
                raise ZeroDivisionError("boom")
        logger.set_csv_schema("i")
        with pytest.raises(ZeroDivisionError):
            logger.format_csv(Boom())


# ---------- 4096B 라인 버퍼 경계 (bounds check 회귀 가드) ----------

class TestBufferBoundary:
    def test_exact_fit_succeeds(self, logger):
        logger.set_csv_schema("i")
        s = "x" * BUF
        assert logger.format_csv(s) == s

    def test_single_field_overflow(self, logger):
        logger.set_csv_schema("i")
        with pytest.raises(RuntimeError, match="overflow"):
            logger.format_csv("x" * (BUF + 1))

    def test_comma_at_boundary_overflow(self, logger):
        # 첫 필드가 버퍼를 정확히 채우면 콤마 쓰기에서 overflow여야 함 (v2.5.0 수정 경로)
        logger.set_csv_schema("i,i")
        with pytest.raises(RuntimeError, match="overflow"):
            logger.format_csv("x" * BUF, "y")

    def test_nan_fits_exactly_at_end(self, logger):
        logger.set_csv_schema("i,f3")
        got = logger.format_csv("x" * (BUF - 4), float("nan"))
        assert got == "x" * (BUF - 4) + ",nan"

    def test_nan_overflow_at_end(self, logger):
        # 콤마 뒤 남은 공간 2B < 3B("nan") → overflow (v2.5.0 수정 경로)
        logger.set_csv_schema("i,f3")
        with pytest.raises(RuntimeError, match="overflow"):
            logger.format_csv("x" * (BUF - 3), float("nan"))

    def test_float_tochars_overflow(self, logger):
        logger.set_csv_schema("i,f17")
        with pytest.raises(RuntimeError, match="overflow"):
            logger.format_csv("x" * (BUF - 6), 0.1)  # 콤마 뒤 5B < 19B 필요

    def test_cumulative_overflow_many_fields(self, logger):
        n = 40
        logger.set_csv_schema(",".join(["i"] * n))
        with pytest.raises(RuntimeError, match="overflow"):
            logger.format_csv(*(["y" * 200] * n))  # 8000B+ > 4096B


# ---------- log_csv가 실제 파일에 쓰는 내용 ----------

class TestFileOutput:
    def test_payload_roundtrip_with_v_pattern(self, tmp_path):
        path = tmp_path / "csv.log"
        lg = spdlog.FileLogger(name="csv-roundtrip", filename=str(path),
                               multithreaded=False, truncate=True)
        try:
            lg.set_pattern("%v")
            lg.set_csv_schema("i,i,f3")
            rows = [(1, "2", 3.5), (-7, "+8", float("nan")), (2**64, "", -0.0)]
            for r in rows:
                lg.log_csv(*r)
            expected = [py_format("i,i,f3", r) for r in rows]
        finally:
            lg.close()
        assert path.read_text().splitlines() == expected

    def test_csv_time_pattern_prefix(self, tmp_path):
        # 실전 패턴 → 전용 csv_time_formatter로 자동 대체되는 경로
        import re
        path = tmp_path / "csvts.log"
        lg = spdlog.FileLogger(name="csv-ts", filename=str(path),
                               multithreaded=False, truncate=True)
        try:
            lg.set_pattern("%H:%M:%S.%e,%v")
            lg.set_csv_schema("i,f2")
            lg.log_csv(42, 1.5)
            lg.log_csv(43, 2.5)
        finally:
            lg.close()
        lines = path.read_text().splitlines()
        assert [l[13:] for l in lines] == ["42,1.50", "43,2.50"]
        for l in lines:
            assert re.match(r"^\d{2}:\d{2}:\d{2}\.\d{3},", l), l
