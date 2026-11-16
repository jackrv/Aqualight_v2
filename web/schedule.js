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
  // Cosine ease-in-out between two points; the firmware uses the same curve.
  function ease(progress) { return (1-Math.cos(progress*Math.PI))/2; }
  // Brightness of one channel (array of points) at a fractional minute of the day.
  function valueAt(points, minute) {
    for (let i=1;i<points.length;i++) {
      const a=points[i-1],b=points[i];
      if (minute<=b.t) return a.v+(b.v-a.v)*ease((minute-a.t)/(b.t-a.t));
    }
    return 0;
  }
  const api={validate,defaults,ease,valueAt};
  if(typeof module!=='undefined') module.exports=api; else root.Schedule=api;
})(globalThis);
