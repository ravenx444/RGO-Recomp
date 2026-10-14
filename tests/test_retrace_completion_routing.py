"""Compile the actual completion selector with synthetic diagnostic endpoints."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RetraceCompletionRoutingTests(unittest.TestCase):
    def test_original_os_default_and_legacy_completion_contracts(self):
        cmake = shutil.which('cmake')
        if not cmake and os.name == 'nt':
            sys.path.insert(0, str(ROOT/'scripts'))
            from build_from_rom import find_cmake
            try: cmake = find_cmake()
            except ValueError: pass
        if not cmake: self.skipTest('CMake compiler harness unavailable')
        source = (ROOT/'src/boot/native_boot.cpp').read_text()
        match = re.search(r'(\[\[noreturn\]\] void retrace_gate\(State &state\) \{.*?^\})', source, re.S|re.M)
        self.assertIsNotNone(match)
        completion = re.search(r'(bool extract_retrace_completion\(.*?^\})', source, re.S|re.M)
        self.assertIsNotNone(completion)
        preamble = '''
#include <cstdlib>
#include <string>
struct State { unsigned retrace_target, poll_target; bool guest_os_probe; };
enum class Endpoint { Stable, Probe, Closed };
[[noreturn]] void stable_vi_gate(State&) { throw Endpoint::Stable; }
[[noreturn]] void probe_target_gate(State&) { throw Endpoint::Probe; }
[[noreturn]] void fail_closed_dispatch(State&,const char*,const char*,unsigned) { throw Endpoint::Closed; }
bool extract_canonical_success(const std::string& s,std::string& line) {
    if (s != "legacy-three") return false; line=s; return true;
}
bool extract_canonical_probe(const std::string& s,std::string& line) {
    if (s != "legacy-long") return false; line=s; return true;
}
bool extract_original_os_probe(const std::string& s,unsigned target,std::string& line) {
    if (s != "original-"+std::to_string(target)) return false; line=s; return true;
}
'''
        checks = '''
Endpoint route(State state) {
    try { retrace_gate(state); } catch (Endpoint endpoint) { return endpoint; }
    std::abort();
}
int main() {
    std::string line;
    if (!extract_retrace_completion("original-3",3,line)) return 8;
    if (!extract_retrace_completion("legacy-three",3,line)) return 9;
    if (extract_retrace_completion("legacy-long",3,line)) return 10;
    if (extract_retrace_completion("original-120",3,line)) return 11;
    if (!extract_retrace_completion("original-120",120,line)) return 12;
    if (!extract_retrace_completion("legacy-long",120,line)) return 13;
    if (extract_retrace_completion("legacy-three",120,line)) return 14;
    if (extract_retrace_completion("unrecognized",3,line)) return 15;
    if (extract_retrace_completion("unrecognized",120,line)) return 16;
    if (route({3,0,false}) != Endpoint::Stable) return 1;
    if (route({3,1,false}) != Endpoint::Closed) return 2;
    if (route({120,1,true}) != Endpoint::Closed) return 3;
#if defined(JFG_PHASE8_LIVE_RUNTIME)
    if (route({3,0,true}) != Endpoint::Probe) return 4;
    if (route({120,0,true}) != Endpoint::Probe) return 5;
    if (route({120,0,false}) != Endpoint::Probe) return 6;
#else
    if (route({120,0,false}) != Endpoint::Closed) return 7;
#endif
    return 0;
}
'''
        scratch = ROOT/'build/test-tmp';scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            root = Path(directory)
            (root/'routing.cpp').write_text(preamble+match.group(1)+completion.group(1)+checks)
            (root/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.24)
project(retrace_routing LANGUAGES CXX)
add_executable(legacy routing.cpp)
add_executable(original routing.cpp)
target_compile_features(legacy PRIVATE cxx_std_20)
target_compile_features(original PRIVATE cxx_std_20)
target_compile_definitions(original PRIVATE JFG_PHASE8_LIVE_RUNTIME=1)
''')
            configure=[cmake,'-S',str(root),'-B',str(root/'n')]
            if os.name=='nt': configure+=['-G','Visual Studio 17 2022','-A','x64']
            for command in (configure,[cmake,'--build',str(root/'n'),'--config','Release','--parallel','2']):
                result=subprocess.run(command,capture_output=True,text=True,timeout=120)
                self.assertEqual(result.returncode,0,result.stdout[-3000:]+result.stderr[-3000:])
            for name in ('original','legacy'):
                exe=root/'n'/('Release/'+name+'.exe' if os.name=='nt' else name)
                result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,name+': '+result.stdout+result.stderr)


if __name__=='__main__': unittest.main()
