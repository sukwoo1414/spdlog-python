# spdlog-python 성능 개선 후보 (연구 노트, 미적용)

작성일: 2026-07-17
상태: **A3 적용 완료(2026-07-17, v2.4.0). 나머지는 후보.** 적용 시 항목별로 벤치 전/후를 남길 것.

배경: market_collector의 실시간 lob/tick 데이터 싱크가 이 모듈의 `FileLogger.info()` /
`log_csv()`를 레코드마다 호출한다. 타겟 시스템에서 instruction cache(i-cache, CPU가
명령어를 읽는 L1 캐시) 미스가 많다는 점을 전제로, (1) 호출 고정비, (2) 핫패스 코드
크기, (3) 분기 배치 관점에서 후보를 뽑았다.

기준 벤치(2026-07 idle 머신, 5회, ±0.5%):
- lob 40필드: `.info(",".join(...))` 5,628 ns/건 vs `.log_csv(*args)` 4,343 ns/건 (1.30x)
- tick 5필드: `.info(f-string)` 2,896 ns/건 vs `.log_csv(*args)` 3,015 ns/건 (0.96x)

---

## 현재 상태 계측 (근거)

측정 대상: `/home/swyang/venv3.14/.../spdlog_swyang.cpython-314-x86_64-linux-gnu.so`
(pybind11 3.0.4, spdlog v1.17.0, `-O3 -march=native`, `SPDLOG_FWRITE_UNLOCKED`,
`SPDLOG_NO_THREAD_ID`)

| 항목 | 측정값 | 의미 |
|---|---|---|
| `.so` 크기 | 18.4MB (`.text`=777KB, 디버그 섹션 ~17MB) | 코드도 크고 디버그 정보가 대부분 |
| `pybind11::cpp_function::dispatcher` | **11.6KB 단일 함수** | 모든 `.info()`/`.log_csv()` 호출이 매번 통과하는 제네릭 디스패처. L1 i-cache(통상 32KB)의 1/3 |
| `csv_serialize` | 핫부분 1,526B + cold 1,040B | str() 폴백·throw가 핫루프 안에 섞여 있음 |
| `csv_time_formatter::format` | 2,104B | 하는 일(13B + payload append) 대비 큼 |
| pybind11 3.0.4 디스패치 | METH_FASTCALL 사용 | 단 `py::args`는 매 호출 **튜플 재조립** 발생 |

핵심 관찰:
1. 호출당 고정비의 대부분이 pybind11 제네릭 디스패처(=i-cache 오염원).
2. **LS증권은 숫자를 문자열로 보내므로**, `csv_serialize`의 "i" 필드는 실데이터에서
   매번 `PyLong_CheckExact` 실패 → `PyObject_Str` 폴백을 탄다. 즉 폴백이 사실상 본선.

---

## A. 파이썬 경계 (가장 큰 후보)

### A1. vectorcall 전용 writer 객체  ★ 최우선
`tp_vectorcall`(CPython의 저오버헤드 호출 프로토콜: 인자를 튜플 없이 C 스택 배열로
전달)을 가진 초소형 커스텀 타입을 신설. 내부에 `spdlog::logger` 포인터 + CSV 스키마를
보유하고 `logger.csv_writer()`로 발급. 호출 시:
- pybind11 디스패처(11.6KB)를 완전히 우회 — 핫패스가 1KB 미만 전용 경로가 됨.
- `w(*fields)` 형태여도 CPython이 튜플의 item 배열을 그대로 넘겨 인자 재조립 없음.

효과: 호출당 수백 ns + i-cache 압력 대폭 감소. tick(5필드)이 f-string을 역전할
가능성이 가장 높은 후보. / 난이도: 중 / 리스크: 낮음(기존 API는 그대로 두고 추가).

### A3. nanobind 전면 이관 — ✅ 적용됨 (2026-07-17, v2.4.0)
nanobind는 vectorcall 네이티브이고 바인딩 생성 코드가 pybind11보다 수 배 작아
i-cache 목적에 정확히 부합. 실제 이관해보니 트램폴린·특수 캐스터를 안 쓰는
코드라 "전면 공사"가 아니었음(바인딩 치환 + 빌드 통합).

**적용 방식**: setup.py 유지(CMake 미사용). nanobind 2.13.0을 빌드타임 의존성으로,
`nb_combined.cpp`(libnanobind)를 확장 모듈에 정적 컴파일. libnanobind만 `-Os`
(nanobind CMake 기본 재현, 커스텀 build_ext), 핫패스(pyspdlog.cpp)는 `-O3
-march=native` 유지, `-fno-stack-protector` 추가. 런타임 파이썬 의존성 없음.

