/* Runtime loading and navigation only. Performance conclusions remain in Query evidence. */
let resolveRuntime;
const reportRuntimeReady=new Promise(resolve=>{resolveRuntime=resolve});
let reportPhase='initializing',reportError=null,reportCapture=null,reportBundle=null,reportLocation=null,reportViewId=null,reportNativeStarted=false,reportQueue=Promise.resolve(),reportChannel=null;
function reportFail(error){reportError=String(error?.message||error);reportPhase='error';const box=document.getElementById('error');box.style.display='block';box.textContent='查看器未通过验证：'+reportError;document.getElementById('status').textContent='失败，不能把当前画面作为有效截图';}
function reportNavigationError(error){if(reportNativeStarted&&!reportError&&reportNative()?.background_done){reportPhase='ready';const box=document.getElementById('error');box.style.display='block';box.textContent='定位请求未通过：'+String(error?.message||error);}else reportFail(error);}
var Module={noInitialRun:true,canvas:document.getElementById('canvas'),onRuntimeInitialized:()=>resolveRuntime(),onAbort:reason=>reportFail(reason),print:text=>console.log(text),printErr:text=>console.error(text)};
window.addEventListener('error',e=>reportFail(e.error||e.message));
Module.canvas.addEventListener('webglcontextlost',e=>{e.preventDefault();reportFail('WebGL context lost')});
function reportNative(){return reportNativeStarted?JSON.parse(Module.ccall('tracyReportState','string',[],[])):null;}
async function reportWait(predicate,timeout=180000){const until=performance.now()+timeout;while(performance.now()<until){if(reportError)throw Error(reportError);const value=predicate();if(value)return value;await new Promise(resolve=>setTimeout(resolve,50));}throw Error('native readiness timeout');}
async function reportJson(url){const response=await fetch(url,{cache:'no-store'});if(!response.ok)throw Error('HTTP '+response.status+' '+url);return response.json();}
async function reportNavigate(viewId){
 if(!/^[A-Za-z0-9_-]{1,100}$/.test(viewId))throw Error('invalid view identifier');
 const spec=await reportJson('/api/views/'+viewId);await reportRuntimeReady;
 if(reportCapture&&reportCapture.sha256!==spec.capture.sha256)throw Error('another_capture_requires_separate_viewer');
 if(reportCapture&&reportBundle!==spec.bundle_identity)throw Error('viewer_version_changed_reload_required');
 document.getElementById('title').textContent=spec.title;document.title=spec.title+' · Tracy';
 if(!reportCapture){
  reportPhase='loading';document.getElementById('status').textContent='正在加载本地 Trace 并校验身份…';
  const response=await fetch(spec.capture.url,{cache:'no-store'});if(!response.ok)throw Error('Trace load HTTP '+response.status);
  const bytes=await response.arrayBuffer();if(bytes.byteLength!==spec.capture.bytes)throw Error('trace_size_mismatch');
  const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),x=>x.toString(16).padStart(2,'0')).join('');
  if(digest!==spec.capture.sha256)throw Error('trace_identity_mismatch');
  Module.FS.createDataFile('/','report.tracy',new Uint8Array(bytes),true,false,true);
  if(Module.ccall('tracyReportBindCapture','number',['string'],[digest])!==1)throw Error('capture_binding_rejected');
  reportCapture=spec.capture;reportBundle=spec.bundle_identity;reportNativeStarted=true;reportPhase='processing';
  Module.callMain(['/report.tracy']);
  await reportWait(()=>{const state=reportNative();if(state.load_error)throw Error('native_load_error '+state.load_error);return state.background_done&&state.has_data});
  window.name='tracy-report-'+digest+'-'+(reportBundle||'unversioned');reportChannel=new BroadcastChannel(window.name);
  reportChannel.onmessage=e=>{if(e.data?.type==='navigate'&&typeof e.data.view==='string'){reportChannel.postMessage({type:'accepted',view:e.data.view});window.focus();window.TracyReport.navigate(e.data.view).then(()=>{reportChannel.postMessage({type:'completed',view:e.data.view});window.focus();}).catch(error=>{reportChannel.postMessage({type:'failed',view:e.data.view,error:String(error)});reportNavigationError(error)});}};
 }
 reportPhase='locating';const targetLocation={...spec.location,trace_sha256:spec.capture.sha256};
 const result=JSON.parse(Module.ccall('tracyReportNavigate','string',['string'],[JSON.stringify(targetLocation)]));
 if(!result.ok)throw Error(result.error);
 await reportWait(()=>{const state=reportNative();return state.navigation?.revision===result.revision&&state.layout_ready},15000);
 const actual=reportNative();if(actual.navigation.begin_ns!==targetLocation.begin_ns||actual.navigation.end_ns!==targetLocation.end_ns||actual.capture_sha256!==spec.capture.sha256)throw Error('resolved_identity_mismatch');
 reportLocation=targetLocation;reportViewId=viewId;reportPhase='ready';document.getElementById('error').style.display='none';history.replaceState(null,'','?view='+encodeURIComponent(viewId));
 document.getElementById('status').textContent=`${result.frame_set} · Query 索引 ${result.frame_index} · 原生显示号 ${result.display_frame_number} · 已就绪，可探索其他轨道`;
 document.getElementById('reset').disabled=false;document.getElementById('details').disabled=!result.event;
 return result;
}
window.TracyReport={
 state:()=>({phase:reportPhase,error:reportError,view_id:reportViewId,capture:reportCapture,bundle_identity:reportBundle,location:reportLocation,native:reportError?null:reportNative(),linear_memory_bytes:typeof HEAPU8==='undefined'?null:HEAPU8.buffer.byteLength}),
 navigate:viewId=>{const task=reportQueue.then(()=>reportNavigate(viewId)).catch(error=>{reportNavigationError(error);throw error});reportQueue=task.catch(()=>{});return task;},
 selectEvent:async()=>{if(reportPhase!=='ready'||Module.ccall('tracyReportSelectEvent','number',[],[])!==1)throw Error('no_verified_event');await reportWait(()=>reportNative().zone_selected&&reportNative().layout_ready,10000);return reportNative();}
};
document.getElementById('reset').onclick=()=>window.TracyReport.navigate(reportViewId).catch(reportNavigationError);
document.getElementById('details').onclick=()=>window.TracyReport.selectEvent().catch(reportNavigationError);
window.addEventListener('message',e=>{if(e.origin===location.origin&&e.data?.type==='tracy-report-navigate')window.TracyReport.navigate(e.data.view).catch(reportNavigationError)});
const reportInitialView=new URLSearchParams(location.search).get('view');
if(reportInitialView)window.TracyReport.navigate(reportInitialView).catch(reportFail);else reportFail('missing view identifier; open a report link');
setInterval(()=>{if(reportPhase==='ready'&&reportCapture){const state=reportNative();if(!state.has_data||state.capture_sha256!==reportCapture.sha256)reportFail('录制已关闭或从原生菜单更换，请重新打开报告链接。');}},1000);
