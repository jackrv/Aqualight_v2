'use strict';
const tg=window.Telegram?.WebApp;
tg?.ready();tg?.expand();
if(tg?.isVersionAtLeast?.('7.7')) tg.disableVerticalSwipes();
const $=id=>document.getElementById(id);
const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d');
const colors=['#f1e9ce','#ff7a82','#60dbaa','#76b8ff'],names=['Белый','Красный','Зелёный','Синий'];
let data=Schedule.defaults(),channel=0,selected=0,drag=false,valid=true;
const initial=new URLSearchParams(location.search).get('data');
if(initial) {
  try{data=Schedule.validate(JSON.parse(initial));}
  catch(e){valid=false;$('notice').textContent='Ошибка входного графика: '+e.message;$('save').disabled=true;}
} else $('notice').textContent='Демонстрационный график. Для загрузки текущего откройте /editor в боте.';
const points=()=>data['ch'+channel];
const nav=document.querySelector('nav');
names.forEach((name,c)=>{
  const b=document.createElement('button');b.textContent=name;b.setAttribute('role','tab');b.style.setProperty('--channel',colors[c]);
  b.onclick=()=>{channel=c;selected=0;sync();};nav.append(b);
});
function geometry(){const r=canvas.getBoundingClientRect();return {w:r.width,h:r.height,l:37,r:r.width-23,t:18,b:r.height-28};}
function xy(p,g){return [g.l+p.t/1440*(g.r-g.l),g.b-p.v/255*(g.b-g.t)];}
// Segments are drawn with the same cosine easing the controller applies.
function curve(a,g){
 a.forEach((p,i)=>{
   if(!i){ctx.moveTo(...xy(p,g));return;}
   const q=a[i-1],steps=q.v===p.v?1:Math.max(2,Math.ceil((xy(p,g)[0]-xy(q,g)[0])/3));
   for(let k=1;k<=steps;k++){const t=q.t+(p.t-q.t)*k/steps;ctx.lineTo(...xy({t,v:Schedule.valueAt(a,t)},g));}
 });
}
function draw(){
 const g=geometry(),dpr=window.devicePixelRatio||1;
 canvas.width=Math.round(g.w*dpr);canvas.height=Math.round(g.h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,g.w,g.h);
 ctx.font='11px system-ui';ctx.lineWidth=1;
 for(let v=0;v<=255;v+=51){const y=xy({t:0,v},g)[1];ctx.strokeStyle='#28424f';ctx.beginPath();ctx.moveTo(g.l,y);ctx.lineTo(g.r,y);ctx.stroke();ctx.fillStyle='#9eb5c2';ctx.fillText(v,5,y+4);}
 for(let hour=0;hour<=24;hour+=6){const x=xy({t:hour*60,v:0},g)[0];ctx.fillStyle='#9eb5c2';ctx.fillText(String(hour).padStart(2,'0')+':00',x-14,g.h-8);}
 for(const c of [0,1,2,3].filter(c=>c!==channel).concat(channel)){
   ctx.globalAlpha=c===channel?1:.23;ctx.strokeStyle=colors[c];ctx.lineWidth=c===channel?2.5:1.5;ctx.beginPath();
   curve(data['ch'+c],g);ctx.stroke();
 }
 ctx.globalAlpha=1;
 points().forEach((p,i)=>{const [x,y]=xy(p,g);ctx.beginPath();ctx.arc(x,y,i===selected?7:5,0,Math.PI*2);ctx.fillStyle=colors[channel];ctx.fill();if(i===selected){ctx.strokeStyle='#fff';ctx.lineWidth=2;ctx.stroke();}});
}
function sync(){
 const p=points()[selected],edge=selected===0||selected===points().length-1;
 $('time').value=p.t===1440?'00:00':String(Math.floor(p.t/60)).padStart(2,'0')+':'+String(p.t%60).padStart(2,'0');
 $('time').disabled=edge;$('value').value=p.v;$('remove').disabled=edge;
 $('point').textContent=`Точка ${selected+1} из ${points().length}${p.t===1440?' · 24:00 (конец суток)':''}`;
 [...nav.children].forEach((b,i)=>b.setAttribute('aria-selected',String(i===channel)));draw();
}
function change(t,v){
 const a=points(),p=a[selected];
 if(selected>0&&selected<a.length-1) p.t=Math.max(a[selected-1].t+1,Math.min(a[selected+1].t-1,Math.round(t)));
 p.v=Math.max(0,Math.min(255,Math.round(v)));
 if(selected===0) a[a.length-1].v=p.v;
 if(selected===a.length-1) a[0].v=p.v;
 sync();
}
function position(e){const r=canvas.getBoundingClientRect();return [e.clientX-r.left,e.clientY-r.top];}
canvas.onpointerdown=e=>{
 if(!valid)return;e.preventDefault();const g=geometry(),[x,y]=position(e);
 let nearest=-1,best=24;
 points().forEach((p,i)=>{const [px,py]=xy(p,g),distance=Math.hypot(x-px,y-py);if(distance<best){best=distance;nearest=i;}});
 if(nearest<0){
   const t=Math.round((x-g.l)/(g.r-g.l)*1440),v=Math.max(0,Math.min(255,Math.round((g.b-y)/(g.b-g.t)*255)));
   if(points().length>=12||t<=0||t>=1440||points().some(p=>p.t===t))return;
   nearest=points().findIndex(p=>p.t>t);points().splice(nearest,0,{t,v});
 }
 selected=nearest;drag=true;canvas.setPointerCapture(e.pointerId);sync();
};
canvas.onpointermove=e=>{if(!drag)return;e.preventDefault();const g=geometry(),[x,y]=position(e);change((x-g.l)/(g.r-g.l)*1440,(g.b-y)/(g.b-g.t)*255);};
canvas.onpointerup=canvas.onpointercancel=()=>{drag=false;};
canvas.addEventListener('touchstart',e=>e.preventDefault(),{passive:false});
canvas.addEventListener('touchmove',e=>e.preventDefault(),{passive:false});
$('time').onchange=()=>{const [h,m]=$('time').value.split(':').map(Number);if(Number.isFinite(h)&&Number.isFinite(m))change(h*60+m,points()[selected].v);};
$('value').onchange=()=>{if($('value').value!==''&&Number.isFinite(Number($('value').value)))change(points()[selected].t,Number($('value').value));};
$('remove').onclick=()=>{if(selected>0&&selected<points().length-1){points().splice(selected,1);selected--;sync();}};
$('save').onclick=async()=>{
 try{
   Schedule.validate(data);const json=JSON.stringify(data);
   if(new TextEncoder().encode(json).length>2048)throw Error('График слишком большой.');
   if(tg && tg.platform !== 'unknown'){tg.sendData(json);$('notice').textContent='Данные отправлены. Проверьте подтверждение бота.';}
   else{await navigator.clipboard.writeText(json);$('notice').textContent='JSON скопирован. Отправьте его боту сообщением.';}
 }catch(e){$('notice').textContent=e.message;}
};
new ResizeObserver(draw).observe(canvas);sync();
