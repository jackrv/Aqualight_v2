/* Built-in presets and the rules for user presets; shared with host-side tests. */
(function(root) {
  const h=(hours,minutes=0)=>hours*60+minutes;
  // One channel from [minute, brightness] pairs; midnight (0 and 1440) is dark unless given.
  function ch(...pairs) {
    const points=pairs.map(([t,v])=>({t,v}));
    if (points[0].t!==0) points.unshift({t:0,v:0});
    if (points[points.length-1].t!==1440) points.push({t:1440,v:points[0].v});
    return points;
  }
  const schedule=(white,red,green,blue)=>({ch0:white,ch1:red,ch2:green,ch3:blue});
  // Values are perceived brightness 0…255 (the controller applies gamma).
  const builtin=[
    // The default one; identical to data/schedule.json and the schedule built into the firmware.
    {id:'plants',name:'Травник · 10 часов',data:schedule(
      ch([h(8),0],[h(10),240],[h(18),240],[h(20),0]),
      ch([h(7,30),0],[h(9,30),220],[h(18,30),220],[h(20,30),0]),
      ch([h(8),0],[h(10),160],[h(18),160],[h(20),0]),
      ch([h(8),0],[h(10),200],[h(18),200],[h(20),40],[h(22),40],[h(23),0]))},
    {id:'calm',name:'Спокойный · 8 часов',data:schedule(
      ch([h(9),0],[h(11),180],[h(17),180],[h(19),0]),
      ch([h(8,30),0],[h(10,30),160],[h(17,30),160],[h(19,30),0]),
      ch([h(9),0],[h(11),110],[h(17),110],[h(19),0]),
      ch([h(9),0],[h(11),140],[h(17),140],[h(19),30],[h(21),30],[h(22),0]))},
    {id:'siesta',name:'С дневной паузой',data:schedule(
      ch([h(7),0],[h(8,30),230],[h(11,30),230],[h(12,30),30],[h(15,30),30],[h(16,30),230],[h(20),230],[h(21,30),0]),
      ch([h(6,30),0],[h(8),210],[h(11,30),210],[h(12,30),30],[h(15,30),30],[h(16,30),210],[h(20,30),210],[h(22),0]),
      ch([h(7),0],[h(8,30),150],[h(11,30),150],[h(12,30),20],[h(15,30),20],[h(16,30),150],[h(20),150],[h(21,30),0]),
      ch([h(7),0],[h(8,30),190],[h(11,30),190],[h(12,30),30],[h(15,30),30],[h(16,30),190],[h(20),190],[h(21,30),40],[h(23),0]))},
    {id:'evening',name:'Вечерний · 13–23',data:schedule(
      ch([h(12),0],[h(14),230],[h(21),230],[h(23),0]),
      ch([h(11,30),0],[h(13,30),210],[h(21,30),210],[h(23,30),0]),
      ch([h(12),0],[h(14),150],[h(21),150],[h(23),0]),
      ch([h(12),0],[h(14),190],[h(21),190],[h(23),50]))},
    {id:'vacation',name:'Отпуск · 6 часов',data:schedule(
      ch([h(10),0],[h(11,30),140],[h(16,30),140],[h(18),0]),
      ch([h(9,30),0],[h(11),150],[h(17),150],[h(18,30),0]),
      ch([h(10),0],[h(11,30),90],[h(16,30),90],[h(18),0]),
      ch([h(10),0],[h(11,30),110],[h(16,30),110],[h(18),0]))},
    // The owner's schedule from the previous aquarium firmware, converted for gamma.
    {id:'legacy',name:'Прежний график',data:schedule(
      ch([h(7),0],[h(9),186],[h(11),230],[h(12),202],[h(18),90],[h(19),0]),
      ch([h(6),0],[h(8,59),228],[h(9),255],[h(11),255],[h(11,1),122],[h(13),0]),
      ch([h(18),0],[h(20),167],[h(22),167]),
      ch([h(6),0],[h(9),255],[h(11),255],[h(11,1),122],[h(13),0],[h(18),0],[h(20),228],[h(22),228],[h(22,1),255]))},
  ];
  const defaultId='plants';   // the default until the owner picks another one
  const MAX_USER=12, MAX_NAME=24;
  // Trimmed name, or '' when it cannot be used.
  function cleanName(name) {
    const text=String(name??'').replace(/\s+/g,' ').trim();
    return text.length>=1&&text.length<=MAX_NAME?text:'';
  }
  const api={builtin,defaultId,MAX_USER,MAX_NAME,cleanName};
  if(typeof module!=='undefined') module.exports=api; else root.Presets=api;
})(globalThis);
