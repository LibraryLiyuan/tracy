const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync(__dirname+'/viewer.js','utf8');
const sandbox={};vm.runInNewContext(source.slice(0,source.indexOf('const canvas=')),sandbox);
let checks=0;
for(const dpr of [1,1.25,1.5,2])for(const available of [345,1915,2560]){
    for(const [mode,scale] of [['native',1],['double',2]]){
        const layout=sandbox.traceDisplayLayout(1600,1000,dpr,available,mode);
        assert.ok(Math.abs(layout.width*dpr-1600*scale)<1e-6);
        assert.ok(Math.abs(layout.height*dpr-1000*scale)<1e-6);
        assert.equal(layout.smoothing,false);
        // Hit testing must select the same native coordinate even after scroll.
        const rect={left:-217.5,top:183.25,width:layout.width,height:layout.height};
        const point=sandbox.tracePoint(rect.left+rect.width*.75,rect.top+rect.height*.25,rect,1600,1000);
        assert.ok(Math.abs(point.x-1200)<1e-6);assert.ok(Math.abs(point.y-250)<1e-6);checks++;
    }
    const fit=sandbox.traceDisplayLayout(1600,1000,dpr,available,'fit');
    assert.ok(fit.width<=available);assert.equal(fit.smoothing,true);checks++;
}
assert.equal(sandbox.traceDisplayLayout(1600,1000,1,1915,'native').width,1600);
console.log(JSON.stringify({passed:true,geometry_and_hit_checks:checks,dpr:[1,1.25,1.5,2],viewport_widths:[345,1915,2560]}));