**전/후 (2026-07-17 idle, 5회 평균, 200k call/run, tests/bench_binding_hotpath.py)**:

| 케이스 | pybind11 (2.3.0) | nanobind (2.4.0) | Δ |
|---|---|---|---|
| lob40 log_csv | 3,093 ns | 2,897 ns | −6.4% |
| lob40 log_csv str폴백 | 2,528 ns | 2,269 ns | −10.3% |
| lob40 info(join) | 7,610 ns | 7,389 ns | −2.9% |
| tick5 log_csv | 1,814 ns | 1,714 ns | −5.5% |
| tick5 info(f-string) | 2,212 ns | 2,063 ns | −6.7% |
| info(100B 고정) | 1,523 ns | 1,447 ns | −5.0% |

**코드 크기**: `.so` 18.4MB → 12.6MB, `.text` 777KB → 515KB(−34%),
11.6KB 단일 제네릭 디스패처 → nanobind vectorcall 디스패처(최대 ~2KB).
**검증**: pytest 4/4, `format_csv` vs f-string 바이트 동일성 15케이스 통과
(int64 경계/초과, ±0.0, nan/±inf, str/빈문자열/bool 폴백 포함).
**주의**: idle 마이크로벤치는 i-cache 미스가 적어 효과를 과소평가할 수 있음 —
market_collector 실부하에서 `perf stat` 재측정 가치 있음. A1(전용 vectorcall
writer)은 여전히 이 위에 얹을 수 있는 별개 카드.

---

## B. csv_serialize 마이크로 최적화

### B1. 타입 체크 순서를 실데이터에 맞춤  ★
`PyUnicode_CheckExact`를 **먼저** 검사해 str이면 `PyUnicode_AsUTF8AndSize` 직행.
현재의 `PyObject_Str` 호출 + incref/decref가 사라진다(str에 대해 PyObject_Str은
같은 객체를 incref해 돌려줄 뿐). 출력 바이트 동일성 그대로 유지.
순서: unicode → long → 제네릭 str() 폴백. 원하면 "s" 스키마 토큰 신설도 가능.
효과: 필드당 수십 ns × lob 40필드 / 난이도: 하.

### B2. cold 경로 분리 + 분기 힌트 — ✅ 적용됨 (2026-07-17, v2.5.0)
- throw 사이트들을 `[[noreturn]] [[gnu::cold]]` noinline 헬퍼 함수로 추출
  (인라인 throw는 예외 객체 생성 코드를 핫루프에 심는다).
- 에러 조건들에 `__builtin_expect(..., 0)` (unlikely) 부여.
- str() 폴백은 **분리하지 않음** — B1 미적용 상태에선 실데이터(str 필드)의 본선.
- 부수 수정: 콤마·"nan" 쓰기에 bounds check 추가(기존 잠재 스택 오버플로우, 코드 리뷰 지적).

**결과 (v2.4.0 → v2.5.0)**: idle 벤치는 전 케이스 ±1% 노이즈 내(개선/회귀 없음,
lob40 log_csv 2,922→2,901ns 등; spdlog_vs_logging도 동일). 구조 지표는 개선 —
throw 4종이 핫패스에서 ~75KB 떨어진 .text 초입 unlikely 영역으로 분리
(0x144xx vs csv_serialize 0x26f80), 핫 본체 1,526→1,469B(bounds check 추가분 상쇄),
.text 514.8→513.4KB. **i-cache 효과는 idle에서 측정 불가 — market_collector
실부하 perf stat으로 최종 판정 필요.**

### B3. `csv_time_formatter` 프리픽스 재계산 cold 분리
초가 바뀔 때만 타는 "HH:MM:SS." 재계산(localtime 포함)을 unlikely + noinline
멤버함수로 분리. format() 2.1KB → 핫부분 수백 B 목표. / 난이도: 하.

---

## C. spdlog 코어/싱크 경로

### C2. `-DSPDLOG_NO_ATOMIC_LEVELS` — ✅ 적용됨 (2026-07-17, v2.6.0)
레벨 체크의 atomic load를 plain load로. 모든 로거가 `_st` + GIL 직렬화라 안전
(레벨 변경은 셋업 시점뿐).

**결과 (v2.5.0 → v2.6.0)**: idle 벤치 전 케이스 ±2% 노이즈 내 — x86에서 atomic
load는 어차피 plain mov라 예상대로 측정 가능한 차이 없음. 컴파일러 재배치 자유도
확보 차원의 공짜 플래그로 유지. pytest 4/4·바이트 동일성 15케이스 통과.

