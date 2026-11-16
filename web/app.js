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
} else $('notice').textContent='Открыт основной пресет. Для загрузки текущего графика откройте /editor в боте.';
const points=()=>data['ch'+channel];
// The controller stores 0…255; the editor shows whole percent.
const percent=v=>Math.round(v/2.55);
const nav=document.querySelector('nav');
names.forEach((name,c)=>{
  const b=document.createElement('button');b.textContent=name;b.setAttribute('role','tab');b.style.setProperty('--channel',colors[c]);
  b.onclick=()=>{channel=c;selected=0;sync();};nav.append(b);
});
function geometry(){const r=canvas.getBoundingClientRect();return {w:r.width,h:r.height,l:37,r:r.width-23,t:18,b:r.height-28};}
function xy(p,g){return [g.l+p.t/1440*(g.r-g.l),g.b-p.v/255*(g.b-g.t)];}
// Segments are drawn with the same smooth curve the controller follows.
function curve(a,g){
 a.forEach((p,i)=>{
   if(!i){ctx.moveTo(...xy(p,g));return;}
   const q=a[i-1],steps=q.v===p.v?1:Math.max(12,Math.ceil((xy(p,g)[0]-xy(q,g)[0])/2));
   for(let k=1;k<=steps;k++){const t=q.t+(p.t-q.t)*k/steps;ctx.lineTo(...xy({t,v:Schedule.valueAt(a,t)},g));}
 });
}
function draw(){
 const g=geometry(),dpr=window.devicePixelRatio||1;
 canvas.width=Math.round(g.w*dpr);canvas.height=Math.round(g.h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,g.w,g.h);
 ctx.font='11px system-ui';ctx.lineWidth=1;
 for(let v=0;v<=255;v+=51){const y=xy({t:0,v},g)[1];ctx.strokeStyle='#28424f';ctx.beginPath();ctx.moveTo(g.l,y);ctx.lineTo(g.r,y);ctx.stroke();ctx.fillStyle='#9eb5c2';ctx.fillText(percent(v)+'%',5,y+4);}
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
 $('time').disabled=edge;$('value').value=percent(p.v);$('remove').disabled=edge;
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
$('value').onchange=()=>{if($('value').value!==''&&Number.isFinite(Number($('value').value)))change(points()[selected].t,Number($('value').value)*2.55);};
$('remove').onclick=()=>{if(selected>0&&selected<points().length-1){points().splice(selected,1);selected--;sync();}};
$('save').onclick=async()=>{
 try{
   Schedule.validate(data);const json=JSON.stringify(data);
   if(new TextEncoder().encode(json).length>2048)throw Error('График слишком большой.');
   if(tg && tg.platform !== 'unknown'){tg.sendData(json);$('notice').textContent='Данные отправлены. Проверьте подтверждение бота.';}
   else{await navigator.clipboard.writeText(json);$('notice').textContent='JSON скопирован. Отправьте его боту сообщением.';}
 }catch(e){$('notice').textContent=e.message;}
};

// ---- Presets -------------------------------------------------------------
// Built-in presets come from presets.js. The owner's presets and the choice of
// the default one live in Telegram CloudStorage (shared by all devices) or, when
// it is unavailable, in this browser's localStorage.
const clone=value=>JSON.parse(JSON.stringify(value));
const store=(()=>{
 let cloud=!!(tg?.isVersionAtLeast?.('6.9')&&tg.CloudStorage&&tg.platform!=='unknown');
 const ask=(method,...args)=>new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>reject(Error('timeout')),2500);
   try{tg.CloudStorage[method](...args,(error,value)=>{clearTimeout(timer);error?reject(Error(error)):resolve(value);});}
   catch(e){clearTimeout(timer);reject(e);}
 });
 const local={get:k=>{try{return localStorage.getItem(k);}catch(e){return null;}},
   set:(k,v)=>localStorage.setItem(k,v),remove:k=>{try{localStorage.removeItem(k);}catch(e){}}};
 // One failure switches to localStorage for the rest of the session.
 const via=async(method,fallback,...args)=>{
   if(cloud){try{return await ask(method,...args);}catch(e){cloud=false;}}
   return fallback(...args);
 };
 return {get:k=>via('getItem',local.get,k).then(v=>v||null),set:(k,v)=>via('setItem',local.set,k,v),remove:k=>via('removeItem',local.remove,k)};
})();
let userPresets=[],defaultPreset=Presets.defaultId;
const allPresets=()=>Presets.builtin.concat(userPresets);
const findPreset=id=>allPresets().find(p=>p.id===id);
const say=text=>{$('notice').textContent=text;};
function listPresets(current){
 const list=$('preset');list.textContent='';
 const add=(value,text)=>{const o=document.createElement('option');o.value=value;o.textContent=text;list.append(o);};
 add('','— график контроллера —');
 for(const p of allPresets()) add(p.id,(p.id===defaultPreset?'★ ':'')+p.name);
 list.value=current&&findPreset(current)?current:'';
 $('drop').disabled=!userPresets.some(p=>p.id===list.value);
 $('star').disabled=!list.value||list.value===defaultPreset;
}
const saveIndex=()=>store.set('presets',JSON.stringify({default:defaultPreset,list:userPresets.map(p=>({id:p.id,name:p.name}))}));
function usePreset(id){
 const p=findPreset(id);if(!p)return false;
 data=clone(p.data);selected=0;valid=true;$('save').disabled=false;sync();return true;
}
async function loadPresets(){
 try{
   const index=JSON.parse(await store.get('presets')||'{}');
   for(const item of (Array.isArray(index.list)?index.list:[]).slice(0,Presets.MAX_USER)){
     const name=Presets.cleanName(item?.name);
     if(!name||!/^u[a-z0-9]{1,20}$/.test(item.id))continue;
     try{userPresets.push({id:item.id,name,data:Schedule.validate(JSON.parse(await store.get('p_'+item.id)))});}catch(e){}
   }
   if(findPreset(index.default))defaultPreset=index.default;
 }catch(e){}
 // Without a schedule from the controller the editor opens the default preset.
 const opened=!initial&&usePreset(defaultPreset)?defaultPreset:'';
 listPresets(opened);
}
$('preset').onchange=()=>{
 const id=$('preset').value;
 if(id&&usePreset(id))say(`Загружен пресет «${findPreset(id).name}». Чтобы применить, нажмите «Сохранить в контроллер».`);
 listPresets(id);
};
$('star').onclick=async()=>{
 const id=$('preset').value;if(!findPreset(id))return;
 defaultPreset=id;listPresets(id);
 try{await saveIndex();say(`«${findPreset(id).name}» — теперь основной пресет.`);}catch(e){say('Не удалось сохранить выбор основного пресета.');}
};
$('add').onclick=()=>{$('naming').hidden=!$('naming').hidden;if(!$('naming').hidden)$('name').focus();};
$('keep').onclick=async()=>{
 const name=Presets.cleanName($('name').value);
 if(!name){say('Введите название до 24 символов.');return;}
 if(userPresets.length>=Presets.MAX_USER){say(`Можно хранить до ${Presets.MAX_USER} своих пресетов. Удалите лишний.`);return;}
 try{
   const preset={id:'u'+Date.now().toString(36),name,data:clone(Schedule.validate(data))};
   await store.set('p_'+preset.id,JSON.stringify(preset.data));
   userPresets.push(preset);await saveIndex();
   $('name').value='';$('naming').hidden=true;listPresets(preset.id);say(`Пресет «${name}» сохранён.`);
 }catch(e){say('Не удалось сохранить пресет: '+e.message);}
};
$('drop').onclick=async()=>{
 const id=$('preset').value,preset=userPresets.find(p=>p.id===id);if(!preset)return;
 userPresets=userPresets.filter(p=>p.id!==id);
 if(defaultPreset===id)defaultPreset=Presets.defaultId;
 listPresets('');
 try{await saveIndex();await store.remove('p_'+id);say(`Пресет «${preset.name}» удалён.`);}catch(e){say('Не удалось удалить пресет из хранилища.');}
};
new ResizeObserver(draw).observe(canvas);sync();listPresets('');loadPresets();
