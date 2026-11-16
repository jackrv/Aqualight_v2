/* Shared validation used by the editor and host-side tests. */
(function(root) {
  function validate(data) {
    if (!data || typeof data !== 'object' || Array.isArray(data) || Object.keys(data).length !== 4) throw Error('Нужны четыре канала ch0…ch3.');
    for (let c=0;c<4;c++) {
      const a=data['ch'+c];
      if (!Array.isArray(a) || a.length<2 || a.length>12) throw Error('В каждом канале должно быть 2–12 точек.');
      a.forEach((p,i)=>{
        if (!p || Object.keys(p).length!==2 || !Number.isInteger(p.t) || !Number.isInteger(p.v) || p.t<0 || p.t>1440 || p.v<0 || p.v>255 || (i && p.t<=a[i-1].t)) throw Error('Неверные точки или порядок времени.');
      });
      if (a[0].t!==0 || a[a.length-1].t!==1440 || a[0].v!==a[a.length-1].v) throw Error('Края суток: 00:00 и 24:00 с одинаковой яркостью.');
    }
    return data;
  }
  function defaults() {
    const data={};
    for(let c=0;c<4;c++) data['ch'+c]=[{t:0,v:0},{t:420,v:0},{t:540,v:180},{t:1140,v:180},{t:1260,v:0},{t:1440,v:0}];
    return data;
  }
  // Monotone cubic spline (PCHIP) through the points, identical to the firmware
  // (include/schedule.h): no corners, no overshoot, the day wraps at midnight.
  function slope(a,k) { return (a[k+1].v-a[k].v)/(a[k+1].t-a[k].t); }
  function tangent(a,k) {
    const last=a.length-1,before=k===0?last-1:k-1,after=k===last?0:k;
    const d0=slope(a,before),d1=slope(a,after);
    if (d0*d1<=0) return 0;
    const h0=a[before+1].t-a[before].t,h1=a[after+1].t-a[after].t,w1=2*h1+h0,w2=h1+2*h0;
    return (w1+w2)/(w1/d0+w2/d1);
  }
  // Brightness of one channel (array of points) at a fractional minute of the day.
  function valueAt(points, minute) {
    for (let i=1;i<points.length;i++) {
      const a=points[i-1],b=points[i];
      if (minute<=b.t) {
        if (a.v===b.v) return a.v;
        const h=b.t-a.t,p=(minute-a.t)/h,p2=p*p,p3=p2*p;
        const v=(2*p3-3*p2+1)*a.v+(p3-2*p2+p)*h*tangent(points,i-1)+(3*p2-2*p3)*b.v+(p3-p2)*h*tangent(points,i);
        return Math.max(Math.min(a.v,b.v),Math.min(Math.max(a.v,b.v),v));
      }
    }
    return 0;
  }
  const api={validate,defaults,valueAt,tangent};
  if(typeof module!=='undefined') module.exports=api; else root.Schedule=api;
})(globalThis);
