function traceDisplayLayout(width,height,dpr,available,mode){
    dpr=Number.isFinite(dpr)&&dpr>0?dpr:1;
    const factor=mode==='double'?2:1;
    const cssWidth=mode==='fit'?Math.max(1,Math.floor(available*dpr))/dpr:width*factor/dpr;
    return {width:cssWidth,height:cssWidth*height/width,physicalScale:cssWidth*dpr/width,smoothing:mode==='fit'};
}
function tracePoint(clientX,clientY,rect,width,height){return{x:(clientX-rect.left)*width/rect.width,y:(clientY-rect.top)*height/rect.height};}
const canvas=document.querySelector('#timeline'),ctx=canvas.getContext('2d');
const surface=document.querySelector('#surface'),displayMode=document.querySelector('#display-mode');
let displayKey='';
try{const saved=localStorage.getItem('tracy.display-mode.v1');if(['native','double','fit'].includes(saved))displayMode.value=saved;}catch{}
function applyDisplay(){
    const dpr=window.devicePixelRatio||1,mode=displayMode.value;
    const key=[canvas.width,canvas.height,dpr,surface.clientWidth,mode].join(':');if(key===displayKey)return;displayKey=key;
    const layout=traceDisplayLayout(canvas.width,canvas.height,dpr,surface.clientWidth,mode);
    canvas.style.width=layout.width+'px';canvas.style.height=layout.height+'px';
    canvas.style.imageRendering=layout.smoothing?'auto':'pixelated';ctx.imageSmoothingEnabled=false;
    canvas.dataset.physicalScale=String(layout.physicalScale);canvas.dataset.displayMode=mode;
    document.querySelector('#display-info').textContent=mode==='fit'
        ?'原图 '+canvas.width+'×'+canvas.height+' · 适应比例 '+(layout.physicalScale*100).toFixed(1)+'%（小字可能变糊）'
        :'原图 '+canvas.width+'×'+canvas.height+' · '+(mode==='double'?'整数2倍，边缘保留像素感':'屏幕像素1:1，不拉伸')+' · 可滚动查看';
}
displayMode.addEventListener('change',()=>{try{localStorage.setItem('tracy.display-mode.v1',displayMode.value);}catch{}displayKey='';applyDisplay();});
new ResizeObserver(()=>applyDisplay()).observe(surface);
window.addEventListener('resize',()=>applyDisplay());
document.addEventListener('fullscreenchange',()=>{displayKey='';applyDisplay();});
function watchPixelRatio(){
    const query=matchMedia('(resolution: '+(window.devicePixelRatio||1)+'dppx)');
    query.addEventListener('change',()=>{displayKey='';applyDisplay();watchPixelRatio();},{once:true});
}
watchPixelRatio();
let nav=null,requestGeneration=0,lastFrame='0',ready=false,drag=null,pending=[],sending=false;
let pumping=false,frameAbort=null,paintedNavigation=null,paintCount=0,paintAt=performance.now(),lastPaint=0;
const sentTimes=new Map(),latencies=[];
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function info(text){document.querySelector('#status').textContent=text;}
function showError(text){ready=false;pending=[];info(text);if(!paintedNavigation)document.querySelector('#wait').textContent=text;}
async function post(path,body){
    const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Tracy-Token':token},body:JSON.stringify(body)});
    const value=await response.json();if(!response.ok)throw Error(value.error);return value;
}
function paint(bitmap,state){
    if(canvas.width!==bitmap.width||canvas.height!==bitmap.height){canvas.width=bitmap.width;canvas.height=bitmap.height;}
    ctx.imageSmoothingEnabled=false;applyDisplay();
    ctx.drawImage(bitmap,0,0);document.querySelector('#wait').style.display='none';paintedNavigation=state.navigation;
    const now=performance.now();if(now-lastPaint>250){paintAt=now;paintCount=0;}lastPaint=now;paintCount++;
    for(const [ack,at] of sentTimes)if(BigInt(state.sequence)>=BigInt(ack)){latencies.push(now-at);sentTimes.delete(ack);}
    if(latencies.length>120)latencies.splice(0,latencies.length-120);
    canvas.dataset.responseMs=JSON.stringify(latencies.map(v=>Math.round(v)));canvas.dataset.frames=state.frame;
    canvas.dataset.navigation=state.navigation;
    if(now-paintAt>500){canvas.dataset.fps=(paintCount*1000/(now-paintAt)).toFixed(0);paintAt=now;paintCount=0;}
    info((state.busy?'读取可见区间…':'已就绪')+' · 当前显示 '+(Number(state.begin_ns)/1e9).toFixed(6)+'–'+(Number(state.end_ns)/1e9).toFixed(6)+' s · '+(canvas.dataset.fps||'—')+' 帧/秒');
    document.querySelector('#receipt').textContent=JSON.stringify(state,null,2);
}
async function frames(){
    if(pumping)return;pumping=true;let failures=0;
    try{
        while(true){
            if(document.hidden||!nav){await delay(150);continue;}
            const requested=nav;frameAbort=new AbortController();const timer=setTimeout(()=>frameAbort?.abort(),5000);
            try{
                const response=await fetch('/api/frame?view='+id+'&navigation='+requested+'&after='+lastFrame,{signal:frameAbort.signal});
                if(response.status===204){failures=0;continue;}
                if(response.status===409){if(nav===requested){showError('另一个页面已切换视图；点击“回到报告位置”继续。');nav=null;}continue;}
                if(!response.ok)throw Error('画面通道返回 '+response.status);
                const state=JSON.parse(response.headers.get('X-Tracy-State'));
                if(state.navigation!==nav){await response.body.cancel();continue;}
                const bitmap=await createImageBitmap(await response.blob());
                if(state.navigation===nav){lastFrame=state.frame;paint(bitmap,state);}bitmap.close();failures=0;
            }catch(error){
                if(requested!==nav)continue;
                if(++failures>=3){showError('实时画面连接失败。请确认本地服务仍在运行，再点击“回到报告位置”。');nav=null;}
                else await delay(250);
            }finally{clearTimeout(timer);frameAbort=null;}
        }
    }finally{pumping=false;}
}
function enqueue(events){
    for(const event of events){const tail=pending[pending.length-1];
        if(event.type==='move'&&tail?.type==='move')pending[pending.length-1]=event;
        else if(event.type==='wheel'&&tail?.type==='wheel')pending[pending.length-1]={...event,delta:Math.max(-4,Math.min(4,event.delta+tail.delta))};
        else pending.push(event);
    }requestAnimationFrame(flush);
}
async function flush(){
    if(sending||!pending.length||!ready)return;sending=true;
    const events=pending.splice(0,64),navigation=nav;
    try{const at=performance.now();const receipt=await post('/api/input/'+id,{events,navigation});sentTimes.set(receipt.sequence,at);}
    catch(error){if(nav===navigation)showError(String(error));}
    finally{sending=false;if(pending.length)requestAnimationFrame(flush);}
}
function pan(dx,dy){
    const x=canvas.width*.5,y=canvas.height*.7;
    enqueue([{type:'move',x,y},{type:'button',x,y,button:1,down:true},{type:'move',x:x+dx,y:y+dy},{type:'button',x:x+dx,y:y+dy,button:1,down:false}]);
}
function lanes(direction){if(ready)pan(0,-direction*canvas.height*.4);}
async function go(action){
    if(!ready&&['zoom_in','zoom_out','pan_left','pan_right'].includes(action)){info('请等待实时画面就绪后再操作。');return;}
    if(ready&&['zoom_in','zoom_out','pan_left','pan_right'].includes(action)){
        if(action.startsWith('zoom'))enqueue([{type:'wheel',x:canvas.width*.5,y:canvas.height*.7,delta:action==='zoom_in'?1:-1}]);
        else pan((action==='pan_left'?1:-1)*canvas.width/12,0);return;
    }
    const generation=++requestGeneration;ready=false;pending=[];nav=null;frameAbort?.abort();lastFrame='0';paintedNavigation=null;
    try{
        info('连接查看器…');const request=await post('/api/open/'+id,{action});if(generation!==requestGeneration)return;
        nav=request.sequence;frames();const started=performance.now();let readyAt=0;
        while(performance.now()-started<180000){
            if(generation!==requestGeneration)return;
            const state=await(await fetch('/api/status')).json();
            if(state.sequence!==request.sequence){showError('另一个页面已切换视图；点击“回到报告位置”继续。');return;}
            if(state.phase==='failed'){showError(state.error||'原生查看器启动失败');return;}
            if(state.frame_channel_error){showError(state.frame_channel_error);return;}
            if(state.phase==='ready'){
                if(!readyAt)readyAt=performance.now();
                if(paintedNavigation===request.sequence){ready=true;enqueue([{type:'refresh'}]);return;}
                if(performance.now()-readyAt>5000){showError('帧已定位，但实时画面未到达。请点击“回到报告位置”重连，并检查画面通道。');return;}
                info('帧已定位，等待实时画面…');
            }else if(!paintedNavigation)info('正在加载原生查看器 / 校验捕获文件（'+Math.floor((performance.now()-started)/1000)+'秒）…');
            document.querySelector('#receipt').textContent=JSON.stringify(state,null,2);await delay(150);
        }
        showError('加载超时。请检查本地查看器进程或重新运行启动脚本。');
    }catch(error){if(generation===requestGeneration)showError('连接失败：'+String(error));}
}
function point(event){return tracePoint(event.clientX,event.clientY,canvas.getBoundingClientRect(),canvas.width,canvas.height);}
canvas.addEventListener('contextmenu',event=>event.preventDefault());
canvas.addEventListener('wheel',event=>{event.preventDefault();if(ready)enqueue([{type:'wheel',...point(event),delta:Math.max(-3,Math.min(3,-event.deltaY/(event.deltaMode===0?120:3)))}]);},{passive:false});
canvas.addEventListener('pointerdown',event=>{if(!ready)return;canvas.focus({preventScroll:true});canvas.setPointerCapture(event.pointerId);drag={button:event.button===2?1:event.button===1?2:0};enqueue([{type:'move',...point(event)},{type:'button',...point(event),button:drag.button,down:true}]);});
canvas.addEventListener('pointermove',event=>{if(ready)enqueue([{type:'move',...point(event)}]);});
canvas.addEventListener('pointerup',event=>{if(!drag)return;const button=drag.button;drag=null;enqueue([{type:'button',...point(event),button,down:false}]);});
canvas.addEventListener('pointercancel',event=>{if(drag)enqueue([{type:'button',...point(event),button:drag.button,down:false}]);drag=null;});
canvas.addEventListener('pointerleave',()=>{if(ready&&!drag)enqueue([{type:'move',x:-100,y:-100}]);});
window.addEventListener('blur',()=>{drag=null;if(ready)enqueue([0,1,2].map(button=>({type:'button',button,down:false})));});
async function connectExistingOrOpen(){
    try{
        const state=await(await fetch('/api/status')).json();
        if(state.view_id===id&&state.phase==='ready'&&state.frame_channel_ready){
            nav=state.sequence;frames();info('恢复当前时间区间…');const until=performance.now()+5000;
            while(performance.now()<until){
                if(nav!==state.sequence)return;
                if(paintedNavigation===nav){ready=true;enqueue([{type:'refresh'}]);return;}
                await delay(50);
            }
            showError('当前视图已定位，但画面未到达。可点击“回到报告位置”重新定位。');return;
        }
    }catch{}
    go('open');
}
connectExistingOrOpen();
