"""Build the runtime-loaded Wasm64 viewer without embedding any capture."""
import argparse,datetime,hashlib,json,os,shutil,subprocess,sys,time
from pathlib import Path
def main():
 p=argparse.ArgumentParser();p.add_argument('--emsdk',type=Path,required=True);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--host-embed',type=Path,required=True);p.add_argument('--cmake',default=shutil.which('cmake'));p.add_argument('--ninja',default=shutil.which('ninja'));p.add_argument('--dependency-cache',type=Path);p.add_argument('--imgui-source',type=Path);p.add_argument('--ppqsort-source',type=Path);p.add_argument('--git-tools',type=Path);p.add_argument('--jobs',type=int,default=8)
 a=p.parse_args();root=Path(__file__).resolve().parents[2];build=a.build_dir.resolve();build.mkdir(parents=True,exist_ok=True);sdk=a.emsdk.resolve()
 if not a.cmake or not a.ninja:p.error('CMake and Ninja must be available')
 env=dict(os.environ);env['EM_CONFIG']=str(sdk/'.emscripten');env['EMSDK']=str(sdk);env['EMSDK_PYTHON']=sys.executable
 env['PATH']=os.pathsep.join([str(Path(a.cmake).parent),str(Path(a.ninja).parent),str(sdk/'upstream/emscripten')]+([str(a.git_tools)] if a.git_tools else [])+[env.get('PATH','')])
 revision=subprocess.check_output(['git','-c','safe.directory='+root.as_posix(),'-C',str(root),'rev-parse','HEAD'],text=True).strip()
 args=[a.cmake,'-S',str(root/'profiler'),'-B',str(build),'-G','Ninja','-DCMAKE_BUILD_TYPE=MinSizeRel','-DBUILD_SHARED_LIBS=OFF','-DZSTD_BUILD_SHARED=OFF','-DCMAKE_C_FLAGS=-sMEMORY64=1','-DCMAKE_CXX_FLAGS=-sMEMORY64=1','-DTRACY_LOCAL_REPORT_VIEWER=ON','-DTRACY_HOST_EMBED='+str(a.host_embed.resolve()),'-DTRACY_REPORT_REVISION='+revision,'-DCMAKE_TOOLCHAIN_FILE='+str(sdk/'upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake'),'-DCMAKE_MAKE_PROGRAM='+str(a.ninja)]
 if a.dependency_cache:args+=['-DCPM_SOURCE_CACHE='+str(a.dependency_cache.resolve())]
 if bool(a.imgui_source)!=bool(a.ppqsort_source):p.error('Provide both prepatched sources or neither')
 if a.imgui_source:args+=['-DTRACY_REPORT_PREPATCHED_DEPS=ON','-DCPM_ImGui_SOURCE='+str(a.imgui_source.resolve()),'-DCPM_PPQSort_SOURCE='+str(a.ppqsort_source.resolve())]
 args=[value.replace('\\','/') if value.startswith('-D') else value for value in args]
 started=datetime.datetime.now(datetime.timezone.utc).isoformat();t=time.monotonic()
 (build/'configure-command.json').write_text(json.dumps(args,indent=2),encoding='utf-8')
 with (build/'configure.log').open('wb') as log:result=subprocess.run(args,env=env,stdout=log,stderr=subprocess.STDOUT)
 if result.returncode:print((build/'configure.log').read_text(errors='replace')[-6000:]);return result.returncode
 with (build/'build.log').open('wb') as log:result=subprocess.run([a.cmake,'--build',str(build),'--target','tracy-profiler','--parallel',str(a.jobs)],env=env,stdout=log,stderr=subprocess.STDOUT)
 record={'version':'0.1.0','revision':revision,'working_tree_changes_included':True,'started_at':started,'ended_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'elapsed_seconds':time.monotonic()-t,'exit_code':result.returncode,'capture_embedded':False,'memory64':True}
 if not result.returncode:record['artifacts']={name:hashlib.sha256((build/name).read_bytes()).hexdigest() for name in ['index.html','report-viewer.js','tracy-profiler.js','tracy-profiler.wasm']}
 (build/'viewer-build.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record));print((build/'build.log').read_text(errors='replace')[-4000:]);return result.returncode
if __name__=='__main__':raise SystemExit(main())
