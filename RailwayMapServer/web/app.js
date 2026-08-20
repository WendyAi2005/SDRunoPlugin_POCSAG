const listEl=document.getElementById('train-list');
const connectionEl=document.getElementById('connection');
const unavailableEl=document.getElementById('map-unavailable');
const rawFormatEl=document.getElementById('raw-format');
const sourceDatumEl=document.getElementById('source-datum');
const mapTransformEl=document.getElementById('map-transform');
const coordinateModeEl=document.getElementById('coordinate-mode');
const positionPriorityEl=document.getElementById('position-priority');
const comparePositionsEl=document.getElementById('compare-positions');
const showAnchorsEl=document.getElementById('show-anchors');
const learningEnabledEl=document.getElementById('learning-enabled');
const defaultLineEl=document.getElementById('default-line');
const mileageSummaryEl=document.getElementById('mileage-summary');
const mileageSearchResultEl=document.getElementById('mileage-search-result');
const markers=new Map(),tracks=new Map(),targetsByUid=new Map(),comparisonLayers=new Map(),osmEstimates=new Map(),osmPending=new Set();
let anchorLayer=null,mileageAnchors=[],lineOverrides={};
const defaults={rawFormat:'NMEA',sourceDatum:'WGS84',mapTransform:'NONE',positionPriority:'AUTO',comparePositions:false,showAnchors:false,defaultLine:'沪昆线'};
const transformLabels={NONE:'no transform',WGS84_GCJ02:'WGS84 → GCJ-02',WGS84_BD09:'WGS84 → BD-09',GCJ02_WGS84:'GCJ-02 → WGS84',GCJ02_BD09:'GCJ-02 → BD-09',BD09_WGS84:'BD-09 → WGS84'};
let settings={...defaults},lastTrains=[],map=null,tiles=null,hasFitted=false;