(C1 직접쓰기 싱크는 삭제 — flush_on(INFO)은 market_collector의 일시적 설정이라
spdlog-python 차원의 대응 대상이 아님. C3 COARSE 시계는 비추천이라 삭제.)

---

## D. 빌드/링크 (i-cache·코드 크기)

### D1. PGO (profile-guided optimization)  ★
`-fprofile-generate`로 빌드 → 기존 벤치 실행 → `-fprofile-use`로 재빌드.
GCC가 실측 기반으로 hot/cold 블록 분할(`.text.unlikely`), 블록 재배치, 인라인 판단을
수행 — i-cache 미스에 가장 직접적인 정공법. B2/B3 수작업의 상당 부분을 자동화.
난이도: 빌드 파이프라인 2단계화(setup.py에 프로파일 빌드 경로 추가).

### D2. 공짜 플래그 묶음 — ✅ 적용됨 (2026-07-17, v2.7.0)
- `-fno-plt`: CPython API 호출마다 타는 PLT(Procedure Linkage Table, 공유라이브러리
  외부 함수 호출용 간접점프 스텁) 제거.
- `-fno-semantic-interposition`: -fPIC로 막히는 DSO 내부 직접호출/인라인 허용.
- `-fvisibility-inlines-hidden`: 인라인 함수 심볼 은닉(이미 `-fvisibility=hidden`은 적용됨).

**결과 (v2.6.0 → v2.7.0)**: idle 벤치 전 케이스 ±2% 노이즈 내(lob40 log_csv
2,901→2,906ns, spdlog_vs_logging sync 10B 1.76→1.75µs 등) — PLT 스텁 제거분(호출당
~1ns대)은 idle에서 노이즈에 묻힘. 회귀 없음, 간접점프 제거는 BTB/i-cache 위생
차원에서 유지. pytest 4/4·바이트 동일성 15케이스 통과.

### D3. 사용 안 하는 바인딩 다이어트
tcp/syslog/daily/rotating/dup_filter/dist/console 싱크류, `set_error_handler`
(pybind11 functional.h 캐스터 유발), `sinks()`(vector<Sink> 캐스터, stl.h) 등을
`#ifdef SPDLOG_PY_MINIMAL`류로 제외. `.text` 777KB의 상당분이 이런 템플릿 인스턴스.
import 속도·i-TLB(instruction TLB) 압력 개선. 단, 정상 상태 핫루프가 만지는 코드를
직접 줄이는 건 아니라 효과는 간접적. / 난이도: 하.

### D4. -O3 재고
-O3는 콜드 코드까지 비대화한다. 전역 -O2 + 핫 함수만 `[[gnu::hot]]`(또는 PGO에 위임)
조합을 벤치로 비교할 가치. / 난이도: 하.

### D5. strip / -g 제거
18.4MB 중 ~17MB가 디버그 섹션. `strip --strip-unneeded` 또는 릴리스 빌드에서 -g 제거.
런타임 i-cache와 무관하지만 배포·로드 위생. / 공짜.

### D6. (선택) 링커·포스트링크
- lld의 `-Wl,--icf=all`(Identical Code Folding, 동일 기계어 함수 접기 — nm에 중복
  심볼 존재 확인됨) + `-ffunction-sections -Wl,--gc-sections`.
- llvm-BOLT(포스트링크 바이너리 레이아웃 최적화): PGO보다 더 공격적이나 툴링 부담.

---

## 추천 우선순위

1. **A1 (vectorcall writer) + B1 (unicode 우선 체크)** — 호출 고정비와 실데이터
   핫패스를 동시에 타격. 가장 확실한 체감.
2. **B2/B3 (cold 분리·분기 힌트)** 또는 **D1 (PGO)** — i-cache 요구에 직접 대응.
   PGO를 쓰면 B2/B3의 상당 부분이 자동화됨.
3. **D5 (strip)** — 남은 공짜 항목. (D2·C2는 적용 완료)
4. D6(BOLT)는 확장 카드. (A3·B2는 적용 완료 — 각 섹션 참고)

## 검증 방법

- 기존 ns/건 벤치(lob 40필드 / tick 5필드, idle 머신 5회)를 항목별 전/후로 실행.
- `perf stat -e L1-icache-load-misses,iTLB-load-misses,branch-misses,instructions,cycles`
  전/후 비교 (i-cache 가설의 직접 검증).
- log_csv 계열 변경 시 출력 바이트 동일성 검증: `format_csv()`로 f-string 결과와
  바이트 비교 (str/int/float/'+','-'/빈문자열/int64 초과 정수/NaN 케이스 포함).
- market_collector 쪽은 기존 목 서버 E2E(postprocess parquet까지)로 회귀 확인.
