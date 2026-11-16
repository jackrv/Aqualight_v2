const assert=require('node:assert/strict');
const {validate,defaults,ease,valueAt}=require('../web/schedule.js');
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
// The editor curve matches the firmware: cosine easing between points.
assert.equal(ease(0),0);assert.equal(ease(1),1);assert.ok(Math.abs(ease(.5)-.5)<1e-12);assert.ok(Math.abs(ease(.25)-.1464466)<1e-6);
const ramp=[{t:0,v:0},{t:60,v:240},{t:120,v:0},{t:1440,v:0}];
assert.equal(valueAt(ramp,0),0);assert.equal(valueAt(ramp,60),240);assert.ok(Math.abs(valueAt(ramp,30)-120)<1e-9);
assert.equal(Math.round(valueAt(ramp,15)),35);assert.equal(Math.round(valueAt(ramp,75)),205);assert.equal(valueAt(ramp,800),0);
console.log('Web validation, easing curve and URL round-trip: PASS');
