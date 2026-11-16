const assert=require('node:assert/strict');
const {validate,defaults,valueAt,tangent}=require('../web/schedule.js');
const data=defaults();assert.equal(validate(data),data);
function invalid(change){const copy=structuredClone(data);change(copy);assert.throws(()=>validate(copy));}
invalid(d=>d.ch0[1].t=0);
invalid(d=>d.ch0[1].v=256);
invalid(d=>d.ch0[1].v=-1);
invalid(d=>d.ch0[1].v=2.5);
invalid(d=>d.ch0[1].v='20');
invalid(d=>d.ch0[0].t=1);
invalid(d=>d.ch0.at(-1).t=1439);
invalid(d=>d.ch0.at(-1).v=1);
invalid(d=>delete d.ch3);
invalid(d=>d.extra=[]);
invalid(d=>d.ch0[1].extra=1);
invalid(d=>d.ch0=Array.from({length:13},(_,i)=>({t:i*120,v:0})));
assert.deepEqual(validate(JSON.parse(decodeURIComponent(encodeURIComponent(JSON.stringify(data))))),data);
// The editor curve matches the firmware: a monotone spline through the points.
const ramp=[{t:0,v:0},{t:60,v:240},{t:120,v:0},{t:1440,v:0}];
assert.equal(valueAt(ramp,0),0);assert.equal(valueAt(ramp,60),240);assert.ok(Math.abs(valueAt(ramp,30)-120)<1e-9);
assert.ok(Math.abs(valueAt(ramp,15)-37.5)<1e-9);assert.ok(Math.abs(valueAt(ramp,75)-202.5)<1e-9);assert.equal(valueAt(ramp,800),0);
const rise=[{t:0,v:0},{t:60,v:100},{t:120,v:200},{t:180,v:0},{t:1440,v:0}];
assert.equal(valueAt(rise,60),100);assert.ok(tangent(rise,1)>1.2);assert.equal(tangent(rise,2),0);assert.equal(tangent(rise,0),0);
for(let m=0.5;m<=120;m+=0.5){assert.ok(valueAt(rise,m)>=valueAt(rise,m-0.5));assert.ok(valueAt(rise,m)<=200);}
const night=[{t:0,v:100},{t:360,v:0},{t:1080,v:200},{t:1440,v:100}];
assert.ok(tangent(night,0)<0);assert.equal(tangent(night,0),tangent(night,3));
// Built-in presets: valid for the controller, small enough to send, one default
// that is the schedule built into the firmware.
const Presets=require('../web/presets.js'),fs=require('node:fs'),path=require('node:path');
assert.ok(Presets.builtin.length>=3);
assert.equal(new Set(Presets.builtin.map(p=>p.id)).size,Presets.builtin.length);
for(const p of Presets.builtin){
  assert.ok(Presets.cleanName(p.name)===p.name,p.name);assert.ok(!p.id.startsWith('u'));
  validate(p.data);assert.ok(Buffer.byteLength(JSON.stringify(p.data))<=2048,p.name);
  for(const c of Object.values(p.data))for(let m=0;m<=1440;m+=5){const v=valueAt(c,m);assert.ok(v>=0&&v<=255);}
}
const main=Presets.builtin.find(p=>p.id===Presets.defaultId);assert.ok(main);
assert.deepEqual(main.data,JSON.parse(fs.readFileSync(path.join(__dirname,'../data/schedule.json'),'utf8')));
assert.equal(Presets.cleanName('  Мой   свет '),'Мой свет');assert.equal(Presets.cleanName(''),'');assert.equal(Presets.cleanName('x'.repeat(25)),'');
// The page addresses its scripts by content hash (scripts/stamp_web.py).
const hash=require('node:crypto').createHash('sha256');
for(const name of ['schedule.js','presets.js','app.js'])hash.update(fs.readFileSync(path.join(__dirname,'../web',name)));
const stamp=hash.digest('hex').slice(0,8),page=fs.readFileSync(path.join(__dirname,'../web/index.html'),'utf8');
for(const name of ['schedule.js','presets.js','app.js'])
  assert.ok(page.includes(`src="${name}?v=${stamp}"`),`web/index.html is stale: run python3 scripts/stamp_web.py (${name})`);
console.log('Web validation, smooth curve, presets, script stamps and URL round-trip: PASS');
