/* Export registered native views and a standalone Markdown tool-acceptance page. */
const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const args=process.argv.slice(2);function arg(name,fallback){const i=args.indexOf(name);return i<0?fallback:args[i+1];}
const origin=arg('--server','http://127.0.0.1:8140').replace(/\/$/,'');
if(!['127.0.0.1','localhost'].includes(new URL(origin).hostname))throw Error('Only a local viewer server is supported');
const ids=(arg('--views','')||'').split(',').filter(Boolean);if(!ids.length||ids.some(id=>!/^[A-Za-z0-9_-]{1,100}$/.test(id)))throw Error('--views requires registered view IDs');
const out=path.resolve(arg('--output','native-views'));fs.mkdirSync(out,{recursive:true});
const md=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/([\[\]\\])/g,'\\$1').replace(/\r?\n/g,' ');
const hash=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
(async()=>{
 const browser=await chromium.launch({channel:'chrome',headless:true,args:['--enable-unsafe-swiftshader']});
 const page=await browser.newPage({viewport:{width:1800,height:1200},deviceScaleFactor:1});
 const run={version:'0.1.0',started_at:new Date().toISOString(),browser:browser.version(),views:[],errors:[],trace_requests:0};
 const timeout=setTimeout(()=>browser.close(),300000);
 page.on('pageerror',error=>run.errors.push(String(error)));
 page.on('request',r=>{if(r.url().endsWith('/trace'))run.trace_requests++});
 await page.route('**/*',r=>new URL(r.request().url()).origin===origin?r.continue():r.abort());
 try{
  const capabilities=await page.request.get(origin+'/api/capabilities');run.server_capabilities=await capabilities.json();
  const metadata=await page.request.get(origin+'/profiler/viewer-build.json');if(!metadata.ok())throw Error('Missing native bundle identity');run.renderer=await metadata.json();
  for(const [index,id] of ids.entries()){
   const started=new Date().toISOString();
   if(index===0){await page.goto(origin+'/profiler/?view='+id,{waitUntil:'domcontentloaded'});await page.waitForFunction(()=>window.TracyReport?.state().phase==='ready'||window.TracyReport?.state().phase==='error',{},{timeout:180000});}
   else await page.evaluate(id=>window.TracyReport.navigate(id),id);
   const state=await page.evaluate(()=>window.TracyReport.state());
   if(state.phase!=='ready'||!state.native.layout_ready||!state.native.background_done||run.errors.length)throw Error('View not ready: '+JSON.stringify(state));
   if(state.native.navigation.begin_ns!==state.location.begin_ns||state.native.navigation.end_ns!==state.location.end_ns||state.native.capture_sha256!==state.capture.sha256)throw Error('Native location mismatch');
   const image=path.join(out,id+'.png');await page.locator('#canvas').screenshot({path:image});
   const record={view_id:id,started_at:started,location:state.location,trace_sha256:state.capture.sha256,native_receipt:state.native,linear_memory_bytes_observed:state.linear_memory_bytes,image:{path:path.basename(image),sha256:hash(image)},interactive_url:origin+'/open/'+id,ended_at:new Date().toISOString()};
   let detail='';
   if(state.native.navigation.event){await page.evaluate(()=>window.TracyReport.selectEvent());const image2=path.join(out,id+'-details.png');await page.locator('#canvas').screenshot({path:image2});record.detail_image={path:path.basename(image2),sha256:hash(image2)};detail=`\n\n![原生事件详情](${path.basename(image2)})\n`;}
   const response=await page.request.get(origin+'/api/views/'+id);const spec=await response.json();
   const text=`<!-- tracy-local-report-view:${id} -->\n# ${md(spec.title)}\n\n本页为独立工具验收，不修改原性能报告，也不把该观察认定为已确认缺陷。\n\n[在可交互 Profiler 中查看](${record.interactive_url})\n\nFrameSet：**${md(state.native.navigation.frame_set)}**；Query 索引 **${state.native.navigation.frame_index}**；原生显示号 **${state.native.navigation.display_frame_number}**。\n\n原生核对区间：${state.native.navigation.begin_ns}—${state.native.navigation.end_ns} ns。\n\n![Tracy 原生帧视图](${path.basename(image)})${detail}\n\n图片由同一定位记录导出。点击链接进入真实 Profiler，滚轮缩放，右键拖动时间/轨道，左键选择事件；其他轨道没有被删除。需要先运行本地查看服务。\n\n原生详情中的运行状态时间若出现异常值，不作为性能分析依据；本页仅验收导航、原生渲染和交互。\n`;
   fs.writeFileSync(path.join(out,id+'.md'),text);run.views.push(record);
  }
  run.passed=true;
 }catch(error){run.passed=false;run.failure=error.stack||String(error);process.exitCode=1;}
 finally{clearTimeout(timeout);await browser.close();run.ended_at=new Date().toISOString();fs.writeFileSync(path.join(out,'native-view-receipts.json'),JSON.stringify(run,null,2));console.log(JSON.stringify(run));}
})();