function esc(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function text(v,fallback='未知'){return v===null||v===undefined||v===''?fallback:String(v)}
function age(t){return `${Math.max(0,Math.round((Date.now()-t)/1000))}秒前`}
function title(t){return t.train||(`机车 ${text(t.locomotive_id)}`)}
function uidOf(t){return String(t.target_uid??t.id)}
function parseNmea(raw,isLongitude){
  const value=String(raw??''),degreeDigits=isLongitude?3:2,expected=degreeDigits+6,max=isLongitude?180:90;
  if(!new RegExp(`^\\d{${expected}}$`).test(value))return null;
  const degrees=Number(value.slice(0,degreeDigits)),minutes=Number(value.slice(degreeDigits))/10000;
  if(degrees>max||minutes>=60||(degrees===max&&minutes>0))return null;
  return{decimal:degrees+minutes/60,dm:`${degrees}°${minutes.toFixed(4)}′ ${isLongitude?'E':'N'}`,minutes};
}
function parseDirectDecimal(raw,isLongitude){
  const value=String(raw??'');if(!/^\d+$/.test(value))return null;
  const decimal=Number(value)/1e6,max=isLongitude?180:90;
  return decimal<=max?{decimal,dm:'--',minutes:null}:null;
}
function baseCoordinate(item){
  const format=settings.rawFormat;
  let lon=format==='DECIMAL'?parseDirectDecimal(item.longitude_raw,true):parseNmea(item.longitude_raw,true);
  let lat=format==='DECIMAL'?parseDirectDecimal(item.latitude_raw,false):parseNmea(item.latitude_raw,false);
  if(format==='AUTO'&&(!lon||!lat)){lon=parseDirectDecimal(item.longitude_raw,true);lat=parseDirectDecimal(item.latitude_raw,false)}
  // Compatibility fallback is allowed only for older state files that do not
  // contain RAW fields.  A present but malformed RAW coordinate is invalid and
  // must never be forced onto the map via the already-converted values.
  if((!lon||!lat)&&format!=='DECIMAL'&&!item.longitude_raw&&!item.latitude_raw&&item.longitude!=null&&item.latitude!=null&&(!item.position_source||item.position_source==='RADIO_GPS')){const legacy=numericCoordinate(item.longitude,item.latitude,'RADIO_GPS');if(legacy){legacy.lonDm=item.longitude_degree_minute||'--';legacy.latDm=item.latitude_degree_minute||'--';return legacy}}
  if(lon&&lat){const parsed=numericCoordinate(lon.decimal,lat.decimal,'RADIO_GPS');if(parsed){parsed.lonDm=lon.dm;parsed.latDm=lat.dm;return parsed}}
  return null;
}
function numericCoordinate(lon,lat,source){
  if(lon===null||lon===undefined||lon===''||lat===null||lat===undefined||lat==='')return null;
  lon=Number(lon);lat=Number(lat);
  if(!Number.isFinite(lon)||!Number.isFinite(lat)||lon<-180||lon>180||lat<-90||lat>90||(lon===0&&lat===0))return null;
  return{lon,lat,lonDm:'--',latDm:'--',source};
}
function effectiveLine(t){const override=String(lineOverrides[uidOf(t)]||'').trim(),radio=String(t.line_name||'').trim();return override||radio||settings.defaultLine||'沪昆线'}
function effectiveLineOrigin(t){return lineOverrides[uidOf(t)]?'手动':(String(t.line_name||'').trim()?'报文':'默认')}
function selectedCoordinate(t){
  const anyRadio=baseCoordinate(t)||numericCoordinate(t.radio_longitude,t.radio_latitude,'RADIO_GPS');
  if(anyRadio)anyRadio.source='RADIO_GPS';const radio=t.radio_gps_fresh===false?null:anyRadio;
  const manualLine=String(lineOverrides[uidOf(t)]||'').trim(),backendLine=String(t.line_name||'').trim(),trustBackendMileage=!manualLine||manualLine===backendLine;
  const mileage=trustBackendMileage?(numericCoordinate(t.mileage_longitude,t.mileage_latitude,t.mileage_position_source)||((t.position_source||'').startsWith('LOCAL_')?numericCoordinate(t.display_longitude,t.display_latitude,t.position_source):null)):null;
  const osm=osmEstimates.get(uidOf(t))||null;
  switch(settings.positionPriority){case'RADIO':return anyRadio;case'MILEAGE':return mileage;case'OSM':return osm;default:return radio||mileage||osm||numericCoordinate(t.display_longitude??t.longitude,t.display_latitude??t.latitude,t.position_source)}
}
function trackCoordinate(p){return p.source&&p.source!=='RADIO_GPS'?numericCoordinate(p.longitude,p.latitude,p.source):baseCoordinate(p)||numericCoordinate(p.longitude,p.latitude,p.source||'RADIO_GPS')}
function sourceLabel(source){return({RADIO_GPS:'无线 GPS',LOCAL_MILEAGE_EXACT:'本地公里标',LOCAL_MILEAGE_INTERPOLATED:'本地公里标插值',OSM_MILEAGE_EXACT:'OpenStreetMap 公里标',OSM_MILEAGE_INTERPOLATED:'OSM 公里标插值',NO_POSITION:'无可靠位置'})[source]||text(source)}

const PI=Math.PI,AXIS=6378245.0,EE=0.00669342162296594323,XPI=PI*3000.0/180.0;
function outsideChina(lon,lat){return lon<72.004||lon>137.8347||lat<0.8293||lat>55.8271}
function transformLat(x,y){let r=-100+2*x+3*y+.2*y*y+.1*x*y+.2*Math.sqrt(Math.abs(x));r+=(20*Math.sin(6*x*PI)+20*Math.sin(2*x*PI))*2/3;r+=(20*Math.sin(y*PI)+40*Math.sin(y/3*PI))*2/3;r+=(160*Math.sin(y/12*PI)+320*Math.sin(y*PI/30))*2/3;return r}
function transformLon(x,y){let r=300+x+2*y+.1*x*x+.1*x*y+.1*Math.sqrt(Math.abs(x));r+=(20*Math.sin(6*x*PI)+20*Math.sin(2*x*PI))*2/3;r+=(20*Math.sin(x*PI)+40*Math.sin(x/3*PI))*2/3;r+=(150*Math.sin(x/12*PI)+300*Math.sin(x/30*PI))*2/3;return r}
function wgsToGcj(lon,lat){if(outsideChina(lon,lat))return[lon,lat];let dLat=transformLat(lon-105,lat-35),dLon=transformLon(lon-105,lat-35),rad=lat/180*PI,magic=Math.sin(rad);magic=1-EE*magic*magic;const sqrt=Math.sqrt(magic);dLat=dLat*180/((AXIS*(1-EE))/(magic*sqrt)*PI);dLon=dLon*180/(AXIS/sqrt*Math.cos(rad)*PI);return[lon+dLon,lat+dLat]}
function gcjToWgs(lon,lat){if(outsideChina(lon,lat))return[lon,lat];let wLon=lon,wLat=lat;for(let i=0;i<8;i++){const g=wgsToGcj(wLon,wLat);wLon-=g[0]-lon;wLat-=g[1]-lat}return[wLon,wLat]}
function gcjToBd(lon,lat){const z=Math.sqrt(lon*lon+lat*lat)+.00002*Math.sin(lat*XPI),theta=Math.atan2(lat,lon)+.000003*Math.cos(lon*XPI);return[z*Math.cos(theta)+.0065,z*Math.sin(theta)+.006]}
function bdToGcj(lon,lat){const x=lon-.0065,y=lat-.006,z=Math.sqrt(x*x+y*y)-.00002*Math.sin(y*XPI),theta=Math.atan2(y,x)-.000003*Math.cos(x*XPI);return[z*Math.cos(theta),z*Math.sin(theta)]}
function mapCoordinate(base){
  const {lon,lat}=base;
  switch(settings.mapTransform){case'WGS84_GCJ02':return wgsToGcj(lon,lat);case'WGS84_BD09':return gcjToBd(...wgsToGcj(lon,lat));case'GCJ02_WGS84':return gcjToWgs(lon,lat);case'GCJ02_BD09':return gcjToBd(lon,lat);case'BD09_WGS84':return gcjToWgs(...bdToGcj(lon,lat));default:return[lon,lat]}
}
function modeLabel(){return`${settings.sourceDatum} / ${transformLabels[settings.mapTransform]} / ${settings.rawFormat} / ${settings.positionPriority}`}
function popup(t,base,shown,line){const radio=numericCoordinate(t.radio_longitude,t.radio_latitude,'RADIO_GPS'),source=base.source||t.position_source||'NO_POSITION';return `<b>${esc(title(t))}</b><br>车次：${esc(text(t.train))}<br>速度：${t.speed_kmh==null?'未知':t.speed_kmh+' km/h'}<br>公里标：${t.kilometer_km==null?'未知':'K'+Number(t.kilometer_km).toFixed(1)}<br>机车号：${esc(text(t.locomotive_id))}<br>机车端号：${esc(text(t.locomotive_end))}<br>线路：${esc(line)}（${esc(effectiveLineOrigin(t))}）<br><br>Lon RAW: ${esc(text(t.longitude_raw))}<br>Lat RAW: ${esc(text(t.latitude_raw))}<br>Lon DM: ${esc(text(t.longitude_degree_minute||base.lonDm))}<br>Lat DM: ${esc(text(t.latitude_degree_minute||base.latDm))}<br>Radio GPS: ${radio?radio.lon.toFixed(6):'--'}, ${radio?radio.lat.toFixed(6):'--'}<br>Displayed: ${base.lon.toFixed(6)}, ${base.lat.toFixed(6)}<br>Map coordinate: ${shown[0].toFixed(6)}, ${shown[1].toFixed(6)}<br>定位来源：${esc(sourceLabel(source))}<br>位置质量：${esc(text(t.position_quality||t.mileage_position_quality))}<br>置信度：${t.position_confidence==null?'--':Number(t.position_confidence).toFixed(2)}<br>GPS vs mileage：${esc(text(t.gps_mileage_comparison,'UNAVAILABLE'))}${t.gps_vs_mileage_distance_m==null?'':' / '+Number(t.gps_vs_mileage_distance_m).toFixed(1)+' m'}<br>Map mode: ${esc(modeLabel())}<br><br>最后更新：${age(t.last_update_unix_ms)}<br>报文质量：${esc(text(t.quality))}`}

function initMap(){
  if(!window.L){unavailableEl.hidden=false;return false}
  map=L.map('map').setView([27.87925,112.910493],9);tiles=L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'});
  let tileOk=false;tiles.on('load',()=>{tileOk=true;unavailableEl.hidden=true});tiles.on('tileerror',()=>{if(!tileOk)unavailableEl.hidden=false});tiles.addTo(map);setTimeout(()=>{if(!tileOk)unavailableEl.hidden=false},5000);return true;
}
function clearComparison(uid){const layers=comparisonLayers.get(uid)||[];layers.forEach(layer=>map&&map.removeLayer(layer));comparisonLayers.delete(uid)}
function renderComparison(t,uid){
  clearComparison(uid);if(!map||!settings.comparePositions)return;
  const radio=baseCoordinate(t)||numericCoordinate(t.radio_longitude,t.radio_latitude,'RADIO_GPS');
  const mileage=numericCoordinate(t.mileage_longitude,t.mileage_latitude,t.mileage_position_source);
  if(!radio||!mileage)return;
  const rp=mapCoordinate(radio),mp=mapCoordinate(mileage),distance=t.gps_vs_mileage_distance_m==null?'--':Number(t.gps_vs_mileage_distance_m).toFixed(1);
  const gps=L.circleMarker([rp[1],rp[0]],{radius:6,color:'#29c46a',fillOpacity:.9}).bindTooltip(`GPS`),km=L.circleMarker([mp[1],mp[0]],{radius:6,color:'#ffad33',fillOpacity:.75}).bindTooltip(`Mileage`),line=L.polyline([[rp[1],rp[0]],[mp[1],mp[0]]],{color:'#ffd166',weight:2,dashArray:'5 5'}).bindTooltip(`偏差：${distance} m`);
  [gps,km,line].forEach(layer=>layer.addTo(map));comparisonLayers.set(uid,[gps,km,line]);
}
async function requestOsm(t){
  const uid=uidOf(t),line=effectiveLine(t);if(osmPending.has(uid)||osmEstimates.has(uid)||!line)return;osmPending.add(uid);
  try{const url=`/api/mileage/lookup?line=${encodeURIComponent(line)}&km=${encodeURIComponent(t.kilometer_km)}`;const result=await(await fetch(url,{cache:'no-store'})).json();if(result.valid){const coordinate=numericCoordinate(result.longitude,result.latitude,result.source);if(coordinate)osmEstimates.set(uid,coordinate);osmPending.delete(uid);render(lastTrains)}else if(result.pending){setTimeout(()=>{osmPending.delete(uid);requestOsm(t)},3000)}else osmPending.delete(uid)}catch(_){osmPending.delete(uid)}
}
function renderAnchorLayer(){
  if(anchorLayer&&map){map.removeLayer(anchorLayer);anchorLayer=null}if(!map||!settings.showAnchors)return;
  anchorLayer=L.layerGroup(mileageAnchors.map(a=>L.circleMarker([Number(a.latitude),Number(a.longitude)],{radius:3,color:'#d58cff',fillOpacity:.7}).bindTooltip(`${esc(a.line)} K${Number(a.mileage_km).toFixed(1)} · ${a.samples}样本`))).addTo(map);
}
async function refreshMileageSummary(){
  try{const data=await(await fetch('/api/mileage/anchors',{cache:'no-store'})).json();mileageAnchors=data.anchors||[];learningEnabledEl.checked=data.learning_enabled!==false;const groups=new Map();mileageAnchors.forEach(a=>{const g=groups.get(a.line)||{count:0,min:Infinity,max:-Infinity,samples:0};g.count++;g.min=Math.min(g.min,Number(a.mileage_km));g.max=Math.max(g.max,Number(a.mileage_km));g.samples+=Number(a.samples||0);groups.set(a.line,g)});mileageSummaryEl.innerHTML=groups.size?[...groups].map(([line,g])=>`<b>${esc(line)}</b><br>锚点：${g.count} · 覆盖 K${g.min.toFixed(1)}–K${g.max.toFixed(1)} · GPS样本：${g.samples}`).join('<br>'):'尚未学习到公里标锚点';renderAnchorLayer()}catch(_){mileageSummaryEl.textContent='公里标数据库暂不可用'}
}
function render(trains){
  lastTrains=trains;listEl.replaceChildren();coordinateModeEl.textContent=`当前：${modeLabel()}`;const liveIds=new Set(),bounds=[];
  trains.forEach(t=>{
    const uid=uidOf(t),lineName=effectiveLine(t),lineOrigin=effectiveLineOrigin(t);liveIds.add(uid);const base=selectedCoordinate(t);const source=base?.source||t.position_source||'NO_POSITION';const card=document.createElement('article');card.className='train'+(t.stale?' stale':'');card.innerHTML=`<h2>${esc(title(t))}</h2><p>${t.speed_kmh==null?'速度未知':t.speed_kmh+' km/h'} · ${t.kilometer_km==null?'公里标未知':'K'+Number(t.kilometer_km).toFixed(1)}</p><p>${esc(lineName)} <span class="line-origin">(${esc(lineOrigin)})</span> · ${age(t.last_update_unix_ms)}</p><p>定位：${esc(sourceLabel(source))}</p><span class="badge">${esc(text(t.data_completeness))} / ${t.stale?'STALE':esc(text(t.position_quality||t.quality))}</span>`;
    const editor=document.createElement('div'),input=document.createElement('input'),apply=document.createElement('button'),automatic=document.createElement('button');editor.className='line-editor';input.value=lineName;input.setAttribute('aria-label',`${title(t)} 线路`);apply.type='button';apply.textContent='修改';automatic.type='button';automatic.textContent='自动';[editor,input,apply,automatic].forEach(el=>el.addEventListener('click',event=>event.stopPropagation()));apply.onclick=()=>{const value=input.value.trim();if(!value)return;lineOverrides[uid]=value;localStorage.setItem('railwayLineOverrides',JSON.stringify(lineOverrides));osmEstimates.delete(uid);osmPending.delete(uid);hasFitted=false;render(lastTrains)};automatic.onclick=()=>{delete lineOverrides[uid];localStorage.setItem('railwayLineOverrides',JSON.stringify(lineOverrides));osmEstimates.delete(uid);osmPending.delete(uid);hasFitted=false;render(lastTrains)};editor.append(input,apply,automatic);card.appendChild(editor);listEl.appendChild(card);
    if(!base&&lineName&&t.kilometer_km!=null)requestOsm(t);
    if(map&&base){const shown=mapCoordinate(base),pos=[shown[1],shown[0]];bounds.push(pos);let marker=markers.get(uid);if(!marker){marker=L.marker(pos).addTo(map);markers.set(uid,marker)}else marker.setLatLng(pos);const markerElement=marker.getElement();if(markerElement)markerElement.classList.toggle('mileage-interpolated',(source||'').includes('INTERPOLATED'));marker.bindPopup(popup(t,base,shown,lineName));card.onclick=()=>{map.setView(pos,14);marker.openPopup()};const points=(t.track||[]).map(p=>{const b=trackCoordinate(p);if(!b)return null;const c=mapCoordinate(b);return[c[1],c[0]]}).filter(Boolean);let line=tracks.get(uid);if(!line){line=L.polyline(points,{color:'#39a0ff',weight:3,opacity:.75}).addTo(map);tracks.set(uid,line)}else line.setLatLngs(points);renderComparison(t,uid)}else if(map&&markers.has(uid)){map.removeLayer(markers.get(uid));markers.delete(uid);if(tracks.has(uid)){map.removeLayer(tracks.get(uid));tracks.delete(uid)}clearComparison(uid)}
  });
  [...markers.keys()].forEach(id=>{if(!liveIds.has(id)){map.removeLayer(markers.get(id));markers.delete(id)}});[...tracks.keys()].forEach(id=>{if(!liveIds.has(id)){map.removeLayer(tracks.get(id));tracks.delete(id)}});[...comparisonLayers.keys()].forEach(id=>{if(!liveIds.has(id))clearComparison(id)});if(map&&!hasFitted&&bounds.length){map.fitBounds(bounds,{padding:[40,40],maxZoom:13});hasFitted=true}if(!trains.length)listEl.innerHTML='<article class="train"><h2>暂无有效铁路目标</h2><p>合法 BASIC 或 EXT 到达后会立即显示。</p></article>';
}
function loadSettings(){try{settings={...defaults,...JSON.parse(localStorage.getItem('railwayCoordinateSettings')||'{}')}}catch(_){settings={...defaults}}try{lineOverrides=JSON.parse(localStorage.getItem('railwayLineOverrides')||'{}')||{}}catch(_){lineOverrides={}}rawFormatEl.value=settings.rawFormat;sourceDatumEl.value=settings.sourceDatum;mapTransformEl.value=settings.mapTransform;positionPriorityEl.value=settings.positionPriority;comparePositionsEl.checked=!!settings.comparePositions;showAnchorsEl.checked=!!settings.showAnchors;defaultLineEl.value=settings.defaultLine||'沪昆线';const searchLine=document.getElementById('search-line');if(!searchLine.value)searchLine.value=defaultLineEl.value}
function settingsChanged(){settings={rawFormat:rawFormatEl.value,sourceDatum:sourceDatumEl.value,mapTransform:mapTransformEl.value,positionPriority:positionPriorityEl.value,comparePositions:comparePositionsEl.checked,showAnchors:showAnchorsEl.checked,defaultLine:settings.defaultLine||'沪昆线'};localStorage.setItem('railwayCoordinateSettings',JSON.stringify(settings));hasFitted=false;render(lastTrains);renderAnchorLayer()}
[rawFormatEl,sourceDatumEl,mapTransformEl,positionPriorityEl,comparePositionsEl,showAnchorsEl].forEach(el=>el.addEventListener('change',settingsChanged));
document.getElementById('apply-default-line').addEventListener('click',()=>{settings.defaultLine=defaultLineEl.value.trim()||'沪昆线';defaultLineEl.value=settings.defaultLine;document.getElementById('search-line').value=settings.defaultLine;localStorage.setItem('railwayCoordinateSettings',JSON.stringify(settings));osmEstimates.clear();osmPending.clear();hasFitted=false;render(lastTrains)});
document.getElementById('restore-defaults').addEventListener('click',()=>{settings={...defaults};lineOverrides={};localStorage.removeItem('railwayCoordinateSettings');localStorage.removeItem('railwayLineOverrides');rawFormatEl.value=settings.rawFormat;sourceDatumEl.value=settings.sourceDatum;mapTransformEl.value=settings.mapTransform;positionPriorityEl.value=settings.positionPriority;comparePositionsEl.checked=settings.comparePositions;showAnchorsEl.checked=settings.showAnchors;defaultLineEl.value=settings.defaultLine;document.getElementById('search-line').value=settings.defaultLine;osmEstimates.clear();osmPending.clear();hasFitted=false;render(lastTrains);renderAnchorLayer()});
learningEnabledEl.addEventListener('change',async()=>{await fetch('/api/mileage/learning',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:learningEnabledEl.checked})});refreshMileageSummary()});
document.getElementById('search-position').addEventListener('click',async()=>{const line=document.getElementById('search-line').value.trim(),kmText=document.getElementById('search-km').value.trim().replace(/^K/i,''),km=Number(kmText);if(!line||!Number.isFinite(km)){mileageSearchResultEl.textContent='请输入线路名和合法公里标';return}mileageSearchResultEl.textContent='正在查询本地数据库 / OSM…';const query=async()=>{const result=await(await fetch(`/api/mileage/lookup?line=${encodeURIComponent(line)}&km=${encodeURIComponent(km)}`,{cache:'no-store'})).json();if(result.pending){setTimeout(query,3000);return}if(!result.valid){mileageSearchResultEl.textContent=`无法定位：${result.reason}`;return}mileageSearchResultEl.textContent=`${sourceLabel(result.source)}：${Number(result.longitude).toFixed(6)}, ${Number(result.latitude).toFixed(6)} / 置信度 ${Number(result.confidence).toFixed(2)}`;if(map){map.setView([Number(result.latitude),Number(result.longitude)],15);L.popup().setLatLng([Number(result.latitude),Number(result.longitude)]).setContent(`${esc(line)} K${km.toFixed(1)}<br>${esc(sourceLabel(result.source))}`).openOn(map)}};query()});
document.getElementById('export-mileage').addEventListener('click',()=>{location.href='/api/mileage/export'});
const importFileEl=document.getElementById('import-mileage-file');document.getElementById('import-mileage').addEventListener('click',()=>importFileEl.click());importFileEl.addEventListener('change',async()=>{const file=importFileEl.files[0];if(!file)return;const response=await fetch('/api/mileage/import',{method:'POST',headers:{'Content-Type':'application/json'},body:await file.text()});mileageSearchResultEl.textContent=response.ok?'数据库导入成功':'数据库导入失败';refreshMileageSummary()});
document.getElementById('clear-mileage').addEventListener('click',async()=>{if(!confirm('确定清空全部公里标学习数据？此操作不可撤销。'))return;await fetch('/api/mileage/clear',{method:'POST'});mileageSearchResultEl.textContent='学习数据已清空';refreshMileageSummary()});
function replaceSnapshot(trains){targetsByUid.clear();trains.forEach(t=>targetsByUid.set(uidOf(t),t));render([...targetsByUid.values()])}
function updateTarget(target){targetsByUid.set(uidOf(target),target);render([...targetsByUid.values()])}
function removeTarget(uid){uid=String(uid);targetsByUid.delete(uid);if(map&&markers.has(uid)){map.removeLayer(markers.get(uid));markers.delete(uid)}if(map&&tracks.has(uid)){map.removeLayer(tracks.get(uid));tracks.delete(uid)}render([...targetsByUid.values()])}
async function refresh(){try{const r=await fetch('/api/trains',{cache:'no-store'});replaceSnapshot(await r.json())}catch(e){connectionEl.textContent='本机数据接口暂不可用'}}
function connect(){const ws=new WebSocket(`ws://${location.host}/ws`);ws.onopen=()=>{connectionEl.textContent='实时连接已建立'};ws.onmessage=e=>{try{const m=JSON.parse(e.data);if(m.type==='snapshot')replaceSnapshot(m.trains);else if(m.type==='target_update')updateTarget(m.target);else if(m.type==='target_remove')removeTarget(m.target_uid)}catch(_){}};ws.onclose=()=>{connectionEl.textContent='实时连接中断，正在重连';setTimeout(connect,1500)};ws.onerror=()=>ws.close()}
loadSettings();initMap();refresh();refreshMileageSummary();connect();setInterval(refresh,5000);setInterval(refreshMileageSummary,15000);
