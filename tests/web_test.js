const assert=require('node:assert/strict');
const {validate,defaults}=require('../web/schedule.js');
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
console.log('Web validation and URL round-trip: PASS');
