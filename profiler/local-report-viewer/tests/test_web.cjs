const {chromium}=require('playwright');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const origin=process.env.TRACY_TEST_URL||'http://127.0.0.1:8140';
const out=process.env.TRACY_TEST_OUTPUT||'.';fs.mkdirSync(out,{recursive:true});
(async()=>{
 const browser=await chromium.launch({channel:'chrome',headless:true,args:['--enable-unsafe-swiftshader']});
 const page=await browser.newPage({viewport:{width:1800,height:1200}});
 const result={started:new Date().toISOString(),checks:[],errors:[],trace_requests:0};
 const timer=setTimeout(()=>browser.close(),240000);
 page.on('pageerror',e=>result.errors.push(String(e)));
 page.on('request',r=>{if(r.url().endsWith('/trace'))result.trace_requests++});
 await page.route('**/*',r=>new URL(r.request().url()).origin===origin?r.continue():r.abort());
 const state=()=>page.evaluate(()=>window.TracyReport.state());
 try{
  await page.goto(origin+'/profiler/?view=selected-observation',{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>window.TracyReport,{},{timeout:15000});
  await page.waitForFunction(()=>window.TracyReport.state().phase==='ready'||window.TracyReport.state().phase==='error',{},{timeout:180000});
  let initial=await state();assert.equal(initial.phase,'ready',JSON.stringify(initial));assert.equal(initial.native.background_done,true);assert.notEqual(initial.location.frame_index,1324);assert.equal(initial.native.navigation.frame_index,initial.location.frame_index);assert.equal(initial.native.navigation.frame_set,initial.location.frame_set);assert.equal(initial.native.navigation.event.name,'Gfx.PresentFrame');assert.equal(initial.native.layout_ready,true);
  result.checks.push({name:'runtime load and random observation location',state:initial});
  await page.locator('#canvas').screenshot({path:path.join(out,'native-observation.png')});
  for(const key of ['navigation-frames','navigation-render','selected-observation']){
   await page.evaluate(id=>window.TracyReport.navigate(id),key);const s=await state();assert.equal(s.phase,'ready');assert.equal(s.native.navigation.begin_ns,s.location.begin_ns);assert.equal(s.native.navigation.end_ns,s.location.end_ns);result.checks.push({name:'navigate '+key,state:s});
  }
  assert.equal(result.trace_requests,1,'same Trace navigation must not download again');
  const rejection=await page.evaluate(()=>{
   const req=structuredClone(window.TracyReport.state().location);req.frame_index=2147483647;
   return JSON.parse(Module.ccall('tracyReportNavigate','string',['string'],[JSON.stringify(req)]));
  });assert.equal(rejection.ok,false);result.checks.push({name:'invalid native frame rejected',response:rejection});
  const wrongRange=await page.evaluate(()=>{const req=structuredClone(window.TracyReport.state().location);req.begin_ns='1';return JSON.parse(Module.ccall('tracyReportNavigate','string',['string'],[JSON.stringify(req)]));});assert.equal(wrongRange.ok,false);result.checks.push({name:'wrong frame range rejected',response:wrongRange});
  const rejected=await page.evaluate(()=>{
   const results=[];
   for(const change of [{trace_sha256:'0'.repeat(64)},{frame_set:'missing-frame-set'},{threads:[{id:'61572',name:'wrong-thread'}]}]){const req={...structuredClone(TracyReport.state().location),...change};results.push(JSON.parse(Module.ccall('tracyReportNavigate','string',['string'],[JSON.stringify(req)])));}
   results.push(JSON.parse(Module.ccall('tracyReportNavigate','string',['string'],['not-json'])));return results;
  });assert.deepEqual(rejected.map(x=>x.error),['trace_identity_mismatch','frame_set_not_found','thread_identity_mismatch','invalid_json']);result.checks.push({name:'invalid identities rejected without moving view',responses:rejected});assert.equal((await state()).native.navigation.frame_index,1798);
  let before=await state();await page.mouse.move(1000,600);await page.mouse.wheel(0,-120);await page.waitForTimeout(1200);let zoom=await state();assert.ok(BigInt(zoom.native.end_ns)-BigInt(zoom.native.begin_ns)<BigInt(before.native.end_ns)-BigInt(before.native.begin_ns));result.checks.push({name:'interactive wheel zoom',state:zoom});
  await page.mouse.move(1000,950);await page.mouse.down({button:'right'});await page.mouse.move(1200,350,{steps:25});await page.mouse.up({button:'right'});await page.waitForTimeout(1000);let drag=await state();assert.notEqual(drag.native.begin_ns,zoom.native.begin_ns);assert.notEqual(drag.native.scroll_y,zoom.native.scroll_y);result.checks.push({name:'pan and other tracks',state:drag});await page.locator('#canvas').screenshot({path:path.join(out,'native-other-tracks.png')});
  await page.evaluate(()=>window.TracyReport.navigate('selected-observation'));
  await page.evaluate(()=>window.TracyReport.selectEvent());let selected=await state();assert.equal(selected.native.zone_selected,true);result.checks.push({name:'native event details',state:selected});await page.locator('#canvas').screenshot({path:path.join(out,'native-event-details.png')});
  await page.waitForTimeout(5000);await page.mouse.move(1100,650,{steps:10});await page.waitForTimeout(500);assert.equal(result.errors.length,0);assert.equal((await state()).phase,'ready');result.passed=true;
 }catch(e){result.passed=false;result.failure=e.stack||String(e);process.exitCode=1;}
 finally{clearTimeout(timer);await browser.close();result.finished=new Date().toISOString();fs.writeFileSync(path.join(out,'web-test.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result));}
})();
