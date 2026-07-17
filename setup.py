import os
import platform

from setuptools import setup
from setuptools.extension import Extension
from setuptools.command.build_ext import build_ext
from distutils.command.install_headers import install_headers

def is_posix():
    return platform.os.name == "posix"

def link_libs():
    libs = []
    if is_posix():
        libs.append("stdc++")
    return libs

def get_extra_compile_args():
    args = ["-std=c++17", "-O3", "-march=native", "-mtune=native", "-fvisibility=hidden",
            # nanobind CMake 기본과 동일: 바인딩 코드에는 스택 프로텍터 불필요
            "-fno-stack-protector",
            # 모든 로거가 _st(+GIL 직렬화)이므로 libc FILE 락 불필요, 패턴에 %t 미사용
            "-DSPDLOG_FWRITE_UNLOCKED", "-DSPDLOG_NO_THREAD_ID"]
    return args

def nanobind_root():
    import nanobind
    return os.path.dirname(nanobind.__file__)

def get_include_dirs():
    nb_root = nanobind_root()
    include_dirs = [
        'spdlog/include/',
        os.path.join(nb_root, 'include'),
        # nb_combined.cpp가 사용하는 동봉 해시맵
        os.path.join(nb_root, 'ext', 'robin_map', 'include'),
    ]

    conda_prefix = os.environ.get('CONDA_PREFIX')
    if conda_prefix is not None:
        include_dirs.append(os.path.join(conda_prefix, "include"))

    return include_dirs


class build_ext_nanobind(build_ext):
    """libnanobind(nb_combined.cpp)만 -Os로 컴파일.

    nanobind의 CMake 래퍼(nanobind_add_module)가 기본으로 하는 크기 최적화를 재현한다.
    디스패치 코드는 작을수록 i-cache에 유리하고, 우리 핫패스(pyspdlog.cpp)는 -O3 유지.
    """
    _strip_for_nb = {"-O3", "-march=native", "-mtune=native"}

    def build_extensions(self):
        original_compile = self.compiler._compile

        def _compile(obj, src, ext, cc_args, extra_postargs, pp_opts):
            postargs = extra_postargs
            if os.path.basename(src) == "nb_combined.cpp":
                # -fno-strict-aliasing: libnanobind 필수 (raw CPython API의 type punning,
                # nanobind CMake가 무조건 적용). NB_COMPACT_ASSERTIONS: CMake -Os 경로와 동일.
                postargs = ([a for a in extra_postargs if a not in self._strip_for_nb]
                            + ["-Os", "-fno-strict-aliasing", "-DNB_COMPACT_ASSERTIONS"])
            return original_compile(obj, src, ext, cc_args, postargs, pp_opts)

        self.compiler._compile = _compile
        try:
            super().build_extensions()
        finally:
            self.compiler._compile = original_compile


def include_dir_files(folder):
    """Find all C++ header files in folder"""
    from os import walk
    files = []
    for (dirpath, _, filenames) in walk(folder):
        for fn in filenames:
            if os.path.splitext(fn)[1] in {'.h', '.hpp'}:
                files.append(os.path.join(dirpath, fn))
    return files

class install_headers_subdir(install_headers):
    """Install headers and keep subfolder structure"""
    def run(self):
        headers = self.distribution.headers or []
        for header in headers:
            submod_dir = os.path.dirname(os.path.relpath(header, 'spdlog/include/spdlog'))
            install_dir = os.path.join(self.install_dir, submod_dir)
            self.mkpath(install_dir)
            (out, _) = self.copy_file(header, install_dir)
            self.outfiles.append(out)

setup(
    name='spdlog_swyang',
    version='2.5.0',
    author='Gergely Bod',
    author_email='bodgergely@hotmail.com',
    description='python wrapper around C++ spdlog logging library (https://github.com/bodgergely/spdlog-python)',
    license='MIT',
    long_description='python wrapper (https://github.com/bodgergely/spdlog-python) around C++ spdlog (http://github.com/gabime/spdlog.git) logging library.',
    python_requires='>=3.10',
    ext_modules=[
        Extension(
            'spdlog_swyang',
            # nanobind는 헤더 온리가 아님 — 런타임(libnanobind)을 정적으로 함께 컴파일
            ['src/pyspdlog.cpp',
             os.path.join(nanobind_root(), 'src', 'nb_combined.cpp')],
            include_dirs=get_include_dirs(),
            libraries=link_libs(),
            extra_compile_args=get_extra_compile_args(),
            language='c++'
        )
    ],
    headers=include_dir_files('spdlog/include/spdlog'),
    cmdclass={'install_headers': install_headers_subdir,
              'build_ext': build_ext_nanobind},
    zip_safe=False,
)
